/*
 * Uae4all libretro core input implementation
 * SF2K-UAE v078
 * (c) Chips 2021, Grzegorz Korycki 2024
 * v078: DEBUG - return at start of save_state() to find crash
 * v077: Save/Load State using RPATH (same as config) - FIXED path bug
 * v076: TEST - Save 1 byte using SAME fs_* as config (RPATH + .asf)
 * v075: TEST - Save State without RAM (CRAM/BRAM/FRAM/ZRAM skipped)
 * v074: Save/Load State using firmware fs_* functions (internal save system)
 * v073: Remove Fast RAM, expand Slow RAM to 1.5MB, A=RMB/B=LMB in mouse mode, scroll arrow
 * v072: Fast RAM (1-4MB) implementation at 0x200000 (Zorro II area) - REMOVED
 * v071: Slow RAM (512KB) implementation - replaces non-functional Fast RAM option
 * v070: Mouse Speed 1-8, Delete Config, Settings scroll, START exits Settings
 * v069: Mouse Speed option (1-5, default 2), saved per-game
 * v068: Fix mouse from menu - add second_joystick_enable to sf2000_apply_settings()
 * v067: Per-game config (gamename.cfg), kickstart override after default_prefs
 * v066: Per-game config attempt (BROKEN - romfile cleared by default_prefs)
 * v061: Direct fs_* firmware calls (bypass broken stdio), root directory config
 * v060b: Global config in root /mnt/sda1/ (like FrogUI game_history.txt), removed fs_sync
 * v059: Fix config save - mkdir + correct path (/mnt/sda1/cores/config/) + fs_sync()
 * v058: Menu redesign - About, Settings with Kickstart/RAM selection, per-game config
 * v057: Fix Disk Shuffler - detect 5+ disks, reset disabled mask on shuffle
 * v056: Disk Shuffler moved to first menu position for easier access
 * v055: Disk Shuffler - rotate disks, disable unused drives to save chip RAM
 * v054: L+R hold 3sec for mouse, FrogJoy2 mouse fix, better menu labels
 * v053: Fix mouse - add L+R toggle for mouse mode (like v007), remove broken save states
 * v042: Fix frameskip autofire - cache input state per retro_run frame
 * v040: Fix autofire - getjoystate nr convention was SWAPPED (nr=0 is Port 1!)
 * v038: A+B both work as fire
 * v037: 2MB CHIP RAM, fix 2nd controller (Data Frog) input
 */

// v042: Cached input state - sampled once per retro_run, stable for all vsync_handlers
static struct {
    int up, down, left, right;
    int fire;  // Combined A|B
    int valid;  // 1 if cache is valid this frame
} g_cached_joy0, g_cached_joy1;

#include "libretro.h"
#include "libretro-core.h"
#include "retroscreen.h"
#include "graph.h"

#include "sf2000_diag.h"

#include "uae.h"
#include "sysconfig.h"
#include "sysdeps.h"
#include "config.h"
#include "options.h"
#include "savestate.h"
#include "disk.h"  // v055: Disk shuffler

// v051: Use savestate globals instead of direct function calls
extern int savestate_state;
extern char *savestate_filename;

//FIXME
extern int uae4all_keystate[256];
extern void changedisk( bool );
extern char uae4all_image_file[256];

// v058: Kickstart ROM management (externs - variables defined elsewhere)
extern char romfile[64];
extern const char *retro_system_directory;
extern const char *retro_save_directory;

// v067: Path to current ROM file (for per-game config)
extern char RPATH[512];

// v067: Flag - was config loaded with non-default kickstart?
static int sf2000_config_loaded = 0;

// v061: Direct firmware filesystem functions (bypass core's broken stdio)
// These are linked via bisrv_08_03-core.ld linker script
extern "C" int fs_open(const char *path, int flags, int perms);
extern "C" ssize_t fs_write(int fd, const void *buf, size_t count);
extern "C" ssize_t fs_read(int fd, void *buf, size_t count);
extern "C" int fs_close(int fd);
extern "C" int fs_sync(const char *path);

// Firmware file flags (from stockfw.h)
#define FS_O_RDONLY 0x0000
#define FS_O_WRONLY 0x0001
#define FS_O_RDWR   0x0002
#define FS_O_CREAT  0x0100
#define FS_O_TRUNC  0x0200

//TIME
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>


extern void virtual_kdb (char *buffer,int vx,int vy);
extern int  check_vkey2 (int x,int y);

//VIDEO
extern char *gfx_mem;

//EMU FLAGS
int NPAGE=-1, KCOL=1, BKGCOLOR=0;
int SHOWKEY=-1;

int MOUSE_EMULATED=-1;

int SHIFTON=-1,PAS=6;  // v032: increased from 3 to 6 for better mouse responsiveness
int pauseg=0; //enter_gui

// SF2000 OPTIONS MENU - v058: Redesigned menu
int sf2000_menu_active = 0;
int sf2000_menu_item = 0;
int sf2000_menu_scroll = 0;  // v036: scroll offset for menu
int sf2000_disk_shuffler_active = 0;  // v055: Disk Shuffler submenu
int sf2000_settings_active = 0;  // v058: Settings submenu (replaces Details)
int sf2000_about_active = 0;  // v058: About submenu
int sf2000_settings_item = 0;  // v058: Selected item in Settings submenu
#define SF2000_MENU_ITEMS 12  // v069: Disks,FJ1,FJ2,MouseSpd,Skip,Sound,CPU,Y,Floppy,Settings,About,EXIT
#define SF2000_MENU_VISIBLE 8  // v036: max visible items at once

// v035: Auto CPU fix - open menu after 3 seconds, toggle CPU, close
static int sf2000_frame_counter = 0;
static int sf2000_auto_fix_done = 0;
static int sf2000_auto_fix_state = 0;  // 0=waiting, 1=menu opened, 2=cpu changed, 3=done
#define SF2000_AUTO_FIX_FRAMES (60 * 3)  // 60 fps * 3 sec = 3 seconds
int sf2000_frameskip = 2;
int sf2000_sound_mode = 1;
int sf2000_cpu_timing = 2;
// v034: FrogJoy system - 0=Port1 Joy (P1), 1=Port0 Joy (P2), 2=Port0 Mouse
int sf2000_frogjoy1 = 0;  // Main controller -> Port1 Joy (P1) by default
int sf2000_frogjoy2 = 1;  // Second controller -> Port0 Joy (P2) by default
int sf2000_y_offset = 0;
int sf2000_v_stretch = 0;
int sf2000_turbo_floppy = 0;
// v070: Mouse Speed (index 0-7 = speed 1-8, default index 1 = speed 2)
int sf2000_mouse_speed = 1;  // Default: speed 2/8
static const int mouse_speed_table[8] = { 3, 6, 9, 12, 15, 20, 28, 40 };  // PAS values for speeds 1-8
int sf2000_dpad_mode = 1;
// v058: Kickstart selection (0=1.3, 1=2.0, 2=3.0)
int sf2000_kickstart = 0;  // Default: Kickstart 1.3
// v073: Slow RAM (bogomem) - 0=off, 1=512KB, 2=1MB, 3=1.5MB
int sf2000_slowram = 0;    // Default: 0KB
static const unsigned int slowram_values[] = {0, 0x80000, 0x100000, 0x180000};  // 0KB, 512KB, 1MB, 1.5MB
extern unsigned prefs_bogomem_size;  // From memory.cpp
// v074: Settings menu items and scrolling (added Save/Load State)
// v091: Removed Save/Load State from menu (use Y button instead)
#define SF2000_SETTINGS_ITEMS 6  // Kickstart, SlowRAM, Reset, SaveCfg, DeleteCfg, Back
#define SF2000_SETTINGS_VISIBLE 5  // Max visible items at once
static int sf2000_settings_scroll = 0;  // Scroll offset for Settings menu

// v058: Feedback message system
static char sf2000_feedback_msg[64] = {0};
static int sf2000_feedback_timer = 0;
#define SF2000_FEEDBACK_FRAMES 90  // Show message for ~1.5 seconds

// v058: Kickstart ROM filenames (in bios folder)
static const char* kickstart_files[] = {
    "kick13.rom",   // 0 = Kick 1.3
    "kick20.rom",   // 1 = Kick 2.0
    "kick30.rom"    // 2 = Kick 3.0
};

// v067: Check if file exists (using firmware fs_open)
static int file_exists(const char* path) {
    int fd = fs_open(path, FS_O_RDONLY, 0);
    if (fd >= 0) {
        fs_close(fd);
        return 1;
    }
    return 0;
}

// v058: Build kickstart ROM path and check existence
static int kickstart_rom_exists(int kick_version, char* path_out, int path_size) {
    if (kick_version < 0 || kick_version > 2) return 0;
    snprintf(path_out, path_size, "%s/%s", retro_system_directory, kickstart_files[kick_version]);
    return file_exists(path_out);
}

// v058: Find best available kickstart (fallback chain: requested -> 1.3 -> 2.0 -> 3.0)
static int find_available_kickstart(int requested) {
    char path[256];
    // Try requested version first
    if (kickstart_rom_exists(requested, path, sizeof(path))) return requested;
    // Fallback chain: 1.3, 2.0, 3.0
    for (int i = 0; i <= 2; i++) {
        if (kickstart_rom_exists(i, path, sizeof(path))) return i;
    }
    return 0;  // Default to 1.3 even if not found
}

// v058: Update romfile to match sf2000_kickstart
static void update_romfile_for_kickstart(void) {
    char path[256];
    int actual_kick = find_available_kickstart(sf2000_kickstart);
    if (actual_kick != sf2000_kickstart) {
        // ROM not found, using fallback
        sf2000_kickstart = actual_kick;
    }
    snprintf(path, sizeof(path), "%s/%s", retro_system_directory, kickstart_files[sf2000_kickstart]);
    strncpy(romfile, path, 63);
    romfile[63] = '\0';
}

// v067: Per-game config - same folder as ROM, with .cfg extension
// Example: /mnt/sda1/ROMS/amiga/Lotus2.adf -> /mnt/sda1/ROMS/amiga/Lotus2.cfg
static void get_config_path(char* path, int size) {
    // Copy RPATH and replace extension with .cfg
    strncpy(path, RPATH, size - 5);  // Leave room for .cfg\0
    path[size - 5] = '\0';

    // Find last dot
    char* dot = strrchr(path, '.');
    if (dot) {
        strcpy(dot, ".cfg");
    } else {
        // No extension - just append .cfg
        strncat(path, ".cfg", size - strlen(path) - 1);
    }
}

// v067: Save per-game config using DIRECT firmware fs_* calls
static int sf2000_save_config(void) {
    char path[256];
    get_config_path(path, sizeof(path));

    // v061: Use firmware fs_open directly - this is what xlog uses internally
    int fd = fs_open(path, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, 0666);
    if (fd < 0) return 0;

    // Build config content in buffer
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "# SF2K-UAE Config v073\n"
        "kickstart=%d\n"
        "slowram=%d\n"
        "frameskip=%d\n"
        "sound=%d\n"
        "cpu=%d\n"
        "frogjoy1=%d\n"
        "frogjoy2=%d\n"
        "turbo_floppy=%d\n"
        "y_offset=%d\n"
        "mouse_speed=%d\n",
        sf2000_kickstart,
        sf2000_slowram,
        sf2000_frameskip,
        sf2000_sound_mode,
        sf2000_cpu_timing,
        sf2000_frogjoy1,
        sf2000_frogjoy2,
        sf2000_turbo_floppy,
        sf2000_y_offset,
        sf2000_mouse_speed);

    // Write using firmware function
    ssize_t written = fs_write(fd, buf, len);
    fs_close(fd);

    // Sync to ensure data is written to SD card
    fs_sync(path);

    return (written == len) ? 1 : 0;
}

// v067: Load per-game config using DIRECT firmware fs_* calls
static int sf2000_load_config(void) {
    char path[256];
    get_config_path(path, sizeof(path));

    // v061: Use firmware fs_open directly
    int fd = fs_open(path, FS_O_RDONLY, 0);
    if (fd < 0) return 0;

    // Read entire file into buffer
    char buf[512];
    ssize_t bytes_read = fs_read(fd, buf, sizeof(buf) - 1);
    fs_close(fd);

    if (bytes_read <= 0) return 0;
    buf[bytes_read] = '\0';

    // Parse line by line
    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = '\0';

        if (line[0] != '#' && line[0] != '\0') {
            int val;
            if (sscanf(line, "kickstart=%d", &val) == 1) sf2000_kickstart = val;
            else if (sscanf(line, "slowram=%d", &val) == 1) sf2000_slowram = val;
            // v073: fastram removed (didn't work), ignore old configs with fastram
            else if (sscanf(line, "frameskip=%d", &val) == 1) sf2000_frameskip = val;
            else if (sscanf(line, "sound=%d", &val) == 1) sf2000_sound_mode = val;
            else if (sscanf(line, "cpu=%d", &val) == 1) sf2000_cpu_timing = val;
            else if (sscanf(line, "frogjoy1=%d", &val) == 1) sf2000_frogjoy1 = val;
            else if (sscanf(line, "frogjoy2=%d", &val) == 1) sf2000_frogjoy2 = val;
            else if (sscanf(line, "turbo_floppy=%d", &val) == 1) sf2000_turbo_floppy = val;
            else if (sscanf(line, "y_offset=%d", &val) == 1) sf2000_y_offset = val;
            else if (sscanf(line, "mouse_speed=%d", &val) == 1) sf2000_mouse_speed = val;
        }
        line = next;
    }

    // Validate loaded kickstart - make sure ROM exists
    sf2000_kickstart = find_available_kickstart(sf2000_kickstart);

    return 1;
}

// v058: Set feedback message
static void sf2000_set_feedback(const char* msg) {
    strncpy(sf2000_feedback_msg, msg, sizeof(sf2000_feedback_msg) - 1);
    sf2000_feedback_msg[sizeof(sf2000_feedback_msg) - 1] = '\0';
    sf2000_feedback_timer = SF2000_FEEDBACK_FRAMES;
}

// v058: Forward declaration for apply_settings (defined later)
static void sf2000_apply_settings(void);

// v067: Initialize config at startup (called from retro_load_game)
// NOTE: Does NOT set romfile - that happens in sf2000_apply_kickstart_override()
// which is called AFTER default_prefs() sets romfile to "kick.rom"
void sf2000_init_config(void) {
    sf2000_config_loaded = 0;  // Reset flag
    if (sf2000_load_config()) {
        // Config loaded - remember kickstart setting
        sf2000_config_loaded = 1;
        // Apply other settings (frameskip, sound, cpu, floppy)
        sf2000_apply_settings();
    }
}

// v067: Apply kickstart from config (call AFTER update_prefs_retrocfg sets default)
// This is called from libretro-core.cpp after path_join sets romfile to kick13.rom
void sf2000_apply_kickstart_override(void) {
    // Always apply kickstart from config if config was loaded
    if (sf2000_config_loaded) {
        update_romfile_for_kickstart();
    }
}

// v030: FIXED direction codes (hardcoded, not configurable)
// Based on user testing: UP=0100, DOWN=0001, LEFT=1100, RIGHT=0011
// Format: 4-bit value = bit9 bit8 bit1 bit0
#define DIR_UP    4   // 0100
#define DIR_DOWN  1   // 0001
#define DIR_LEFT  12  // 1100
#define DIR_RIGHT 3   // 0011

// v030: FIXED diagonal codes (based on user testing)
// UP+RT = 0111 (7)
// UP+LT = 1000 (8) - CORRECTED!
// DN+RT = 0010 (2) - CORRECTED!
// DN+LT = 1101 (13)
#define DIAG_UPRT 7   // 0111
#define DIAG_UPLT 8   // 1000 - user tested
#define DIAG_DNRT 2   // 0010 - user tested
#define DIAG_DNLT 13  // 1101

extern int produce_sound;
extern int prefs_gfx_framerate;
extern int m68k_speed;
extern int floppy_speed;
static int menu_start_held = 0;
static int menu_entry_delay = 0;
static int menu_first_frame = 1;

static retro_input_state_t input_state_cb_menu;

#define NORMAL_FLOPPY_SPEED 1830
static const int floppy_speed_table[5] = { 1830, 915, 458, 229, 100 };

// v068: Forward declaration for second_joystick_enable (defined later)
extern int second_joystick_enable;

static void sf2000_apply_settings(void) {
    prefs_gfx_framerate = sf2000_frameskip;
    produce_sound = sf2000_sound_mode;
    m68k_speed = sf2000_cpu_timing;
    // v034: FrogJoy system - MOUSE_EMULATED based on whether any controller is mouse mode
    MOUSE_EMULATED = (sf2000_frogjoy1 == 2 || sf2000_frogjoy2 == 2) ? 1 : -1;
    // v068: Also update second_joystick_enable - this was missing and caused menu mouse toggle to fail!
    // When mouse mode is active, disable second joystick (same as L+R toggle does)
    if (MOUSE_EMULATED == 1) {
        second_joystick_enable = 0;
    }
    floppy_speed = floppy_speed_table[sf2000_turbo_floppy];
    // v069: Apply mouse speed from table
    PAS = mouse_speed_table[sf2000_mouse_speed];
    // v073: Apply Slow RAM setting (expanded to 1.5MB)
    prefs_bogomem_size = slowram_values[sf2000_slowram];
}

// Convert 4-bit config to joy1dir format
static unsigned int bits4_to_joydir(int v) {
    return (v & 3) | ((v & 12) << 6);
}

// v058: Joystick debug overlay - REMOVED from menu (function kept for compatibility)
extern unsigned int joy1dir;
void sf2000_joy_debug_overlay(char *pixels) {
    // v058: JoyDbg removed from menu - this function is now a no-op
    (void)pixels;
    return;
}

// Menu layout constants - v030 simplified
#define MENU_X      50
#define MENU_Y      30
#define MENU_W      220
#define MENU_H      180
#define MENU_LINE_H 14
#define MENU_BG     RGB565(0, 0, 32)
#define MENU_FG     RGB565(255, 255, 255)
#define MENU_SEL    RGB565(255, 255, 0)
#define MENU_TITLE  RGB565(0, 200, 255)   // Cyan for title
#define MENU_AUTHOR RGB565(180, 180, 180) // Gray for author
#define MENU_SEP    RGB565(100, 100, 100) // Gray separator

extern int disk_empty(int num);
static int count_loaded_adfs(void) {
    int count = 0;
    for (int i = 0; i < NUM_DRIVES; i++) {
        if (!disk_empty(i)) count++;
    }
    return count;
}

// v058: Feedback message overlay - shows temporary messages on screen
void sf2000_feedback_overlay(char *pixels) {
    if (sf2000_feedback_timer <= 0) return;  // No message to show
    sf2000_feedback_timer--;

    // Draw message box at top of screen
    int msg_len = strlen(sf2000_feedback_msg);
    int box_w = msg_len * 8 + 20;
    int box_x = (320 - box_w) / 2;
    int box_y = 10;

    // Background
    DrawFBoxBmp(pixels, box_x, box_y, box_w, 20, RGB565(0, 64, 0));
    DrawBoxBmp(pixels, box_x, box_y, box_w, 20, RGB565(0, 255, 0));

    // Text (centered)
    Draw_text(pixels, box_x + 10, box_y + 4, RGB565(255, 255, 255), RGB565(0, 64, 0), 1, 1, 40, sf2000_feedback_msg);
}

// v054: FrogJoy mode strings - clearer labels
static const char* frogjoy_str(int mode) {
    switch(mode) {
        case 0: return "Port1 Joy(Plr1)";
        case 1: return "Port0 Joy(Plr2)";
        case 2: return "Port0 Mouse";
        default: return "???";
    }
}

// v036: Access actual memory sizes from UAE core
extern unsigned prefs_chipmem_size;
extern uae_u32 allocated_chipmem;
extern uae_u32 allocated_bogomem;
extern uae_u32 allocated_fastmem;

// v058: Kickstart version strings
static const char* kickstart_str(int ver) {
    switch(ver) {
        case 0: return "1.3";
        case 1: return "2.0";
        case 2: return "3.0";
        default: return "1.3";
    }
}

// v058: Settings submenu overlay (replaces Details)
void sf2000_settings_overlay(char *pixels) {
    DrawFBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_BG);
    DrawBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_FG);

    int y = MENU_Y + 6;
    char buf[40];

    // Title
    Draw_text(pixels, MENU_X + 40, y, MENU_TITLE, MENU_BG, 1, 1, 30, "SETTINGS");
    y += MENU_LINE_H;

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 10;

    // Current system info
    int chip_kb = allocated_chipmem / 1024;
    snprintf(buf, sizeof(buf), "Chip RAM: %dMB", chip_kb / 1024);
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, buf);
    y += MENU_LINE_H;

    snprintf(buf, sizeof(buf), "Disks: %d loaded", count_loaded_adfs());
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, buf);
    y += MENU_LINE_H;

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 10;

    // v070: Settings items with scrolling
    int end_item = sf2000_settings_scroll + SF2000_SETTINGS_VISIBLE;
    if (end_item > SF2000_SETTINGS_ITEMS) end_item = SF2000_SETTINGS_ITEMS;

    for (int i = sf2000_settings_scroll; i < end_item; i++) {
        unsigned int col = (sf2000_settings_item == i) ? MENU_SEL : MENU_FG;
        const char *sel = (sf2000_settings_item == i) ? ">" : " ";

        switch(i) {
            case 0:
                snprintf(buf, sizeof(buf), "%sKickstart: %s", sel, kickstart_str(sf2000_kickstart));
                break;
            case 1:
                // v073: Slow RAM (Off/512KB/1MB/1.5MB)
                {
                    const char* slowram_str[] = {"Off", "512KB", "1MB", "1.5MB"};
                    snprintf(buf, sizeof(buf), "%sSlow RAM: %s", sel, slowram_str[sf2000_slowram]);
                }
                break;
            case 2:
                snprintf(buf, sizeof(buf), "%sReset Machine", sel);
                break;
            case 3:
                snprintf(buf, sizeof(buf), "%sSave Config", sel);
                break;
            case 4:
                // v070: Delete Config in reddish warning color
                snprintf(buf, sizeof(buf), "%sDelete Config", sel);
                col = (sf2000_settings_item == i) ? RGB565(255, 100, 100) : RGB565(200, 80, 80);
                break;
            case 5:
                // v091: Back (was case 7 before removing Save/Load State)
                snprintf(buf, sizeof(buf), "%sBack", sel);
                break;
        }
        Draw_text(pixels, MENU_X + 10, y, col, MENU_BG, 1, 1, 30, buf);
        y += MENU_LINE_H;
    }

    // v073: Show blue down arrow indicator if more items below
    if (end_item < SF2000_SETTINGS_ITEMS) {
        Draw_text(pixels, MENU_X + (MENU_W / 2) - 10, y, RGB565(0, 150, 255), MENU_BG, 1, 1, 10, "\\/");
    }

    // Help text at bottom
    y = MENU_Y + MENU_H - 26;
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "L/R:change A:select");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 10, y, RGB565(255, 200, 100), MENU_BG, 1, 1, 30, "*Kick/RAM need restart");
}

// v058: About submenu overlay
void sf2000_about_overlay(char *pixels) {
    DrawFBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_BG);
    DrawBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_FG);

    int y = MENU_Y + 6;

    // Title
    Draw_text(pixels, MENU_X + 55, y, MENU_TITLE, MENU_BG, 1, 1, 30, "ABOUT");
    y += MENU_LINE_H;

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 12;

    // Version
    Draw_text(pixels, MENU_X + 50, y, MENU_FG, MENU_BG, 1, 1, 30, "SF2K-UAE v078");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 25, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "Amiga 500 Emulator");
    y += MENU_LINE_H + 4;

    // Contact
    Draw_text(pixels, MENU_X + 10, y, RGB565(128, 200, 255), MENU_BG, 1, 1, 30, "Contact:");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 20, y, MENU_FG, MENU_BG, 1, 1, 30, "@the_q_dev on Telegram");
    y += MENU_LINE_H + 4;

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 10;

    // Greetings
    Draw_text(pixels, MENU_X + 10, y, RGB565(128, 200, 255), MENU_BG, 1, 1, 30, "Greetings to:");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 20, y, MENU_FG, MENU_BG, 1, 1, 30, "Maciek, Madzia, Eliasz");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 20, y, MENU_FG, MENU_BG, 1, 1, 30, "Eliza, Tomek");
    y += MENU_LINE_H;

    // Help text at bottom
    y = MENU_Y + MENU_H - 16;
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "Press any button to close");
}

// v055: Disk Shuffler submenu overlay
void sf2000_disk_shuffler_overlay(char *pixels) {
    // Draw background
    DrawFBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_BG);
    DrawBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_FG);

    int y = MENU_Y + 6;
    char buf[40];
    char diskname[20];

    // Title
    int total_disks = disk_get_multidisk_count();
    snprintf(buf, sizeof(buf), "DISK SHUFFLER (%d disks)", total_disks);
    Draw_text(pixels, MENU_X + 20, y, MENU_TITLE, MENU_BG, 1, 1, 30, buf);
    y += MENU_LINE_H;

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 10;

    // Show current drive assignments
    for (int i = 0; i < NUM_DRIVES; i++) {
        disk_get_name(i, diskname, 18);
        snprintf(buf, sizeof(buf), "DF%d: %s", i, diskname);

        // Highlight drives with disks, dim empty drives
        unsigned int col = (diskname[0] == '<') ? MENU_AUTHOR : MENU_FG;
        Draw_text(pixels, MENU_X + 10, y, col, MENU_BG, 1, 1, 30, buf);
        y += MENU_LINE_H;
    }

    // Separator
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 10;

    // Show hint about extra disks
    if (total_disks > NUM_DRIVES) {
        snprintf(buf, sizeof(buf), "+%d disks in queue", total_disks - NUM_DRIVES);
        Draw_text(pixels, MENU_X + 10, y, RGB565(255, 200, 100), MENU_BG, 1, 1, 30, buf);
        y += MENU_LINE_H;
    }

    // Help text at bottom
    y = MENU_Y + MENU_H - 26;
    Draw_text(pixels, MENU_X + 10, y, MENU_SEL, MENU_BG, 1, 1, 30, "A/B: SHUFFLE DISKS");
    y += MENU_LINE_H;
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "START: back to menu");
}

// v058: Scrollable menu - redesigned
void sf2000_menu_overlay(char *pixels) {
    const char *sound_str = (sf2000_sound_mode==0)?"OFF":(sf2000_sound_mode==1)?"ON":"EMUL";
    const char *turbo_str[] = {"1x", "2x", "4x", "8x", "MAX"};

    // Draw background
    DrawFBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_BG);
    DrawBoxBmp(pixels, MENU_X, MENU_Y, MENU_W, MENU_H, MENU_FG);

    int y = MENU_Y + 6;
    char buf[40];

    // Title line 1 - cyan (centered)
    Draw_text(pixels, MENU_X + 55, y, MENU_TITLE, MENU_BG, 1, 1, 30, "SF2K-UAE v078");
    y += MENU_LINE_H;

    // Title line 2 - gray author
    Draw_text(pixels, MENU_X + 35, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "by Grzegorz Korycki");
    y += MENU_LINE_H;

    // Separator line
    DrawFBoxBmp(pixels, MENU_X + 5, y + 2, MENU_W - 10, 1, MENU_SEP);
    y += 8;

    // v036: Update scroll offset to keep selection visible
    if (sf2000_menu_item < sf2000_menu_scroll) {
        sf2000_menu_scroll = sf2000_menu_item;
    } else if (sf2000_menu_item >= sf2000_menu_scroll + SF2000_MENU_VISIBLE) {
        sf2000_menu_scroll = sf2000_menu_item - SF2000_MENU_VISIBLE + 1;
    }

    // v036: Show scroll indicator at top if scrolled
    if (sf2000_menu_scroll > 0) {
        Draw_text(pixels, MENU_X + MENU_W - 30, y - 6, MENU_AUTHOR, MENU_BG, 1, 1, 10, "...");
    }

    // Menu items - v036: only show visible range
    int end_item = sf2000_menu_scroll + SF2000_MENU_VISIBLE;
    if (end_item > SF2000_MENU_ITEMS) end_item = SF2000_MENU_ITEMS;

    for (int item = sf2000_menu_scroll; item < end_item; item++) {
        unsigned int col = (sf2000_menu_item == item) ? MENU_SEL : MENU_FG;
        const char *sel = (sf2000_menu_item == item) ? ">" : " ";

        switch(item) {
            case 0:
                // v056: Disk Shuffler moved to first position
                snprintf(buf, sizeof(buf), "%s1.Disks...", sel);
                break;
            case 1:
                snprintf(buf, sizeof(buf), "%s2.FrogJoy1: %s", sel, frogjoy_str(sf2000_frogjoy1));
                break;
            case 2:
                snprintf(buf, sizeof(buf), "%s3.FrogJoy2: %s", sel, frogjoy_str(sf2000_frogjoy2));
                break;
            case 3:
                // v070: Mouse Speed 1-8 - inactive (gray) when no mouse selected
                {
                    int mouse_active = (sf2000_frogjoy1 == 2 || sf2000_frogjoy2 == 2);
                    if (mouse_active) {
                        snprintf(buf, sizeof(buf), "%s4.MouseSpd: %d/8", sel, sf2000_mouse_speed + 1);
                    } else {
                        snprintf(buf, sizeof(buf), "%s4.MouseSpd: --", sel);
                        col = 0x8410;  // Gray color when inactive
                    }
                }
                break;
            case 4:
                snprintf(buf, sizeof(buf), "%s5.Frameskip: %d", sel, sf2000_frameskip);
                break;
            case 5:
                snprintf(buf, sizeof(buf), "%s6.Sound: %s", sel, sound_str);
                break;
            case 6:
                snprintf(buf, sizeof(buf), "%s7.CPU: %d", sel, sf2000_cpu_timing);
                break;
            case 7:
                snprintf(buf, sizeof(buf), "%s8.Y-Offset: %d", sel, sf2000_y_offset);
                break;
            case 8:
                snprintf(buf, sizeof(buf), "%s9.Floppy: %s", sel, turbo_str[sf2000_turbo_floppy]);
                break;
            case 9:
                snprintf(buf, sizeof(buf), "%sA.Settings...", sel);
                break;
            case 10:
                snprintf(buf, sizeof(buf), "%sB.About...", sel);
                break;
            case 11:
                snprintf(buf, sizeof(buf), "%s0.EXIT", sel);
                break;
        }
        Draw_text(pixels, MENU_X + 10, y, col, MENU_BG, 1, 1, 30, buf);
        y += MENU_LINE_H;
    }

    // v036: Show scroll indicator at bottom if more items below
    if (end_item < SF2000_MENU_ITEMS) {
        Draw_text(pixels, MENU_X + MENU_W - 30, y, MENU_AUTHOR, MENU_BG, 1, 1, 10, "...");
    }

    // Bottom: help text
    y = MENU_Y + MENU_H - 16;
    Draw_text(pixels, MENU_X + 10, y, MENU_AUTHOR, MENU_BG, 1, 1, 30, "L+R:mouse START:close");
}

// v030: Handle menu input - START toggles
static void sf2000_handle_menu_input(void) {
    static int menu_delay = 0;
    static int prev_up = 0, prev_down = 0, prev_left = 0, prev_right = 0, prev_a = 0, prev_b = 0, prev_start = 0;
    static int details_entry_delay = 0;

    // v055: Disk Shuffler submenu - A/B shuffle, START closes
    if (sf2000_disk_shuffler_active) {
        if (details_entry_delay > 0) {
            details_entry_delay--;
            return;
        }
        int any_a = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
        int any_b = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
        int any_start = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
        if (any_a || any_b) {
            // Perform disk shuffle
            disk_shuffle();
            details_entry_delay = 20;  // debounce after shuffle
        } else if (any_start) {
            // Close disk shuffler submenu
            sf2000_disk_shuffler_active = 0;
            details_entry_delay = 15;
        }
        return;
    }

    // v058: About submenu - any button closes it
    if (sf2000_about_active) {
        if (details_entry_delay > 0) {
            details_entry_delay--;
            return;
        }
        int any_a = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
        int any_b = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
        int any_start = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
        if (any_a || any_b || any_start) {
            sf2000_about_active = 0;
            details_entry_delay = 15;  // debounce
        }
        return;
    }

    // v070: Settings submenu - full navigation with scroll and START exit
    if (sf2000_settings_active) {
        if (details_entry_delay > 0) {
            details_entry_delay--;
            return;
        }
        int cur_up    = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        int cur_down  = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        int cur_left  = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        int cur_right = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        int cur_a     = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
        int cur_b     = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
        int cur_start = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);

        static int s_prev_up = 0, s_prev_down = 0, s_prev_left = 0, s_prev_right = 0, s_prev_a = 0, s_prev_b = 0, s_prev_start = 0;
        static int s_delay = 0;

        if (s_delay > 0) s_delay--;

        // v070: START exits to main menu (not closes everything)
        if (cur_start && !s_prev_start) {
            sf2000_settings_active = 0;
            sf2000_settings_item = 0;
            sf2000_settings_scroll = 0;
            details_entry_delay = 15;
            s_prev_start = cur_start;
            return;
        }

        // B also closes settings
        if (cur_b && !s_prev_b) {
            sf2000_settings_active = 0;
            sf2000_settings_item = 0;
            sf2000_settings_scroll = 0;
            details_entry_delay = 15;
            s_prev_b = cur_b;
            return;
        }

        if (s_delay == 0) {
            // UP/DOWN - navigate items with scroll
            if (cur_up && !s_prev_up) {
                sf2000_settings_item--;
                if (sf2000_settings_item < 0) sf2000_settings_item = SF2000_SETTINGS_ITEMS - 1;
                // v070: Adjust scroll
                if (sf2000_settings_item < sf2000_settings_scroll)
                    sf2000_settings_scroll = sf2000_settings_item;
                if (sf2000_settings_item >= sf2000_settings_scroll + SF2000_SETTINGS_VISIBLE)
                    sf2000_settings_scroll = sf2000_settings_item - SF2000_SETTINGS_VISIBLE + 1;
                s_delay = 8;
            }
            if (cur_down && !s_prev_down) {
                sf2000_settings_item++;
                if (sf2000_settings_item >= SF2000_SETTINGS_ITEMS) sf2000_settings_item = 0;
                // v070: Adjust scroll
                if (sf2000_settings_item < sf2000_settings_scroll)
                    sf2000_settings_scroll = sf2000_settings_item;
                if (sf2000_settings_item >= sf2000_settings_scroll + SF2000_SETTINGS_VISIBLE)
                    sf2000_settings_scroll = sf2000_settings_item - SF2000_SETTINGS_VISIBLE + 1;
                s_delay = 8;
            }
            // LEFT/RIGHT - change values
            if (cur_left && !s_prev_left) {
                switch (sf2000_settings_item) {
                    case 0: if (sf2000_kickstart > 0) sf2000_kickstart--; else sf2000_kickstart = 2; break;  // Kickstart
                    case 1: if (sf2000_slowram > 0) sf2000_slowram--; else sf2000_slowram = 3; break;  // v073: Slow RAM cycle
                }
                s_delay = 8;
            }
            if (cur_right && !s_prev_right) {
                switch (sf2000_settings_item) {
                    case 0: if (sf2000_kickstart < 2) sf2000_kickstart++; else sf2000_kickstart = 0; break;  // Kickstart
                    case 1: if (sf2000_slowram < 3) sf2000_slowram++; else sf2000_slowram = 0; break;  // v073: Slow RAM cycle
                }
                s_delay = 8;
            }
            // A - select action
            if (cur_a && !s_prev_a) {
                switch (sf2000_settings_item) {
                    case 2:  // Reset Machine (applies Kickstart change) - v073: shifted back
                        update_romfile_for_kickstart();  // Update ROM path before reset
                        uae_reset();
                        sf2000_set_feedback("Reset with new settings!");
                        sf2000_settings_active = 0;
                        sf2000_menu_active = 0;
                        pauseg = 0;
                        menu_first_frame = 1;
                        break;
                    case 3:  // Save Config - v073: shifted back
                        if (sf2000_save_config()) {
                            sf2000_set_feedback("Config saved!");
                        } else {
                            sf2000_set_feedback("Save FAILED!");
                        }
                        break;
                    case 4:  // v070: Delete Config - v073: shifted back
                        {
                            char path[256];
                            get_config_path(path, sizeof(path));
                            // No fs_unlink in firmware, so truncate file to 0 bytes
                            int fd = fs_open(path, FS_O_WRONLY | FS_O_TRUNC, 0777);
                            if (fd >= 0) {
                                fs_close(fd);
                                sf2000_set_feedback("Config deleted!");
                            } else {
                                sf2000_set_feedback("No config to delete");
                            }
                        }
                        break;
                    case 5:  // v091: Back (was case 7, Save/Load State removed from menu)
                        sf2000_settings_active = 0;
                        sf2000_settings_item = 0;
                        sf2000_settings_scroll = 0;
                        details_entry_delay = 15;
                        break;
                }
                s_delay = 15;
            }
        }

        s_prev_up = cur_up;
        s_prev_down = cur_down;
        s_prev_left = cur_left;
        s_prev_right = cur_right;
        s_prev_a = cur_a;
        s_prev_b = cur_b;
        s_prev_start = cur_start;
        return;
    }

    if (menu_first_frame) {
        prev_up = prev_down = prev_left = prev_right = prev_a = prev_b = prev_start = 1;
        menu_first_frame = 0;
        menu_entry_delay = 20;
    }

    if (menu_entry_delay > 0) {
        menu_entry_delay--;
        return;
    }

    int cur_up    = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    int cur_down  = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    int cur_left  = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    int cur_right = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    int cur_a     = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    int cur_b     = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    int cur_start = input_state_cb_menu(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);

    // START closes menu (toggle)
    if (cur_start && !prev_start) {
        sf2000_menu_active = 0;
        pauseg = 0;
        menu_first_frame = 1;
        prev_start = cur_start;
        return;
    }

    if (menu_delay > 0) {
        menu_delay--;
    } else {
        // UP - previous item
        if (cur_up && !prev_up) {
            sf2000_menu_item--;
            if (sf2000_menu_item < 0) sf2000_menu_item = SF2000_MENU_ITEMS - 1;
            menu_delay = 8;
        }
        // DOWN - next item
        if (cur_down && !prev_down) {
            sf2000_menu_item++;
            if (sf2000_menu_item >= SF2000_MENU_ITEMS) sf2000_menu_item = 0;
            menu_delay = 8;
        }
        // LEFT - decrease value (v069: added Mouse Speed at case 3)
        if (cur_left && !prev_left) {
            switch (sf2000_menu_item) {
                case 0: break;  // Disks submenu - opens via A
                case 1: if (sf2000_frogjoy1 > 0) sf2000_frogjoy1--; else sf2000_frogjoy1 = 2; break;
                case 2: if (sf2000_frogjoy2 > 0) sf2000_frogjoy2--; else sf2000_frogjoy2 = 2; break;
                case 3:  // v069: Mouse Speed - only works when mouse is active
                    if (sf2000_frogjoy1 == 2 || sf2000_frogjoy2 == 2) {
                        if (sf2000_mouse_speed > 0) sf2000_mouse_speed--;
                    }
                    break;
                case 4: if (sf2000_frameskip > 0) sf2000_frameskip--; break;
                case 5: if (sf2000_sound_mode > 0) sf2000_sound_mode--; break;
                case 6: if (sf2000_cpu_timing > 1) sf2000_cpu_timing--; break;
                case 7: if (sf2000_y_offset > -50) sf2000_y_offset -= 5; break;
                case 8: if (sf2000_turbo_floppy > 0) sf2000_turbo_floppy--; break;
                case 9: break;  // Settings submenu - opens via A
                case 10: break;  // About submenu - opens via A
                case 11: break;  // EXIT
            }
            sf2000_apply_settings();
            menu_delay = 8;
        }
        // RIGHT - increase value (v069: added Mouse Speed at case 3)
        if (cur_right && !prev_right) {
            switch (sf2000_menu_item) {
                case 0: break;  // Disks submenu - opens via A
                case 1: if (sf2000_frogjoy1 < 2) sf2000_frogjoy1++; else sf2000_frogjoy1 = 0; break;
                case 2: if (sf2000_frogjoy2 < 2) sf2000_frogjoy2++; else sf2000_frogjoy2 = 0; break;
                case 3:  // v069: Mouse Speed - only works when mouse is active
                    if (sf2000_frogjoy1 == 2 || sf2000_frogjoy2 == 2) {
                        if (sf2000_mouse_speed < 7) sf2000_mouse_speed++;  // v070: max 8 speeds
                    }
                    break;
                case 4: if (sf2000_frameskip < 5) sf2000_frameskip++; break;
                case 5: if (sf2000_sound_mode < 2) sf2000_sound_mode++; break;
                case 6: if (sf2000_cpu_timing < 8) sf2000_cpu_timing++; break;
                case 7: if (sf2000_y_offset < 50) sf2000_y_offset += 5; break;
                case 8: if (sf2000_turbo_floppy < 4) sf2000_turbo_floppy++; break;
                case 9: break;  // Settings submenu - opens via A
                case 10: break;  // About submenu - opens via A
                case 11: break;  // EXIT
            }
            sf2000_apply_settings();
            menu_delay = 8;
        }
        // A - confirm / exit / open submenus (v069: indices shifted +1 due to Mouse Speed)
        if (cur_a && !prev_a) {
            if (sf2000_menu_item == 0) {  // Disk Shuffler
                sf2000_disk_shuffler_active = 1;
                details_entry_delay = 15;
                menu_delay = 15;
            } else if (sf2000_menu_item == 9) {  // Settings
                sf2000_settings_active = 1;
                sf2000_settings_item = 0;
                details_entry_delay = 15;
                menu_delay = 15;
            } else if (sf2000_menu_item == 10) {  // About
                sf2000_about_active = 1;
                details_entry_delay = 15;
                menu_delay = 15;
            } else if (sf2000_menu_item == 11) {  // EXIT
                sf2000_menu_active = 0;
                pauseg = 0;
                menu_first_frame = 1;
                menu_delay = 15;
            }
        }
        // B - also exits
        if (cur_b && !prev_b) {
            sf2000_menu_active = 0;
            pauseg = 0;
            menu_first_frame = 1;
            menu_delay = 15;
        }
    }

    prev_up = cur_up;
    prev_down = cur_down;
    prev_left = cur_left;
    prev_right = cur_right;
    prev_a = cur_a;
    prev_b = cur_b;
    prev_start = cur_start;
}



//MOUSE
int gmx,gmy; //gui mouse
int mouse_wu=0,mouse_wd=0;
//KEYBOARD
char Key_State[512];
static char old_Key_State[512];

static int mbt[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

//STATS GUI
int BOXDEC= 32+2;
int STAT_BASEY;

static retro_input_state_t input_state_cb;
static retro_input_poll_t input_poll_cb;

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
    input_state_cb_menu = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}


long GetTicks(void)
{
#ifndef _ANDROID_
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (tv.tv_sec*1000000 + tv.tv_usec);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec*1000000 + now.tv_nsec/1000);
#endif

}



// Joystick management

int second_joystick_enable = 1;

// v030: Compute joy1dir with FIXED direction and diagonal values
// Uses hardcoded values based on user testing
static unsigned int compute_joy1dir_configurable(int up, int down, int left, int right) {
    // Check for diagonals first
    if (up && right) return bits4_to_joydir(DIAG_UPRT);  // 7 = 0111
    if (up && left)  return bits4_to_joydir(DIAG_UPLT);  // 8 = 1000
    if (down && right) return bits4_to_joydir(DIAG_DNRT);  // 2 = 0010
    if (down && left)  return bits4_to_joydir(DIAG_DNLT);  // 13 = 1101

    // Single directions
    if (up)    return bits4_to_joydir(DIR_UP);     // 4 = 0100
    if (down)  return bits4_to_joydir(DIR_DOWN);   // 1 = 0001
    if (left)  return bits4_to_joydir(DIR_LEFT);   // 12 = 1100
    if (right) return bits4_to_joydir(DIR_RIGHT);  // 3 = 0011

    return 0;  // Neutral
}

// OLD method - uses OR of individual directions (may not work for diagonals)
static unsigned int compute_joy1dir_old(int up, int down, int left, int right) {
    unsigned int joy_value = 0;
    if (up)    joy_value |= bits4_to_joydir(DIR_UP);
    if (down)  joy_value |= bits4_to_joydir(DIR_DOWN);
    if (left)  joy_value |= bits4_to_joydir(DIR_LEFT);
    if (right) joy_value |= bits4_to_joydir(DIR_RIGHT);
    return joy_value;
}

// NEW method: PSP UAE Gray code algorithm
static unsigned int compute_joy1dir_new(int up, int down, int left, int right) {
    int top = up ? 1 : 0;
    int bot = down ? 1 : 0;

    if (left) top = !top;
    if (right) bot = !bot;

    unsigned int result = bot | (right << 1) | (top << 8) | (left << 9);
    return result;
}

// v032: Always use OLD method (configurable with FIXED diagonals)
// Removed sf2000_joy_mode option - NEW method didn't work
static unsigned int compute_joy1dir(int up, int down, int left, int right) {
    return compute_joy1dir_configurable(up, down, left, right);
}

// D-pad mode transformation
static void apply_dpad_mode(int raw_up, int raw_down, int raw_left, int raw_right,
                            int *out_up, int *out_down, int *out_left, int *out_right)
{
    switch(sf2000_dpad_mode) {
        case 0:  // SWAP-XY
            *out_up = raw_left; *out_down = raw_right;
            *out_left = raw_up; *out_right = raw_down;
            break;
        case 1:  // NORMAL
        default:
            *out_up = raw_up; *out_down = raw_down;
            *out_left = raw_left; *out_right = raw_right;
            break;
    }
}

// v042: read_joystick uses CACHED input values to prevent frameskip autofire
// IMPORTANT: getjoystate uses SWAPPED convention:
//   getjoystate(0, &joy1dir, ...) -> reads Port 1 (main joystick), stores in joy1
//   getjoystate(1, &joy0dir, ...) -> reads Port 0 (mouse/P2), stores in joy0
// So: nr=0 means Port 1, nr=1 means Port 0
// FrogJoy1 = physical controller 0 (main SF2000)
// FrogJoy2 = physical controller 1 (Data Frog 2nd controller)
//
// v042 FIX: This function is called multiple times per retro_run frame when
// frameskip > 0 (vsync_handler calls getjoystate for each internal UAE frame).
// Previously, it queried input_state_cb each time, but input_poll_cb is only
// called once per retro_run, so stale/inconsistent data caused autofire.
// Now we read from g_cached_joy0/g_cached_joy1 which are sampled once in Retro_PollEvent.
void read_joystick(int nr, unsigned int *dir, int *button)
{
    *dir = 0;
    *button = 0;

    // Skip if menu/keyboard active (v058: Settings and About submenus)
    if ((SHOWKEY==1) || (pauseg==1) || sf2000_menu_active || sf2000_settings_active || sf2000_about_active || sf2000_disk_shuffler_active)
        return;

    // v040: Check which physical controllers are assigned to this Amiga port
    // FrogJoy value 0 = "P1 Joy" -> Amiga Port 1
    // FrogJoy value 1 = "P0 Joy" -> Amiga Port 0
    // FrogJoy value 2 = "Mouse"  -> Amiga Port 0 mouse (not joystick)
    // NOTE: nr=0 means Port 1, nr=1 means Port 0 (getjoystate convention)
    int use_phys0 = 0, use_phys1 = 0;

    if (nr == 0) {
        // nr=0 means Port 1: check which physical controllers are set to "P1 Joy" (value 0)
        use_phys0 = (sf2000_frogjoy1 == 0);  // main SF2000 -> Port 1
        use_phys1 = (sf2000_frogjoy2 == 0);  // Data Frog -> Port 1
    } else {
        // nr=1 means Port 0: check which physical controllers are set to "P0 Joy" (value 1)
        use_phys0 = (sf2000_frogjoy1 == 1);  // main SF2000 -> Port 0
        use_phys1 = (sf2000_frogjoy2 == 1);  // Data Frog -> Port 0
    }

    // If neither controller is assigned to this port as joystick, return 0
    if (!use_phys0 && !use_phys1)
        return;

    int up = 0, down = 0, left = 0, right = 0;
    int fire = 0;

    // v042: Read from CACHED input (sampled once per retro_run in Retro_PollEvent)
    // This fixes frameskip autofire - all vsync_handler calls see the same stable input

    // Read from physical controller 0 (main SF2000) if assigned
    if (use_phys0 && g_cached_joy0.valid) {
        up    |= g_cached_joy0.up;
        down  |= g_cached_joy0.down;
        left  |= g_cached_joy0.left;
        right |= g_cached_joy0.right;
        fire  |= g_cached_joy0.fire;
    }

    // Read from physical controller 1 (Data Frog) if assigned
    if (use_phys1 && g_cached_joy1.valid) {
        up    |= g_cached_joy1.up;
        down  |= g_cached_joy1.down;
        left  |= g_cached_joy1.left;
        right |= g_cached_joy1.right;
        fire  |= g_cached_joy1.fire;
    }

    *dir = compute_joy1dir(up, down, left, right);
    // v038: A and B both work as fire (bit 0)
    *button = fire ? 1 : 0;
}

void init_joystick(void)
{
}

void close_joystick(void)
{
}




int STATUTON=-1;
#define RETRO_DEVICE_AMIGA_KEYBOARD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0)
#define RETRO_DEVICE_AMIGA_JOYSTICK RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)



void texture_uninit(void)
{

}

void texture_init(void)
{
    DIAG("10a.initsmfont");
    initsmfont();
    DIAG("10b.initmfont");
    initmfont();

    DIAG("10c.memset keys");
    memset(old_Key_State,0, sizeof(old_Key_State));
    memset(uae4all_keystate ,0, sizeof(uae4all_keystate));

    gmx=(retrow/2)-1;
    gmy=(retroh/2)-1;

    sf2000_apply_settings();

    DIAG("10d.texture OK");
}



extern unsigned amiga_devices[ 2 ];

extern void vkbd_key(int key,int pressed);

#include "keyboard.h"
#include "keybuf.h"
#include "libretro-keymap.h"

typedef struct {
    char norml[NLETT];
    char shift[NLETT];
    int val;
    int box;
    int color;
} Mvk;

extern Mvk MVk[NPLGN*NLIGN*2];

void retro_key_down(int key)
{
    int iAmigaKeyCode = keyboard_translation[key];

    if (iAmigaKeyCode >= 0)
        if (!uae4all_keystate[iAmigaKeyCode])
        {
            uae4all_keystate[iAmigaKeyCode] = 1;
            record_key(iAmigaKeyCode << 1);
        }
}

void retro_key_up(int key)
{
    int iAmigaKeyCode = keyboard_translation[key];

    if (iAmigaKeyCode >= 0)
    {
        uae4all_keystate[iAmigaKeyCode] = 0;
        record_key((iAmigaKeyCode << 1) | 1);
    }
}


void vkbd_key(int key,int pressed)
{
    int key2=key;

    if(pressed){
        if(SHIFTON==1){
            uae4all_keystate[AK_LSH] = 1;
            record_key((AK_LSH << 1));
        }
        uae4all_keystate[key2] = 1;
        record_key(key2 << 1);
    }
    else {

        if(SHIFTON==1){
            uae4all_keystate[AK_LSH] = 0;
            record_key((AK_LSH << 1) | 1);
        }
        uae4all_keystate[key2] = 0;
        record_key((key2 << 1) | 1);
    }
}


void retro_virtualkb(void)
{
    int i;
    static int oldi=-1;
    static int vkx=0,vky=0;

    int page= (NPAGE==-1) ? 0 : NPLGN*NLIGN;

    if(oldi!=-1)
    {
       vkbd_key(oldi,0);
       oldi=-1;
    }

    if(SHOWKEY==1)
    {
        static int vkflag[5]={0,0,0,0,0};

        if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) && vkflag[0]==0 )
            vkflag[0]=1;
        else if (vkflag[0]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) )
        {
            vkflag[0]=0;
            vky -= 1;
            if(vky<0)
                vky=NLIGN-1;

            while(MVk[(vky*NPLGN)+vkx+page].box==0){
                vkx -= 1;
                if(vkx<0)
                    vkx=NPLGN-1;
            }
        }

        if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) && vkflag[1]==0 )
            vkflag[1]=1;
        else if (vkflag[1]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) )
        {
             vkflag[1]=0;
             vky += 1;
             if(vky>NLIGN-1)
                 vky=0;

             while(MVk[(vky*NPLGN)+vkx+page].box==0){
                 vkx -= 1;
                 if(vkx<0)
                     vkx=NPLGN-1;
             }
        }

        if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) && vkflag[2]==0 )
           vkflag[2]=1;
        else if (vkflag[2]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) )
        {
            vkflag[2]=0;
            vkx -= 1;
            if(vkx<0)
                vkx=NPLGN-1;

            while(MVk[(vky*NPLGN)+vkx+page].box==0){
                vkx -= 1;
                if(vkx<0)
                    vkx=NPLGN-1;
            }
        }

        if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) && vkflag[3]==0 )
            vkflag[3]=1;
        else if (vkflag[3]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) )
        {
            vkflag[3]=0;
            vkx += 1;
	    if(vkx>NPLGN-1)
                vkx=0;

            while(MVk[(vky*NPLGN)+vkx+page].box==0){
                vkx += 1;
                if(vkx>NPLGN-1)
                    vkx=0;
            }

        }

        if(vkx<0)vkx=NPLGN-1;
        if(vkx>NPLGN-1)vkx=0;
        if(vky<0)vky=NLIGN-1;
        if(vky>NLIGN-1)vky=0;

        virtual_kdb(( char *)gfx_mem,vkx,vky);

        i=RETRO_DEVICE_ID_JOYPAD_A;
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i)  && vkflag[4]==0)
            vkflag[4]=1;
        else if( !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i)  && vkflag[4]==1)
        {
            vkflag[4]=0;
            i=check_vkey2(vkx,vky);

            if(i==-1){
                oldi=-1;
	    }
            if(i==-2)
            {
                NPAGE=-NPAGE;oldi=-1;
            }
            else if(i==-3)
            {
                KCOL=-KCOL;
                oldi=-1;
            }
            else if(i==-4)
            {
                oldi=-1;
                SHOWKEY=-SHOWKEY;
            }
            else if(i==-5)
            {
                oldi=-1;
            }
            else if(i==-6)
            {
                extern void retro_shutdown_core(void);
                retro_shutdown_core();
                oldi=-1;
            }
            else if(i==-7)
            {
                oldi=-1;
            }
            else if(i==-8)
            {
                oldi=-1;
            }
            else
            {
                if(i==AK_LSH)
                {
                    SHIFTON=-SHIFTON;
                    oldi=-1;
                }
                else if(i==0x27)
                {
                    oldi=-1;
                }
                else if(i==-12)
                {
                    oldi=-1;
                }
                else if(i==-13)
                {
                    sf2000_menu_active = 1;
                    pauseg = 1;
                    menu_first_frame = 1;
                    SHOWKEY = -1;
                    oldi=-1;
                }
                else if(i==-14)
                {
                    SHOWKEY=-SHOWKEY;
                    oldi=-1;
                }
                else
                {
                    oldi=i;
                    vkbd_key(oldi,1);
                }
            }
        }
    }
}


void Process_keyboard()
{
    int i;

    for(i=0;i<320;i++)
        Key_State[i]=input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0,i) ? 0x80: 0;

    if(memcmp( Key_State,old_Key_State , sizeof(Key_State) ) )
        for(i=0;i<320;i++)
            if(Key_State[i] && Key_State[i]!=old_Key_State[i]  )
            {
                if(i==RETROK_F12){
                    continue;
                }
                retro_key_down(i);

            }
            else if ( !Key_State[i] && Key_State[i]!=old_Key_State[i]  )
            {
                if(i==RETROK_F12){
                    continue;
                }
                retro_key_up(i);
            }

    memcpy(old_Key_State,Key_State , sizeof(Key_State) );
}

extern int lastmx, lastmy, newmousecounters;
extern int buttonstate[3];
extern int joy1button;
extern unsigned int joy1dir;

int Retro_PollEvent()
{
    static char vbt[16]={0x10,0x00,0x00,0x00,0x01,0x02,0x04,0x08,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

    int i;

    input_poll_cb();

    // v042: Cache input state for both physical controllers ONCE per retro_run frame
    // This prevents autofire caused by frameskip where vsync_handler calls read_joystick
    // multiple times per retro_run, but input_poll_cb only samples once
    {
        // Physical controller 0 (main SF2000)
        int raw_up    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        int raw_down  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        int raw_left  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        int raw_right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        int tmp_up, tmp_down, tmp_left, tmp_right;
        apply_dpad_mode(raw_up, raw_down, raw_left, raw_right, &tmp_up, &tmp_down, &tmp_left, &tmp_right);

        int x_btn = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
        if (x_btn) tmp_up = 1;

        g_cached_joy0.up = tmp_up;
        g_cached_joy0.down = tmp_down;
        g_cached_joy0.left = tmp_left;
        g_cached_joy0.right = tmp_right;
        g_cached_joy0.fire = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) |
                            input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
        g_cached_joy0.valid = 1;

        // Physical controller 1 (Data Frog)
        raw_up    = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        raw_down  = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        raw_left  = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        raw_right = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        apply_dpad_mode(raw_up, raw_down, raw_left, raw_right, &tmp_up, &tmp_down, &tmp_left, &tmp_right);

        x_btn = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
        if (x_btn) tmp_up = 1;

        g_cached_joy1.up = tmp_up;
        g_cached_joy1.down = tmp_down;
        g_cached_joy1.left = tmp_left;
        g_cached_joy1.right = tmp_right;
        g_cached_joy1.fire = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) |
                            input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
        g_cached_joy1.valid = 1;
    }

    // v035: Auto CPU fix - after 1 minute, open menu, toggle CPU, close
    // This fixes a bug where emulation is 40% slower until user manually touches CPU setting
    if (!sf2000_auto_fix_done && !sf2000_menu_active && pauseg == 0) {
        sf2000_frame_counter++;

        if (sf2000_frame_counter >= SF2000_AUTO_FIX_FRAMES) {
            // Time to trigger auto-fix
            sf2000_auto_fix_state = 1;  // menu will open
            sf2000_menu_active = 1;
            pauseg = 1;
            menu_first_frame = 1;
            sf2000_menu_item = 4;  // Position on CPU item
        }
    }

    // v035: Handle auto-fix state machine
    if (sf2000_auto_fix_state == 1 && sf2000_menu_active) {
        // Menu just opened for auto-fix - toggle CPU value
        if (sf2000_cpu_timing == 1) {
            sf2000_cpu_timing = 2;
        } else {
            sf2000_cpu_timing--;
        }
        sf2000_apply_settings();
        sf2000_auto_fix_state = 2;  // CPU changed
    }

    if (sf2000_auto_fix_state == 2) {
        // CPU changed - now close menu
        sf2000_menu_active = 0;
        pauseg = 0;
        menu_first_frame = 1;
        sf2000_auto_fix_state = 0;
        sf2000_auto_fix_done = 1;  // Don't do this again
    }

    // Menu handling
    if (sf2000_menu_active) {
        sf2000_handle_menu_input();
        return 1;
    }

    // START toggles menu
    int start_pressed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    if (start_pressed) {
        if (!menu_start_held) {
            sf2000_menu_active = 1;
            pauseg = 1;
            menu_first_frame = 1;
            menu_start_held = 1;
        }
    } else {
        menu_start_held = 0;
    }

    // v068: L+R INSTANT toggle for mouse emulation (restored from v053)
    // Quick toggle without holding - just press L+R together
    {
        static int lr_combo_held = 0;
        int l_btn = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
        int r_btn = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
        if (l_btn && r_btn) {
            if (!lr_combo_held) {
                // Toggle mouse emulation - this also updates FrogJoy1 setting
                // so menu shows correct state
                if (MOUSE_EMULATED == 1) {
                    // Currently mouse - switch to joystick
                    MOUSE_EMULATED = -1;
                    sf2000_frogjoy1 = 0;  // P1 Joy mode
                } else {
                    // Currently joystick - switch to mouse
                    MOUSE_EMULATED = 1;
                    second_joystick_enable = 0;
                    sf2000_frogjoy1 = 2;  // Mouse mode
                }
                lr_combo_held = 1;
            }
        } else {
            lr_combo_held = 0;
        }
    }

    int mouse_l;
    int mouse_r;
    int16_t rmouse_x,rmouse_y;
    rmouse_x=rmouse_y=0;

    if(SHOWKEY==-1 && pauseg==0)
    {
        Process_keyboard();

        if(second_joystick_enable)
        {

        }
        else
        {
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0,RETRO_DEVICE_ID_JOYPAD_B) ||
                input_state_cb(1, RETRO_DEVICE_JOYPAD, 0,RETRO_DEVICE_ID_JOYPAD_A))
            {
                LOGI("Switch to joystick mode for Port 0.\n");
                second_joystick_enable = 1;
            }
        }
    }
    else
    {
    }


    i=RETRO_DEVICE_ID_JOYPAD_SELECT;
    if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) && mbt[i]==0 )
    {
        mbt[i]=1;
    }
    else if ( mbt[i]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) )
    {
        mbt[i]=0;
        SHOWKEY=-SHOWKEY;
    }

    i=RETRO_DEVICE_ID_JOYPAD_Y;
    if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) && mbt[i]==0 )
        mbt[i]=1;
    else if ( mbt[i]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) )
    {
        mbt[i]=0;
        changedisk((bool) true);
    }

    if(MOUSE_EMULATED==1 && SHOWKEY==-1 ){

        if(pauseg!=0 )return 1;

        // v068: Always use controller 0 for mouse (simplified from v053)
        // This ensures mouse works regardless of FrogJoy settings
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))rmouse_x += PAS;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT ))rmouse_x -= PAS;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN ))rmouse_y += PAS;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP   ))rmouse_y -= PAS;
        mouse_l=input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
        mouse_r=input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    }
    else {

        mouse_wu = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
        mouse_wd = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);
        rmouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        rmouse_y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        mouse_l  = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
        mouse_r  = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
    }

    int analog_deadzone=0;
    unsigned int opt_analogmouse_deadzone = 20;
    analog_deadzone = (opt_analogmouse_deadzone * 32768 / 100);
    int analog_right[2]={0};
    analog_right[0] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
    analog_right[1] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));

    if (abs(analog_right[0]) > analog_deadzone)
        rmouse_x += analog_right[0] * 10 *  0.7 / (32768 );

    if (abs(analog_right[1]) > analog_deadzone)
        rmouse_y += analog_right[1] * 10 *  0.7 / (32768 );

    mouse_l    |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
    mouse_r    |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);

    static int mmbL=0,mmbR=0;

    if(mmbL==0 && mouse_l){
        mmbL=1;
    }
    else if(mmbL==1 && !mouse_l) {
        mmbL=0;
    }

    if(mmbR==0 && mouse_r){
        mmbR=1;
    }
    else if(mmbR==1 && !mouse_r) {
        mmbR=0;
    }

    gmx+=rmouse_x;
    gmy+=rmouse_y;
    if(gmx<0)gmx=0;
    if(gmx>retrow-1)gmx=retrow-1;
    if(gmy<0)gmy=0;
    if(gmy>retroh-1)gmy=retroh-1;

    lastmx +=rmouse_x;
    lastmy +=rmouse_y;
    newmousecounters=1;

    // v054: Read fire buttons from correct controller based on FrogJoy settings
    int fire_a_0 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    int fire_b_0 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    int fire_a_1 = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    int fire_b_1 = input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);

    // v036: Block joystick/buttons when keyboard is active (SHOWKEY==1)
    // v058: Also block when Settings or About submenus are active
    if (pauseg==0 && !sf2000_menu_active && !sf2000_settings_active && !sf2000_about_active && !sf2000_disk_shuffler_active && SHOWKEY != 1)
    {
        // v034: FrogJoy system
        // L/R buttons ALWAYS control mouse buttons (for cracked games)
        buttonstate[0] = mmbL;  // L = LMB
        buttonstate[2] = mmbR;  // R = RMB

        // v073: Handle FrogJoy1 mouse mode (controller 0) - A=RMB, B=LMB
        if (sf2000_frogjoy1 == 2) {
            // FrogJoy1 = Mouse mode: D-pad controls mouse, B=LMB, A=RMB
            buttonstate[0] |= fire_b_0;  // B = LMB
            buttonstate[2] |= fire_a_0;  // A = RMB
        }

        // v073: Handle FrogJoy2 mouse mode (controller 1) - A=RMB, B=LMB
        if (sf2000_frogjoy2 == 2) {
            // FrogJoy2 = Mouse mode: D-pad controls mouse, B=LMB, A=RMB
            buttonstate[0] |= fire_b_1;  // B = LMB
            buttonstate[2] |= fire_a_1;  // A = RMB
        }

        // Handle FrogJoy1 joystick modes
        if (sf2000_frogjoy1 == 0) {
            // FrogJoy1 = P1 Joy: D-pad controls port 1 joystick, A/B = fire
            joy1button = (fire_a_0 | fire_b_0) ? 1 : 0;

            int raw_up    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
            int raw_down  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
            int raw_left  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
            int raw_right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
            int j_x       = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);

            int j_up, j_down, j_left, j_right;
            apply_dpad_mode(raw_up, raw_down, raw_left, raw_right, &j_up, &j_down, &j_left, &j_right);

            if (j_x) j_up = 1;

            joy1dir = compute_joy1dir(j_up, j_down, j_left, j_right);
        } else {
            // FrogJoy1 is Mouse or P0 Joy - Port 1 joystick disabled
            joy1button = 0;
            joy1dir = 0;
        }
    }

    return 1;
}
