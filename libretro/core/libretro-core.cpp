/*
 * Uae4all libretro core implementation
 * (c) Chips 2022
 */

#include "libretro.h"
#include "libretro-core.h"

#include "sf2000_diag.h"

#include "uae.h"

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "custom.h"

#include "m68k/uae/newcpu.h"

#include "savestate.h"

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

unsigned int VIRTUAL_WIDTH=PREFS_GFX_WIDTH;
unsigned int retrow=PREFS_GFX_WIDTH;
unsigned int retroh=PREFS_GFX_HEIGHT;

extern char *gfx_mem;

// v094: Y-offset and V-stretch
extern int mainMenu_vpos;      // from retrogfx.cpp
extern int sf2000_y_offset;    // from core-mapper.cpp (-50 to +50)
extern int sf2000_v_stretch;   // from core-mapper.cpp (0 to 32)

// v097: Global variable for Y-offset accessible from drawing.cpp
int uae_render_y_offset = 0;

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

struct zfile *retro_deserialize_file = NULL;
static size_t save_state_file_size = 0;

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
   info->library_version  = "v083";
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


   // v097: Set Y-offset for drawing.cpp (shifts which Amiga lines are rendered)
   uae_render_y_offset = sf2000_y_offset;

   // v017: Only run M68K if not paused (menu or keyboard)
   if(pauseg==0)
      m68k_go (1);

   // v058: Draw overlays (menu/settings/about/disk shuffler/keyboard)
   if(sf2000_disk_shuffler_active) {
      sf2000_disk_shuffler_overlay(gfx_mem);  // v055: Disk shuffler submenu
   } else if(sf2000_settings_active) {
      sf2000_settings_overlay(gfx_mem);  // v058: Settings submenu
   } else if(sf2000_about_active) {
      sf2000_about_overlay(gfx_mem);  // v058: About submenu
   } else if(sf2000_menu_active) {
      sf2000_menu_overlay(gfx_mem);  // Main menu
   } else if(SHOWKEY==1) {
      retro_virtualkb();  // Keyboard overlay
   }

   // v058: Feedback message (always on top of emulation)
   sf2000_feedback_overlay(gfx_mem);

   flush_audio();

   DISK_GUI_change();

   video_cb(gfx_mem,retrow,retroh,retrow<<PIXEL_BYTES);

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

   /* v082: Direct buffer I/O - no temp file!
    * restore_state_from_buffer returns true on success */
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

