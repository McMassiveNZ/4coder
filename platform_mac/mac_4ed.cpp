/*
 * Mr. 4th Dimention - Allen Webster
 *
 * 28.06.2017
 *
 * Mac C++ layer for 4coder
 *
 */

// TOP

#define IS_PLAT_LAYER

#include "4coder_base_types.h"
#include "4coder_API/4coder_version.h"

#include "4coder_lib/4coder_utf8.h"

#if defined(FRED_SUPER)
# include "4coder_lib/4coder_arena.h"
# include "4coder_lib/4coder_arena.cpp"
# define FSTRING_IMPLEMENTATION
# include "4coder_lib/4coder_string.h"

# include "4coder_API/4coder_keycodes.h"
# include "4coder_API/4coder_style.h"
# include "4coder_API/4coder_types.h"

#else
# include "4coder_default_bindings.cpp"
#endif

#include "osx_objective_c_to_cpp_links.h"

#include "4ed_math.h"

#include <string.h>

#include "4ed_font.h"
#include "4ed_system.h"
#include "4ed_log.h"
#include "4ed_render_target.h"
#include "4ed_render_format.h"
#include "4ed.h"
#include "4ed_linked_node_macros.h"

#include "4ed_file_track.h"
#include "4ed_system_shared.h"

#include "unix_4ed_headers.h"
#include <sys/syslimits.h>

#undef external
#undef internal
#include <mach/mach.h>
#define external extern "C"
#define internal static
#include <mach-o/dyld.h>

#include <stdlib.h>

////////////////////////////////

#include "4ed_shared_thread_constants.h"
#include "unix_threading_wrapper.h"
#include "mac_semaphore_wrapper.h"

// TODO(allen): Make an intrinsics header that uses the cracked OS to define a single set of intrinsic names.
#define InterlockedCompareExchange(dest, ex, comp) \
__sync_val_compare_and_swap((dest), (comp), (ex))

////////////////////////////////

#define SLASH '/'
#define DLL "so"

global System_Functions sysfunc;
#include "4ed_shared_library_constants.h"
#include "unix_library_wrapper.h"

#include "4ed_standard_libraries.cpp"
#include "4ed_coroutine.cpp"
#include "4ed_font.cpp"

////////////////////////////////

struct OSX_Vars{
    Application_Step_Input input;
    String clipboard_contents;
    b32 keep_running;
    
    b32 has_prev_time;
    u64 prev_time_u;
    
    File_Track_System track;
    void *track_table;
    u32 track_table_size;
    u32 track_node_size;
};

////////////////////////////////

OSX_Objective_C_Vars osx_objc;
OSX_Vars osxvars;
global Render_Target target;
global Application_Memory memory_vars;
global Plat_Settings plat_settings;

global Libraries libraries;
global App_Functions app;
global Custom_API custom_api;

global Coroutine_System_Auto_Alloc coroutines;

////////////////////////////////

#include "mac_error_box.cpp"

////////////////////////////////

internal
Sys_Get_4ed_Path_Sig(system_get_4ed_path){
    Partition *scratch = &shared_vars.scratch;
    Temp_Memory temp = begin_temp_memory(scratch);
    char *temp_buffer = push_array(scratch, char, capacity);
    i32 size = 0;
    u32 buf_size = capacity;
    i32 status = _NSGetExecutablePath(temp_buffer, &buf_size);
    buf_size = str_size(temp_buffer);
    if (status == 0){
        ssize_t ln_len = readlink(temp_buffer, out, capacity);
        if (ln_len != -1){
            out[ln_len] = 0;
            String str = make_string_cap(out, ln_len, capacity);
            remove_last_folder(&str);
            terminate_with_null(&str);
            size = str.size;
        }
        else{
            memcpy(out, temp_buffer, buf_size);
            String str = make_string_cap(out, buf_size, capacity);
            remove_last_folder(&str);
            terminate_with_null(&str);
            size = str.size;
        }
    }
    end_temp_memory(temp);
    return(size);
}

//#include "mac_fd_check.cpp"
#include "unix_4ed_functions.cpp"

internal
Sys_Now_Time_Sig(system_now_time){
    f64 t = osx_timer_seconds();
    u64 result = (u64)(t*1000000.f);
    return(result);
}

#include "4ed_shared_file_handling.cpp"

////////////////////////////////

void
osx_log(char *m, i32 l){
    system_log(m, l);
}

////////////////////////////////

internal void
system_schedule_step(void){
    osx_schedule_step();
}

////////////////////////////////

#include "4ed_work_queues.cpp"

////////////////////////////////

internal
Sys_Show_Mouse_Cursor_Sig(system_show_mouse_cursor){
    switch (show){
        case MouseCursorShow_Never:
        {
            osx_show_cursor(-1, 0);
        }break;
        
        case MouseCursorShow_Always:
        {
            osx_show_cursor(1, 0);
        }break;
    }
}

internal
Sys_Set_Fullscreen_Sig(system_set_fullscreen){
    osx_objc.do_toggle = (osx_objc.full_screen != full_screen);
    return(true);
}

internal
Sys_Is_Fullscreen_Sig(system_is_fullscreen){
    b32 result = (osx_objc.full_screen != osx_objc.do_toggle);
    return(result);
}

#include "4ed_coroutine_functions.cpp"

#include "4ed_system_shared.cpp"

// File Change Listeners

internal b32
handle_track_out_of_memory(i32 val){
    b32 result = false;
    
    switch (val){
        case FileTrack_OutOfTableMemory:
        {
            u32 new_table_size = osxvars.track_table_size*2;
            void *new_table = system_memory_allocate(new_table_size);
            move_track_system(&osxvars.track, &shared_vars.scratch, new_table, new_table_size);
            system_memory_free(osxvars.track_table, osxvars.track_table_size);
            osxvars.track_table_size = new_table_size;
            osxvars.track_table = new_table;
        }break;
        
        case FileTrack_OutOfListenerMemory:
        {
            osxvars.track_node_size *= 2;
            void *node_expansion = system_memory_allocate(osxvars.track_node_size);
            expand_track_system_listeners(&osxvars.track, &shared_vars.scratch, node_expansion, osxvars.track_node_size);
        }break;
        
        default: result = true; break;
    }
    
    return(result);
}

internal
Sys_Add_Listener_Sig(system_add_listener){
    b32 result = false;
    for (;;){
        i32 track_result = add_listener(&osxvars.track, &shared_vars.scratch, (u8*)filename);
        if (handle_track_out_of_memory(track_result)){
            if (track_result == FileTrack_Good){
                result = true;
            }
            break;
        }
    }
    return(result);
}

internal
Sys_Remove_Listener_Sig(system_remove_listener){
    b32 result = false;
    i32 track_result = remove_listener(&osxvars.track, &shared_vars.scratch, (u8*)filename);
    if (track_result == FileTrack_Good){
        result = true;
    }
    return(result);
}

internal
Sys_Get_File_Change_Sig(system_get_file_change){
    b32 result = false;
    
    i32 size = 0;
    i32 get_result = get_change_event(&osxvars.track, &shared_vars.scratch, (u8*)buffer, max, &size);
    
    *required_size = size;
    *mem_too_small = false;
    if (get_result == FileTrack_Good){
        result = true;
    }
    else if (get_result == FileTrack_MemoryTooSmall){
        *mem_too_small = true;
        result = true;
    }
    
    return(result);
}


//
// Clipboard
//

internal
Sys_Post_Clipboard_Sig(system_post_clipboard){
    char *string = str.str;
    if (!terminate_with_null(&str)){
        if (osx_objc.clipboard_space_max <= str.size + 1){
            if (osx_objc.clipboard_space != 0){
                system_memory_free(osx_objc.clipboard_space, osx_objc.clipboard_space_max);
            }
            osx_objc.clipboard_space_max = l_round_up_u32(str.size*2 + 1, KB(4096));
            osx_objc.clipboard_space = (char*)system_memory_allocate(osx_objc.clipboard_space_max);
        }
        memcpy(osx_objc.clipboard_space, str.str, str.size);
        osx_objc.clipboard_space[str.size] = 0;
        string = osx_objc.clipboard_space;
    }
    osx_post_to_clipboard(string);
}

//
// CLI
//

// HACK(allen): ALMOST an exact duplicate from the Linux version.  Just epoll doesn't port.  deduplicate or switch to NSTask.
global i32 cli_count = 0;

internal
Sys_CLI_Call_Sig(system_cli_call, path, script_name, cli_out){
    i32 pipe_fds[2];
    if (pipe(pipe_fds) == -1){
        
        return 0;
    }
    
    i32 child_pid = fork();
    if (child_pid == -1){
        
        return 0;
    }
    
    enum { PIPE_FD_READ, PIPE_FD_WRITE };
    
    // child
    if (child_pid == 0){
        close(pipe_fds[PIPE_FD_READ]);
        dup2(pipe_fds[PIPE_FD_WRITE], STDOUT_FILENO);
        dup2(pipe_fds[PIPE_FD_WRITE], STDERR_FILENO);
        
        if (chdir(path) == -1){
            
            exit(1);
        }
        
        char* argv[] = {
            "sh",
            "-c",
            script_name,
            0
        };
        
        if (execv("/bin/sh", argv) == -1){
            
        }
        exit(1);
    }
    else{
        close(pipe_fds[PIPE_FD_WRITE]);
        
        *(pid_t*)&cli_out->proc = child_pid;
        *(int*)&cli_out->out_read = pipe_fds[PIPE_FD_READ];
        *(int*)&cli_out->out_write = pipe_fds[PIPE_FD_WRITE];
        
        ++cli_count;
    }
    
    return(true);
}

internal
Sys_CLI_Begin_Update_Sig(system_cli_begin_update){
    // NOTE(inso): I don't think anything needs to be done here.
}

internal
Sys_CLI_Update_Step_Sig(system_cli_update_step){
    i32 pipe_read_fd = *(i32*)&cli->out_read;
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(pipe_read_fd, &fds);
    
    struct timeval tv = {};
    
    size_t space_left = max;
    char* ptr = dest;
    
    while (space_left > 0 && select(pipe_read_fd + 1, &fds, NULL, NULL, &tv) == 1){
        ssize_t num = read(pipe_read_fd, ptr, space_left);
        if (num == -1){
            
        } else if (num == 0){
            // NOTE(inso): EOF
            break;
        } else {
            ptr += num;
            space_left -= num;
        }
    }
    
    *amount = (ptr - dest);
    return((ptr - dest) > 0);
}

internal
Sys_CLI_End_Update_Sig(system_cli_end_update){
    pid_t pid = *(pid_t*)&cli->proc;
    b32 close_me = false;
    
    int status;
    if (pid && waitpid(pid, &status, WNOHANG) > 0){
        close_me = true;
        
        cli->exit = WEXITSTATUS(status);
        
        close(*(int*)&cli->out_read);
        close(*(int*)&cli->out_write);
        
        --cli_count;
    }
    
    return(close_me);
}

#include "4ed_font_provider_freetype.h"
global u32 system_font_method = SystemFontMethod_FilePath;
#include "4ed_font_provider_freetype.cpp"

internal
Sys_Font_Path(name, parameters){
    b32 italic = (parameters != 0 && parameters->italics);
    b32 bold = (parameters != 0 && parameters->bold);
    i32 pt_size = 12;
    if (parameters != 0){
        pt_size = parameters->pt_size;
    }
    
    OSX_Font_Match match = osx_get_font_match(name, pt_size, italic, bold);
    
    Font_Path path = {};
    Partition *part = &shared_vars.font_scratch;
    path.temp = begin_temp_memory(part);
    
    if (match.path != 0){
        i32 len = str_size(match.path);
        char *buffer = push_array(part, char, len + 1);
        if (buffer == 0){
            sysshared_partition_grow(part, l_round_up_i32(len + 1, KB(4)));
            buffer = push_array(part, char, len + 1);
        }
        
        if (buffer != 0){
            push_align(part, 8);
            memcpy(buffer, match.path, len + 1);
            path.len = len;
            path.name = buffer;
        }
    }
    
    return(path);
}

Sys_Font_Data_Not_Used;

internal void
osx_get_loadable_fonts(Partition *part, Font_Setup_List *list){
    OSX_Loadable_Fonts fonts = osx_list_loadable_fonts();
    for (i32 i = 0; i < fonts.count; ++i){
        char *name = fonts.names[i];
        char *path = fonts.paths[i];
        
        if (name == 0 || path == 0){
            continue;
        }
        
        Temp_Memory reset= begin_temp_memory(part);
        Font_Setup *setup = push_array(part, Font_Setup, 1);
        
        if (setup != 0){
            memset(setup, 0, sizeof(*setup));
            
            i32 len = str_size(path);
            if (len < sizeof(setup->stub.name)){
                i32 name_len = str_size(name);
                if (name_len < sizeof(setup->name)){
                    setup->stub.load_from_path = true;
                    memcpy(setup->stub.name, path, len + 1);
                    setup->stub.len = len;
                    setup->has_display_name = true;
                    setup->len = name_len;
                    memcpy(setup->name, name, name_len + 1);
                    sll_push(list->first, list->last, setup);
                }
                else{
                    end_temp_memory(reset);
                }
            }
            else{
                end_temp_memory(reset);
            }
        }
    }
    
    free(fonts.names);
}

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include "opengl/4ed_opengl_render.cpp"

////////////////////////////////

#include "4ed_link_system_functions.cpp"
#include "4ed_shared_init_logic.cpp"

external void*
osx_allocate(umem size){
    void *result = system_memory_allocate(size);
    return(result);
}

external void
osx_free(void *ptr, umem size){
    system_memory_free(ptr, size);
}

external void
osx_resize(int width, int height){
    if (width > 0 && height > 0){
        osx_objc.width = width;
        osx_objc.height = height;
        
        target.width = width;
        target.height = height;
        
        osx_schedule_step();
    }
}

internal void
osx_push_key(Key_Code code, Key_Code chr, Key_Code chr_nocaps, b8 *mods){
    i32 count = osxvars.input.keys.count;
    
    if (count < KEY_INPUT_BUFFER_SIZE){
        Key_Event_Data *data = osxvars.input.keys.keys;
        
        data[count].keycode = code;
        data[count].character = chr;
        data[count].character_no_caps_lock = chr_nocaps;
        
        memcpy(data[count].modifiers, mods, sizeof(*mods)*MDFR_INDEX_COUNT);
        
        osxvars.input.keys.count = count + 1;
    }
}

internal void
osx_mods_struct_to_array(OSX_Keyboard_Modifiers flags, b8 *mods){
    mods[MDFR_SHIFT_INDEX]   = ((flags.shift) != 0);
    mods[MDFR_CONTROL_INDEX] = ((flags.control) != 0);
    mods[MDFR_ALT_INDEX]     = ((flags.option) != 0);
    mods[MDFR_COMMAND_INDEX] = ((flags.command) != 0);
    mods[MDFR_CAPS_INDEX]    = ((flags.caps) != 0);
}

external void
osx_character_input(u32 code, OSX_Keyboard_Modifiers modifier_flags){
    Key_Code c = 0;
    switch (code){
        // TODO(allen): Find the canonical list of these things.
        case 0x007F: c = key_back; break;
        case 0xF700: c = key_up; break;
        case 0xF701: c = key_down; break;
        case 0xF702: c = key_left; break;
        case 0xF703: c = key_right; break;
        case 0xF728: c = key_del; break;
        case 0xF729: c = key_home; break;
        case 0xF72B: c = key_end; break;
        case 0xF72C: c = key_page_up; break;
        case 0xF72D: c = key_page_down; break;
        case 0x001B: c = key_esc; break;
        
        case 0xF704: c = key_f1; break;
        case 0xF705: c = key_f2; break;
        case 0xF706: c = key_f3; break;
        case 0xF707: c = key_f4; break;
        
        case 0xF708: c = key_f5; break;
        case 0xF709: c = key_f6; break;
        case 0xF70A: c = key_f7; break;
        case 0xF70B: c = key_f8; break;
        
        case 0xF70C: c = key_f9; break;
        case 0xF70D: c = key_f10; break;
        case 0xF70E: c = key_f11; break;
        case 0xF70F: c = key_f12; break;
        
        case 0xF710: c = key_f13; break;
        case 0xF711: c = key_f14; break;
        case 0xF712: c = key_f15; break;
        case 0xF713: c = key_f16; break;
    }
    
    b8 mods[MDFR_INDEX_COUNT] = {};
    osx_mods_struct_to_array(modifier_flags, mods);
    
    if (c != 0){
        osx_push_key(c, 0, 0, mods);
    }
    else if (code != 0){
        if (code < 0xE000 || code > 0xF8FF){
            if (code == '\r'){
                code = '\n';
            }
            Key_Code chr = code;
            Key_Code nocaps = code;
            if (modifier_flags.caps){
                if ('a' <= nocaps && nocaps <= 'z'){
                    chr += 'A' - 'a';
                }
                else if ('A' <= nocaps && nocaps <= 'Z'){
                    chr += 'a' - 'A';
                }
            }
            osx_push_key(code, chr, nocaps, mods);
        }
        else{
            fprintf(stdout, "unhandled private code %x\n", code);
        }
    }
    else{
        osx_push_key(0, 0, 0, mods);
    }
    
    osx_schedule_step();
}

external void
osx_mouse(i32 mx, i32 my, u32 type){
    i32 new_x = mx;
    i32 new_y = osx_objc.height - my;
    if (new_x != osxvars.input.mouse.x || new_y != osxvars.input.mouse.y){
        osxvars.input.mouse.x = new_x;
        osxvars.input.mouse.y = new_y;
        osx_schedule_step();
    }
    
    if (type == MouseType_Press){
        osxvars.input.mouse.press_l = true;
        osxvars.input.mouse.l = true;
        osx_schedule_step();
    }
    if (type == MouseType_Release){
        osxvars.input.mouse.release_l = true;
        osxvars.input.mouse.l = false;
        osx_schedule_step();
    }
}

external void
osx_mouse_wheel(float dx, float dy){
    osxvars.input.mouse.wheel = - (int32_t)(dy);
    osx_schedule_step();
}

external void
osx_try_to_close(void){
    osxvars.keep_running = false;
    osx_schedule_step();
}

external void
osx_step(void){
    
    Application_Step_Result result = {};
    
    // NOTE(allen): Prepare the Frame Input
    osxvars.input.dt = 1.f/60.f;
    if (osxvars.has_prev_time){
        u64 time_u = system_now_time();
        u64 time_elapsed_u = time_u - osxvars.prev_time_u;
        osxvars.input.dt = time_elapsed_u/1000000.f;
        osxvars.prev_time_u = time_u;
    }
    else{
        osxvars.has_prev_time = true;
        osxvars.prev_time_u = system_now_time();
    }
    
    // TODO(allen): CROSS REFERENCE WITH WINDOWS SPECIAL CODE "TIC898989"
    Application_Step_Input frame_input = osxvars.input;
    frame_input.trying_to_kill = !osxvars.keep_running;
    OSX_Keyboard_Modifiers mods = osx_get_modifiers();
    osx_mods_struct_to_array(mods, frame_input.keys.modifiers);
    
    osxvars.input.first_step = false;
    memset(&osxvars.input.keys, 0, sizeof(osxvars.input.keys));
    osxvars.input.mouse.press_l = false;
    osxvars.input.mouse.release_l = false;
    osxvars.input.mouse.press_r = false;
    osxvars.input.mouse.release_r = false;
    osxvars.input.mouse.wheel = 0;
    
    
    // NOTE(allen): Frame Clipboard Input
    if (osx_objc.has_clipboard_item){
        frame_input.clipboard = make_string(osx_objc.clipboard_data, (i32)osx_objc.clipboard_size);
    }
    else{
        frame_input.clipboard = null_string;
    }
    
    // HACK(allen): Got this all messed up with respect to how everyone else (other OS layers) work
    osx_objc.do_toggle = false;
    osx_objc.full_screen = osx_is_fullscreen();
    
    // HACK(allen): THIS SHIT IS FUCKED (happens on linux too)
    b32 keep_running = osxvars.keep_running;
    
    // NOTE(allen): Application Core Update
    target.buffer.pos = 0;
    if (app.step != 0){
        result = app.step(&sysfunc, &target, &memory_vars, &frame_input);
    }
    else{
        LOG("app.step == 0 -- skipping\n");
    }
    
    // NOTE(allen): Finish the Loop
    if (result.perform_kill){
        osx_close_app();
    }
    else if (!keep_running && !osxvars.keep_running){
        osxvars.keep_running = true;
    }
    
    // NOTE(allen): Switch to New Title
    if (result.has_new_title){
        osx_change_title(result.title_string);
    }
    
    // NOTE(allen): Switch to New Cursor
    osx_show_cursor(0, result.mouse_cursor_type);
    
    // NOTE(allen): Render
    osx_begin_render();
    interpret_render_buffer(&target, &shared_vars.pixel_scratch);
    osx_end_render();
    
    // NOTE(allen): Toggle Full Screen
    if (osx_objc.do_toggle){
        osx_toggle_fullscreen();
    }
    
    // NOTE(allen): Schedule Another Step if Needed
    if (result.animating || cli_count > 0){
        osx_schedule_step();
    }
}

external void
osx_init(){
    //
    // System Linkage
    //
    
    link_system_code();
    
    //
    // Memory init
    //
    
    memset(&target, 0, sizeof(target));
    memset(&memory_vars, 0, sizeof(memory_vars));
    memset(&plat_settings, 0, sizeof(plat_settings));
    
    memset(&libraries, 0, sizeof(libraries));
    memset(&app, 0, sizeof(app));
    memset(&custom_api, 0, sizeof(custom_api));
    
    memory_init();
    
    osxvars.keep_running = true;
    osxvars.input.first_step = true;
    
    //
    // HACK(allen): 
    // Previously zipped stuff is here, it should be zipped in the new pattern now.
    //
    
    init_shared_vars();
    
    osxvars.track_table_size = KB(16);
    osxvars.track_table = system_memory_allocate(osxvars.track_table_size);
    
    osxvars.track_node_size = KB(16);
    void *track_nodes = system_memory_allocate(osxvars.track_node_size);
    
    i32 track_result = init_track_system(&osxvars.track, &shared_vars.scratch,
                                         osxvars.track_table, osxvars.track_table_size,
                                         track_nodes, osxvars.track_node_size);
    
    if (track_result != FileTrack_Good){
        exit(1);
    }
    
    //
    // Load Core Code
    //
    load_app_code();
    
    //
    // Read command line
    //
    read_command_line(osx_objc.argc, osx_objc.argv);
    
    //
    // Load Custom Code
    //
#if defined(FRED_SUPER)
    load_custom_code();
#else
    custom_api.get_bindings = get_bindings;
#endif
    
    //
    // Threads
    //
    
    work_system_init();
    
    //
    // Coroutines
    //
    
    coroutines_init();
    
    //
    // Font System Init
    //
    
    Partition *scratch = &shared_vars.scratch;
    Temp_Memory temp = begin_temp_memory(scratch);
    Font_Setup_List font_setup = system_font_get_local_stubs(scratch);
    osx_get_loadable_fonts(scratch, &font_setup);
    system_font_init(&sysfunc.font, plat_settings.font_size, plat_settings.use_hinting, font_setup);
    end_temp_memory(temp);
    
    //
    // App Init
    //
    
    char cwd[4096];
    u32 size = sysfunc.get_current_path(cwd, sizeof(cwd));
    if (size == 0 || size >= sizeof(cwd)){
        system_error_box("Could not get current directory at launch.");
    }
    String curdir = make_string(cwd, size);
    terminate_with_null(&curdir);
    replace_char(&curdir, '\\', '/');
    
    
    
    String clipboard_string = {};
    if (osx_objc.has_clipboard_item){
        clipboard_string = make_string(osx_objc.clipboard_data, osx_objc.clipboard_size);
    }
    
    LOG("Initializing application variables\n");
    app.init(&sysfunc, &target, &memory_vars, clipboard_string, curdir, custom_api);
}

#include "mac_4ed_file_track.cpp"

// BOTTOM

