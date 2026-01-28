/*
 * Uae4all libretro core implementation
 * (c) Chips 2022
 *
 * v157: Test - NO menu patch, but using our linker script
 */

#include "libretro.h"
#include "libretro-core.h"

#include "sf2000_diag.h"

/* ============================================================
 * v157: MENU PATCH DISABLED FOR TESTING
 * Testing if our linker script alone causes the freeze
 * ============================================================ */
#if 0  /* DISABLED FOR v157 TEST */
#ifdef SF2000

/* MIPS instruction encoding macros */
#define MIPS_JAL(pfunc)  ((3 << 26) | ((uint32_t)(pfunc) >> 2 & ((1 << 26) - 1)))
#define PATCH_JAL(target, hook)  (*(uint32_t*)(target) = MIPS_JAL(hook))

/* External firmware symbols - defined in linker script */
extern "C" {
    extern char jal_run_emulator_menu;      /* Address of JAL instruction to patch */
    extern int run_emulator_menu(void);     /* Original menu function */
    extern void os_disable_interrupt(void); /* Firmware interrupt control */
    extern void os_enable_interrupt(void);
}

/* Flag to ensure we only patch once */
static int menu_patch_applied = 0;

/* Wrapper function - intercepts menu quit to prevent crash */
extern "C" int uae_menu_wrapper(void) {
    int result = run_emulator_menu();
    /* When user presses "quit" (result=1), return 0 instead to prevent crash */
    if (result == 1) {
        return 0;  /* Don't quit - just return to game */
    }
    return result;
}

/* Apply the menu patch - call once at startup */
static void apply_menu_patch(void) {
    if (menu_patch_applied) return;

    os_disable_interrupt();
    PATCH_JAL((uintptr_t)&jal_run_emulator_menu, uae_menu_wrapper);
    __builtin___clear_cache(&jal_run_emulator_menu, &jal_run_emulator_menu + 4);
    os_enable_interrupt();

    menu_patch_applied = 1;
}

#endif /* SF2000 */
#endif /* DISABLED */
/* ============================================================ */

#include "uae.h"

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "custom.h"
#include "xwin.h"

#include "m68k/uae/newcpu.h"

#include "savestate.h"

/* v158: splash_logo.h removed - no pre-boot */

#ifndef NO_ZLIB
#include "zlib.h"
#endif
#include "fsdb.h"
#include "filesys.h"
#include "autoconf.h"

#ifdef SF2000
/* strcasestr not available on bare metal - provide compat version */
#include <ctype.h>
static const char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}
#endif

/* v082: UAE4ALL save state functions for libretro serialize API
 * Now uses direct buffer I/O - no temp files! */
extern size_t save_state_to_buffer(void *buffer, size_t max_size);
extern bool restore_state_from_buffer(const void *buffer, size_t size);
extern int savestate_state;  /* UAE4ALL save state status global */
/* v145: fs_* externs and restore_state removed - no more temp file mechanism! */

unsigned int VIRTUAL_WIDTH=PREFS_GFX_WIDTH;
unsigned int retrow=PREFS_GFX_WIDTH;
unsigned int retroh=PREFS_GFX_HEIGHT;

extern char *gfx_mem;

// v094: Y-offset and V-stretch
extern int mainMenu_vpos;      // from retrogfx.cpp
extern int sf2000_y_offset;       // from core-mapper.cpp (-50 to +50)
extern int sf2000_pos_correction; // v101: from core-mapper.cpp (0=OFF, 1=ON)
extern int sf2000_v_stretch;      // from core-mapper.cpp (0 to 32)
extern int sf2000_show_leds;      // v109: from core-mapper.cpp (0=OFF, 1=ON)

// v097: Global variable for Y-offset accessible from drawing.cpp
int uae_render_y_offset = 0;

// v102: Global variable for LED position - base line for status bar (y_start + 240)
int uae_led_base_line = 240;

extern char uae4all_image_file[];
extern char uae4all_image_file2[];

extern int mainMenu_throttle;

extern void DISK_GUI_change (void);

static unsigned msg_interface_version = 0;


#define RETRO_DEVICE_AMIGA_KEYBOARD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_KEYBOARD, 0)
#define RETRO_DEVICE_AMIGA_JOYSTICK RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)

unsigned amiga_devices[ 2 ];

extern int SHIFTON,pauseg;
char RPATH[512];
extern int SHOWKEY;
extern int sf2000_menu_active;
extern int sf2000_settings_active;  // v058: Settings submenu (replaces Details)
extern int sf2000_about_active;     // v058: About submenu
extern int sf2000_disk_shuffler_active;  // v055: Disk Shuffler submenu
extern void sf2000_menu_overlay(char *pixels);
extern void sf2000_settings_overlay(char *pixels);  // v058: Settings submenu overlay
extern void sf2000_about_overlay(char *pixels);     // v058: About submenu overlay
extern void sf2000_disk_shuffler_overlay(char *pixels);  // v055: Disk Shuffler overlay
extern void sf2000_joy_debug_overlay(char *pixels);  // v025: joystick debug overlay (now no-op)
extern void sf2000_init_config(void);  // v067: Load per-game config at startup
extern void sf2000_apply_kickstart_override(void);  // v067: Apply kickstart after default_prefs
extern void sf2000_feedback_overlay(char *pixels);  // v058: Feedback message overlay

#include "cmdline.cpp"

extern void update_input(void);
extern void texture_init(void);
extern void texture_uninit(void);
extern void input_gui(void);
extern void retro_virtualkb(void);

extern void flush_audio(void);

const char *retro_save_directory;
const char *retro_system_directory;
const char *retro_content_directory;

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;

// v105: Y-Stretch buffer (320 x 240 x 2 bytes = 153600 bytes)
static uint16_t stretch_buffer[320 * 240];

// v106: LED drawing after stretch - need gui_data for drive status
#include "gui.h"

// v109: Digit patterns for track numbers (from drawing.cpp)
// Each digit is 7x7 pixels, 'x' = white pixel, '-' = transparent
static const char *led_numbers =
"------ ------ ------ ------ ------ ------ ------ ------ ------ ------ "
"-xxxxx ---xx- -xxxxx -xxxxx -x---x -xxxxx -xxxxx -xxxxx -xxxxx -xxxxx "
"-x---x ----x- -----x -----x -x---x -x---- -x---- -----x -x---x -x---x "
"-x---x ----x- -xxxxx -xxxxx -xxxxx -xxxxx -xxxxx ----x- -xxxxx -xxxxx "
"-x---x ----x- -x---- -----x -----x -----x -x---x ---x-- -x---x -----x "
"-xxxxx ----x- -xxxxx -xxxxx -----x -xxxxx -xxxxx ---x-- -xxxxx -xxxxx "
"------ ------ ------ ------ ------ ------ ------ ------ ------ ------ ";

#define LED_NUM_WIDTH 7
#define LED_NUM_HEIGHT 7

// v109: Draw a single digit on stretch_buffer at (x,y)
static void draw_digit_on_stretch(int x, int y, int digit) {
    if (digit < 0 || digit > 9) return;

    for (int row = 0; row < LED_NUM_HEIGHT; row++) {
        int screen_y = y + row;
        if (screen_y < 0 || screen_y >= 240) continue;

        const char *numptr = led_numbers + digit * LED_NUM_WIDTH + 10 * LED_NUM_WIDTH * row;

        for (int col = 0; col < LED_NUM_WIDTH; col++) {
            int screen_x = x + col;
            if (screen_x < 0 || screen_x >= 320) continue;

            if (numptr[col] == 'x') {
                stretch_buffer[screen_y * 320 + screen_x] = 0xFFFF;  // White
            }
        }
    }
}

// v109: Draw LEDs directly to stretch_buffer (fixed position at bottom-right)
// LEDs don't move with stretch and are always at screen bottom
// Includes track numbers!
static void draw_leds_on_stretch_buffer(void) {
    // LED dimensions (from drawing.cpp)
    const int TD_PADX = 20;
    const int TD_PADY = 2;
    const int TD_LED_WIDTH = 18;
    const int TD_LED_HEIGHT = 4;
    const int TD_WIDTH = 26;
    const int TD_TOTAL_HEIGHT = TD_PADY * 2 + LED_NUM_HEIGHT;  // 2+2+7=11

    // Position: bottom-right corner
    int base_x = 320 - TD_PADX - 5*TD_WIDTH + 100 - (TD_WIDTH*(NUM_DRIVES-1));
    int base_y = 240 - TD_TOTAL_HEIGHT;

    // Draw 5 LEDs: Power + 4 drives
    for (int led = 0; led < (NUM_DRIVES+1); led++) {
        int on;
        uint16_t color;
        int track = -1;

        if (led > 0) {
            // Drive LED
            on = gui_data.drive_motor[led-1];
            color = on ? 0x07E0 : 0x0200;  // Green (bright/dim)
            track = gui_data.drive_track[led-1];  // v109: Get track number
        } else {
            // Power LED
            on = gui_data.powerled;
            color = on ? 0xF800 : 0x4000;  // Red (bright/dim)
        }

        // Draw LED rectangle
        int led_x = base_x + led * TD_WIDTH;
        for (int dy = 0; dy < TD_LED_HEIGHT; dy++) {
            int y = base_y + dy;
            if (y >= 0 && y < 240) {
                for (int dx = 0; dx < TD_LED_WIDTH; dx++) {
                    int x = led_x + dx;
                    if (x >= 0 && x < 320) {
                        stretch_buffer[y * 320 + x] = color;
                    }
                }
            }
        }

        // v109: Draw track number below LED (only for drives, not power)
        if (track >= 0) {
            int num_offs = (TD_WIDTH - 2 * LED_NUM_WIDTH) / 2 - 4;
            int num_y = base_y + TD_PADY;
            draw_digit_on_stretch(led_x + num_offs, num_y, track / 10);
            draw_digit_on_stretch(led_x + num_offs + LED_NUM_WIDTH, num_y, track % 10);
        }
    }
}

struct zfile *retro_deserialize_file = NULL;
static size_t save_state_file_size = 0;

/* v145: Delayed load mechanism REMOVED!
 * The v123 mechanism wrote 13MB+ temp file to SD card every load.
 * This was wasteful and the file was never deleted (no fs_unlink in firmware).
 * Now we always use direct buffer I/O - simpler and no card waste. */

int libretroreset = 1;

// Amiga default kickstarts

#define A500_ROM        "kick13.rom"


#ifdef _WIN32
#define RETRO_PATH_SEPARATOR            "\\"
#else
#define RETRO_PATH_SEPARATOR            "/"
#endif

void path_join(char* out, const char* basedir, const char* filename)
{
   snprintf(out, 64, "%s%s%s", basedir, RETRO_PATH_SEPARATOR, filename);
}


static void trimwsa (char *s)
{
  /* Delete trailing whitespace.  */
  int len = strlen (s);
  while (len > 0 && strcspn (s + len - 1, "\t \r\n") == 0)
    s[--len] = '\0';
}

// Very light management of .uae file...

void cfgfile_load(const char *filename)
{
   FILE * fh;
   char line[128];

   fh = fopen(filename,"rb");
   if (!fh)
      return;

   while (fgets (line, sizeof (line), fh) != 0)
   {
      if ((strlen(line) >= strlen("floppy") + 3))
         if (!strncmp(line, "floppy", 6))
         {
            if ((line[6] == '0') && (line[7] == '='))
            {
               strncpy(uae4all_image_file, &line[8], 127);
               trimwsa(uae4all_image_file);
            }
            if ((line[6] == '1') && (line[7] == '='))
            {
               strncpy(uae4all_image_file2, &line[8], 127);
               trimwsa(uae4all_image_file2);
            }
         }   
   }
   fclose(fh);
}



void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

  static const struct retro_controller_description p1_controllers[] = {
    { "AMIGA Joystick", RETRO_DEVICE_AMIGA_JOYSTICK },
    { "AMIGA Keyboard", RETRO_DEVICE_AMIGA_KEYBOARD },
  };
  static const struct retro_controller_description p2_controllers[] = {
    { "AMIGA Joystick", RETRO_DEVICE_AMIGA_JOYSTICK },
    { "AMIGA Keyboard", RETRO_DEVICE_AMIGA_KEYBOARD },
  };

  static const struct retro_controller_info ports[] = {
    { p1_controllers, 2  }, // port 1
    { p2_controllers, 2  }, // port 2
    { NULL, 0 }
  };

  cb( RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports );

  struct retro_variable variables[] = {
//      { "uae4all_fastmem",        "Fast Mem; None|1 MB|2 MB|4 MB|8 MB", },
//      { "uae4all_resolution",     "Internal resolution; 320x240", },
//      { "uae4all_leds_on_screen", "Leds on screen; on|off", },
//      { "uae4all_floppy_speed",   "Floppy speed; 100|200|400|800", },
      { "uae4all_throttle",   "Optimize level; none|1|2|3|4|5", },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}


void Retro_Msg(const char * msg_str)
{
   if (msg_interface_version >= 1)
   {
      struct retro_message_ext msg = {
         msg_str,
         3000,
         3,
         RETRO_LOG_WARN,
         RETRO_MESSAGE_TARGET_ALL,
         RETRO_MESSAGE_TYPE_NOTIFICATION,
         -1
      };
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
   }
   else
   {
      struct retro_message msg = {
         msg_str,
         180
      };
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
   }
}


void update_prefs_retrocfg(void)
{
   struct retro_variable var;

   var.key = "uae4all_throttle";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "none")  == 0)
         mainMenu_throttle = 0;
      else
         mainMenu_throttle = atoi(var.value);
      
   }

#if 0

   var.key = "uae4all_resolution";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char *pch;
      char str[100];
      snprintf(str, sizeof(str), var.value);

      pch = strtok(str, "x");
      if (pch)
         retrow = strtoul(pch, NULL, 0);
      pch = strtok(NULL, "x");
      if (pch)
         retroh = strtoul(pch, NULL, 0);

      prefs->gfx_size.width  = retrow;
      prefs->gfx_size.height = retroh;
      prefs->gfx_resolution  = prefs->gfx_size.width > 600 ? 1 : 0;

      LOGI("[libretro-uae4all]: Got size: %u x %u.\n", retrow, retroh);

      VIRTUAL_WIDTH = retrow;
      texture_init();

   }

   var.key = "uae4all_leds_on_screen";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "on")  == 0) prefs->leds_on_screen = 1;
      if (strcmp(var.value, "off") == 0) prefs->leds_on_screen = 0;
   }

   var.key = "uae4all_fastmem";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "None") == 0)
      {
         prefs->fastmem[0].size = 0;
      }
      if (strcmp(var.value, "1 MB") == 0)
      {
         prefs->fastmem[0].size = 0x100000;
      }
      if (strcmp(var.value, "2 MB") == 0)
      {
         prefs->fastmem[0].size = 0x100000 * 2;
      }
      if (strcmp(var.value, "4 MB") == 0)
      {
         prefs->fastmem[0].size = 0x100000 * 4;
      }
      if (strcmp(var.value, "8 MB") == 0)
      {
         prefs->fastmem[0].size = 0x100000 * 8;
      }
   }


   var.key = "uae4all_floppy_speed";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      prefs->floppy_speed=atoi(var.value);
   }
#endif

   if (strcasestr(RPATH,".uae"))
      cfgfile_load(RPATH);

   // Set default kickstart (kick13.rom)
   path_join(romfile, retro_system_directory, A500_ROM);

   // v067: Override kickstart if per-game config specified different one
   sf2000_apply_kickstart_override();
}


void retro_shutdown_core(void)
{
   LOGI("SHUTDOWN\n");

   texture_uninit();
   environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

void retro_reset(void)
{
   libretroreset = 1;
   uae_reset();
}

void retro_init(void)
{
   DIAG("1.retro_init");

/* v157: Menu patch DISABLED for testing
#ifdef SF2000
   DIAG("1a.MENU_PATCH");
   apply_menu_patch();
#endif
*/

   const char *system_dir = NULL;

   msg_interface_version = 0;
   DIAG("2.GET_MSG_VER");
   environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &msg_interface_version);

   DIAG("3.GET_SYSDIR");
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
   {
      retro_system_directory=system_dir;
   }

   const char *content_dir = NULL;

   DIAG("4.GET_CONTDIR");
   if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir)
   {
      retro_content_directory=content_dir;
   }

   const char *save_dir = NULL;

   DIAG("5.GET_SAVEDIR");
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
   {
      retro_save_directory = *save_dir ? save_dir : retro_system_directory;
   }
   else
   {
      retro_save_directory=retro_system_directory;
   }

   DIAG("6.SET_PIXFMT");
#ifndef RENDER16B
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
#endif

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      DIAG("PIXFMT FAIL!");
      exit(0);
   }

   DIAG("7.SET_SERIAL_Q");
   static uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
   environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);

   DIAG("8.INPUT_DESC");
   struct retro_input_descriptor inputDescriptors[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3" }
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &inputDescriptors);

   DIAG("9.MEMSET_KEY");
   memset(Key_State,0,512);

   DIAG("10.texture_init");
   texture_init();

   DIAG("11.retro_init OK");
}

extern void main_exit();
void retro_deinit(void)
{
   texture_uninit();
   uae_quit ();

   LOGI("Retro DeInit\n");
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}


void retro_set_controller_port_device( unsigned port, unsigned device )
{
  if ( port < 2 )
  {
    amiga_devices[ port ] = device;

    LOGI(" (%d)=%d \n",port,device);
  }
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "uae4all";
   info->library_version  = UAE_VERSION;
   info->valid_extensions = "adf|adz|zip";
   info->need_fullpath    = true;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   //FIXME handle vice PAL/NTSC
   struct retro_game_geometry geom = { retrow, retroh, retrow, retroh,4.0 / 3.0 };
   struct retro_system_timing timing = { 50.0, 44100.0 };

   info->geometry = geom;
   info->timing   = timing;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}


void retro_audiocb(signed short int *sound_buffer,int sndbufsize)
{
    if(pauseg==0)
        if (audio_batch_cb)
            audio_batch_cb(sound_buffer, sndbufsize);
}


extern int Retro_PollEvent();
extern unsigned long sample_evtime, scaled_sample_evtime;

void retro_run(void)
{
   int x;
   bool updated = false;
   static int Deffered = 0;

   /* v145: Delayed load mechanism REMOVED - no more temp file creation! */

   // v099: NULL CHECK - zapobiega crash gdy gfx_mem nie zainicjalizowany!
   extern char *gfx_mem;
   if (gfx_mem == NULL) {
      video_cb(NULL, retrow, retroh, retrow<<PIXEL_BYTES);
      return;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_prefs_retrocfg();

   // Too early event poll or savesate segfault since emu not initialized...
   if (Deffered == 0)
   {
      Deffered = 1;
   }
   else
   {
      if (Deffered == 1)
      {
#if 0
         // Save states
         // > Ensure that save state file path is empty,
         //   since we use memory based save states
         savestate_fname[0] = '\0';
         // > Get save state size
         //   Here we use initial size + 5%
         //   Should be sufficient in all cases
         // NOTE: It would be better to calculate the
         // state size based on current config parameters,
         // but while
         //   - currprefs.chipmem_size
         //   - currprefs.bogomem_size
         //   - currprefs.fastmem_size
         // account for *most* of the size, there are
         // simply too many other factors to rely on this
         // alone (i.e. mem size + 5% is fine in most cases,
         // but if the user supplies a custom uae config file
         // then this is not adequate at all). Untangling the
         // full set of values that are recorded is beyond
         // my patience...
         struct zfile *state_file = save_state("libretro", 0);

         if (state_file)
         {
            save_state_file_size  = (size_t)zfile_size(state_file);
            save_state_file_size += (size_t)(((float)save_state_file_size * 0.05f) + 0.5f);
            zfile_fclose(state_file);
         }
#endif
         Deffered = 2;
      }

      /* v082: No cleanup needed - buffer mode doesn't use temp files */

      Retro_PollEvent();
   }


   // v099: NIE używaj uae_render_y_offset! Offset tylko przez pointer arithmetic!
   extern overscan_settings_t overscan_config;
   uae_render_y_offset = 0;  // UAE zawsze renderuje te same linie Amiga

   // v017: Only run M68K if not paused (menu or keyboard)
   if(pauseg==0)
      m68k_go (1);

   // v101: Y-offset calculation with Position Correction support
   extern unsigned gfx_rowbytes;  // from retrogfx.cpp
   int y_start;

   if (sf2000_pos_correction) {
       // v103: Position Correction ON - direct y-offset mapping
       // sf2000_y_offset: 0 to 48, where 0=top of buffer, 48=bottom
       // (poprzednio y_start = 24 + y_offset, teraz y_start = y_offset)
       y_start = sf2000_y_offset;
       if (y_start < 0) y_start = 0;
       if (y_start > 48) y_start = 48;  // max: 288-240=48
   } else {
       // v101: Position Correction OFF - tryb v97 (bez offsetu)
       y_start = 0;  // Pokaż od góry bufora
   }

   // v102: Update LED base line for drawing.cpp - LEDs should be at screen bottom
   uae_led_base_line = y_start + 240;

   // Overlays rysują do TEJ SAMEJ części co będzie wysłana!
   // KLUCZOWE: overlay_ptr = gfx_mem + (y_start * gfx_rowbytes)
   // To znaczy overlays rysują do środkowej części 240 linii z 288
   char *overlay_ptr = gfx_mem + (y_start * gfx_rowbytes);

   if(sf2000_disk_shuffler_active) {
      sf2000_disk_shuffler_overlay(overlay_ptr);  // v055: Disk shuffler submenu
   } else if(sf2000_settings_active) {
      sf2000_settings_overlay(overlay_ptr);  // v058: Settings submenu
   } else if(sf2000_about_active) {
      sf2000_about_overlay(overlay_ptr);  // v058: About submenu
   } else if(sf2000_menu_active) {
      sf2000_menu_overlay(overlay_ptr);  // Main menu
   } else if(SHOWKEY==1) {
      retro_virtualkb();  // Keyboard overlay
   }

   // v058: Feedback message (always on top of emulation)
   sf2000_feedback_overlay(overlay_ptr);

   flush_audio();

   DISK_GUI_change();

   // v109: Y-Stretch with multiple levels
   // 0=OFF, 1=Small (skip/32), 2=Medium (skip/16), 3=Large (skip/12)
   // Menu and overlays are NOT stretched - only game graphics!
   int overlay_active = sf2000_menu_active || sf2000_settings_active ||
                        sf2000_about_active || sf2000_disk_shuffler_active || (SHOWKEY == 1);

   if (sf2000_v_stretch > 0 && sf2000_pos_correction && !overlay_active) {
       // Determine skip divisor based on stretch level
       int skip_div;
       switch (sf2000_v_stretch) {
           case 1: skip_div = 32; break;  // Small: ~247 lines
           case 2: skip_div = 16; break;  // Medium: ~255 lines
           case 3: skip_div = 12; break;  // Large: ~260 lines
           default: skip_div = 16; break;
       }

       // v109: max_available = how many lines available from y_start position
       // Buffer is 288 lines, we start at y_start, so we have (288 - y_start) lines
       int max_available = 288 - y_start - 1;

       // Copy lines with skip pattern
       uint16_t *src_base = (uint16_t*)overlay_ptr;
       for (int disp_line = 0; disp_line < 240; disp_line++) {
           int src_line = disp_line + (disp_line / skip_div);
           // Clamp to available buffer (don't read past gfx_mem end)
           if (src_line > max_available) src_line = max_available;
           uint16_t *src_row = src_base + (src_line * 320);
           uint16_t *dst_row = stretch_buffer + (disp_line * 320);
           for (int x = 0; x < 320; x++) {
               dst_row[x] = src_row[x];
           }
       }

       // v109: Draw LEDs on stretch_buffer AFTER stretch (fixed position)
       // Only draw if show_leds is ON
       if (sf2000_show_leds) {
           draw_leds_on_stretch_buffer();
       }

       video_cb(stretch_buffer, retrow, retroh, retrow << PIXEL_BYTES);
   } else {
       // v099: Wysyłaj tę samą część co overlays (ZERO copy!)
       video_cb(overlay_ptr, retrow, retroh, retrow << PIXEL_BYTES);
   }

}


bool retro_load_game(const struct retro_game_info *info)
{
   DIAG("retro_load_game() start");

   const char *full_path;

   full_path = info->path;

   strcpy(RPATH,full_path);

   /* v046: Save states now use libretro serialize API */

   // v058: Load saved config (kickstart, fastram, etc.) BEFORE pre_main
   DIAG("sf2000_init_config()");
   sf2000_init_config();

   DIAG("pre_main()");
   pre_main(RPATH);

   DIAG("retro_load_game() done");
   libretroreset = 1;

   quit_program = 2;

   /* v159: WARMUP LOOP - NO GRAPHICS, 6 sec (300 frames)
    * Just run Amiga in background, no splash screen displayed
    */
   {
       #define V159_WARMUP_FRAMES 300  /* ~6 sec (was 350 = ~7 sec) */

       for (int frame = 0; frame < V159_WARMUP_FRAMES; frame++) {
           /* Run Amiga one frame */
           if (pauseg == 0) {
               m68k_go(1);
           }
           /* Flush audio */
           flush_audio();
       }

       /* Reset Amiga after warmup - audio will be properly initialized */
       uae_reset();
   }

   return true;
}

void retro_unload_game(void)
{
#if 0
   // Ensure save state de-serialization file
   // is closed/NULL
   // Note: Have to do this here (not in retro_deinit())
   // since leave_program() calls zfile_exit()
   if (retro_deserialize_file)
   {
      zfile_fclose(retro_deserialize_file);
      retro_deserialize_file = NULL;
   }
#endif
   pauseg=0;
   leave_program ();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_PAL;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

/*
 * v082: Libretro Serialize API Implementation - DIRECT BUFFER I/O
 *
 * Like PicoDrive and SNES9x2002, we use direct buffer I/O.
 * NO temp files - save_state_to_buffer() and restore_state_from_buffer()
 * read/write directly to the libretro-provided buffer!
 */

/* Estimated maximum save state size for Amiga:
 * - Chip RAM: 512KB-2MB (uncompressed in buffer mode!)
 * - CPU state: ~1KB
 * - Custom chips: ~4KB
 * - Audio: ~1KB
 * - CIA: ~1KB
 * - Disk state: ~16KB
 * - Expansion: varies
 * v082: Keep 5MB for safety with uncompressed RAM
 */
#define UAE4ALL_STATE_SIZE (5 * 1024 * 1024)  /* 5 MB */

size_t retro_serialize_size(void)
{
   /* Return fixed size estimate - like PicoDrive */
   return UAE4ALL_STATE_SIZE;
}

bool retro_serialize(void *data, size_t size)
{
   DIAG("retro_serialize: saving state to buffer (direct I/O)");

   /* v082: Direct buffer I/O - no temp file!
    * save_state_to_buffer returns actual size written, or 0 on error */
   size_t actual_size = save_state_to_buffer(data, size);

   if (actual_size > 0 && actual_size <= size) {
      DIAG("retro_serialize: success");
      return true;
   }

   DIAG("retro_serialize: failed");
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   DIAG("retro_unserialize: restoring state from buffer (direct I/O)");

   /* v145: Delayed load mechanism REMOVED!
    * v123 wrote 13MB+ temp files to SD card that were never deleted.
    * Now we ALWAYS use direct buffer I/O - simpler and no card waste. */

   bool success = restore_state_from_buffer(data, size);

   if (success) {
      DIAG("retro_unserialize: success");
      return true;
   }

   DIAG("retro_unserialize: failed");
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

