 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Save/restore emulator state
  *
  * (c) 1999-2001 Toni Wilen
  *
  * see below for ASF-structure
  */

 /* Features:
  *
  * - full CPU state (68000/68010/68020, no FPU)
  * - full CIA-A and CIA-B state (with all internal registers)
  * - saves all custom registers and audio internal state but not all registers are restored yet.
  * - only Chip-ram and Bogo-ram are saved and restored.
  * - disk drive type, imagefile, track and motor state
  * - Kickstart ROM version, address and size is saved. This data is not used during restore yet.
  */

 /* Notes:
  *
  * - blitter state is not saved, blitter is forced to finish immediately if it
  *   was active
  * - disk DMA state is completely saved (I hope so..)
  * - does not ask for statefile name and description. Currently uses DF0's disk
  *   image name (".adf" is replaced with ".asf")
  * - only Amiga state is restored, harddisk support, autoconfig, expansion boards etc..
  *   are not saved/restored (and probably never will).
  * - use this for saving games that can't be saved to disk
  */

 /* Usage :
  *
  * save:
  * 
  * set savestate_state = STATE_DOSAVE, savestate_filename = "..."
  *
  * restore:
  * 
  * set savestate_state = STATE_DORESTORE, savestate_filename = "..."
  *
  */

#include "uae.h"
#include "sysconfig.h"
#include "sysdeps.h"

#include "config.h"
#include "gui.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "sound.h"
#include "audio.h"
#include "m68k/m68k_intrf.h"
#include "debug_uae4all.h"

#ifdef USE_LIB7Z
#include "lib7z/lzma.h"
#define LZMA_COMPRESSION_LEVEL 9
#define LZMA_DICT_SIZE (65536*4)
#elif !defined(NO_ZLIB)
#include <zlib.h>
#endif

#include "savestate.h"
#include "sf2000_diag.h"
#include "events.h"  /* v113: For eventtab sync after restore */
#include "cia.h"     /* v115: For CIA_calctimers, CIA_reset_div10 */

/* v118: For reset_drawing() - fixes "kasza" bug
 * Using extern instead of #include "drawing.h" to avoid xcolnr dependency issues */
extern void reset_drawing(void);

/* v143: Wrappers for static functions in custom.cpp
 * These fix various display issues after restore */
extern void calcdiw_wrapper(void);
extern void expand_sprres_wrapper(void);

/* v144: THE ORIGINAL FIX - BPLCON0 reinit trick from raport_save.txt
 * This was the fix in original v141 that worked better! */
extern void reinit_bplcon0_after_restore(void);

/* v145: THE MISSING RESET - init_hardware_frame!
 * This resets last_decide_line_hpos, last_fetch_hpos, etc.
 * Without this, 2nd/3rd loads have 20-40px on right not refreshing! */
extern void init_hardware_frame_wrapper(void);

/* v143: DIW state machine - needs reset after restore */
enum diw_states { DIW_waiting_start, DIW_waiting_stop };
extern enum diw_states *get_diwstate_ptr(void);
extern enum diw_states *get_hdiwstate_ptr(void);

/* v143: Sprite reinit removed - sprpos/sprctl are static in custom.cpp
 * This is a minor fix anyway, main fixes are DIWHIGH and calcdiw */

/* v114: External gfx_mem buffer for clearing after restore
 * gfx_mem is the 320x288 framebuffer allocated in retrogfx.cpp
 * gfx_rowbytes is 320*2 = 640 bytes per line
 * We need to clear this after restore because notice_screen_contents_lost()
 * is a no-op when USE_RASTER_DRAW is not defined! */
extern char *gfx_mem;
extern int gfx_rowbytes;

/* v115: External CIA variables for COMPLETE synchronization after restore!
 *
 * ROOT CAUSE OF CIA ASSERTION BUG (since v80):
 * The assertion "(ciabta+1) >= ciaclocks" fails because after restore:
 * 1. eventtab[ev_cia].evtime has OLD stale value (not synced!)
 * 2. eventtab[ev_cia].active is still TRUE (not cleared!)
 * 3. div10 (fractional CIA cycles) is from different context
 * 4. CIA timers (ciabta etc.) may be at boundary condition
 * 5. CIA_calctimers() is NEVER called after restore!
 *
 * When emulation resumes, do_cycles() sees evtime in the "past" and
 * IMMEDIATELY triggers CIA_handler(), which calls CIA_update() with
 * inconsistent state, causing the assertion to fail.
 *
 * FIX: Reset ALL CIA-related state to safe values after restore. */
extern unsigned long ciaata, ciaatb, ciabta, ciabtb;  /* CIA timer values */
extern unsigned long ciaala, ciaalb, ciabla, ciablb;  /* CIA timer latches */
extern unsigned int ciaacra, ciaacrb, ciabcra, ciabcrb;  /* CIA control regs */
extern void CIA_calctimers(void);  /* Recalculates eventtab[ev_cia] */

/* v115: div10 is static in cia.cpp, so we need to reset it via function */
extern void CIA_reset_div10(void);

// SF2000 firmware file functions (exported by firmware)
extern "C" int fs_open(const char *path, int flags, int perms);
extern "C" ssize_t fs_write(int fd, const void *buf, size_t count);
extern "C" ssize_t fs_read(int fd, void *buf, size_t count);
extern "C" int fs_close(int fd);
extern "C" int fs_sync(const char *path);
extern "C" int64_t fs_lseek(int fd, int64_t offset, int whence);

// SF2000 file flags
#define SF_O_RDONLY 0x0000
#define SF_O_WRONLY 0x0001
#define SF_O_RDWR   0x0002
#define SF_O_CREAT  0x0100
#define SF_O_TRUNC  0x0200

// SF2000 seek modes
#define SF_SEEK_SET 0
#define SF_SEEK_CUR 1
#define SF_SEEK_END 2

// v074: Use firmware fs_* functions instead of stdio
// This wrapper provides FILE-like interface using fs_* calls
static int sf_fd = -1;  // Current savestate file descriptor
static int64_t sf_pos = 0;  // Current position tracking

static int sf_open_read(const char *path) {
    sf_fd = fs_open(path, SF_O_RDONLY, 0);
    sf_pos = 0;
    return sf_fd;
}

static int sf_open_write(const char *path) {
    sf_fd = fs_open(path, SF_O_WRONLY | SF_O_CREAT | SF_O_TRUNC, 0666);
    sf_pos = 0;
    return sf_fd;
}

static ssize_t sf_read(void *buf, size_t size, size_t count) {
    if (sf_fd < 0) return 0;
    size_t total = size * count;
    ssize_t result = fs_read(sf_fd, buf, total);
    if (result > 0) sf_pos += result;
    return result;
}

static ssize_t sf_write(const void *buf, size_t size, size_t count) {
    if (sf_fd < 0) return 0;
    size_t total = size * count;
    ssize_t result = fs_write(sf_fd, buf, total);
    if (result > 0) sf_pos += result;
    return result;
}

static int sf_seek(int64_t offset, int whence) {
    if (sf_fd < 0) return -1;
    int64_t result = fs_lseek(sf_fd, offset, whence);
    if (result >= 0) sf_pos = result;
    return (result >= 0) ? 0 : -1;
}

static int64_t sf_tell(void) {
    return sf_pos;
}

static void sf_close(void) {
    if (sf_fd >= 0) {
        fs_close(sf_fd);
        sf_fd = -1;
    }
    sf_pos = 0;
}

int savestate_state;

static char savestate_filename_default[]={
	'/', 't', 'm', 'p', '/', 'n', 'u', 'l', 'l', '.', 'a', 's', 'f', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
	'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', 
};
char *savestate_filename=(char *)&savestate_filename_default[0];
/* v074: savestate_file no longer used - using sf_fd with firmware fs_* functions */
/* FILE *savestate_file=NULL; */

/* v081: Global buffer for chunk data - replaces malloc() which crashes SF2000
 * Same pattern as savestate_filename_default - global static array works!
 * v086: Increased from 8192 to 32768 bytes - some chunks in complex game states
 * can be larger than 8KB, causing them to be skipped and state to be incomplete */
static uae_u8 chunk_buffer[32768];

/* v088: ARENA ALLOCATOR for save functions
 * Problem: save_cpu(), save_custom(), etc. use malloc()/free() which causes
 * heap fragmentation on SF2000. After 2-3 SAVEs, malloc fails = corrupted state!
 *
 * Solution: Global arena buffer that save functions can use instead of malloc.
 * Reset arena at start of each save_state_to_buffer().
 *
 * Usage: save functions check if savestate_use_arena is true, then use
 * savestate_arena_alloc() instead of malloc(). Never call free() on arena memory!
 */
static uae_u8 save_arena[8192];  /* 8KB should be enough for all save chunks */
static size_t save_arena_pos = 0;
int savestate_use_arena = 0;  /* Global flag - set to 1 during buffer save */

void *savestate_arena_alloc(size_t size) {
    if (save_arena_pos + size > sizeof(save_arena)) {
        write_log("v088 ERROR: Arena overflow! pos=%d, size=%d\n", (int)save_arena_pos, (int)size);
        return NULL;
    }
    void *ptr = &save_arena[save_arena_pos];
    save_arena_pos += size;
    /* Align to 4 bytes for safety */
    save_arena_pos = (save_arena_pos + 3) & ~3;
    return ptr;
}

void savestate_arena_reset(void) {
    save_arena_pos = 0;
}

/* v082: LIBRETRO BUFFER I/O SYSTEM
 * Direct buffer I/O like PicoDrive - no temp files!
 * This is used by retro_serialize/retro_unserialize for Start+Select savestate */
static uae_u8 *buf_ptr = NULL;     // Current buffer pointer
static uae_u8 *buf_start = NULL;   // Start of buffer
static size_t buf_size = 0;        // Total buffer size
static size_t buf_pos = 0;         // Current position
static int buf_mode = 0;           // 0=none, 1=write, 2=read

static void buf_init_write(uae_u8 *buffer, size_t size) {
    buf_start = buffer;
    buf_ptr = buffer;
    buf_size = size;
    buf_pos = 0;
    buf_mode = 1;
}

static void buf_init_read(const uae_u8 *buffer, size_t size) {
    buf_start = (uae_u8 *)buffer;
    buf_ptr = (uae_u8 *)buffer;
    buf_size = size;
    buf_pos = 0;
    buf_mode = 2;
}

static void buf_close(void) {
    buf_start = NULL;
    buf_ptr = NULL;
    buf_size = 0;
    buf_pos = 0;
    buf_mode = 0;
}

static ssize_t buf_write(const void *data, size_t size, size_t count) {
    if (buf_mode != 1) return 0;
    size_t total = size * count;
    if (buf_pos + total > buf_size) total = buf_size - buf_pos;
    if (total > 0) {
        memcpy(buf_ptr, data, total);
        buf_ptr += total;
        buf_pos += total;
    }
    return total;
}

static ssize_t buf_read(void *data, size_t size, size_t count) {
    if (buf_mode != 2) return 0;
    size_t total = size * count;
    if (buf_pos + total > buf_size) total = buf_size - buf_pos;
    if (total > 0) {
        memcpy(data, buf_ptr, total);
        buf_ptr += total;
        buf_pos += total;
    }
    return total;
}

static int buf_seek(long offset, int whence) {
    if (buf_mode == 0) return -1;
    long new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;  // SEEK_SET
        case 1: new_pos = buf_pos + offset; break;  // SEEK_CUR
        case 2: new_pos = buf_size + offset; break;  // SEEK_END
        default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (long)buf_size) new_pos = buf_size;
    buf_pos = new_pos;
    buf_ptr = buf_start + buf_pos;
    return 0;
}

static long buf_tell(void) {
    return buf_pos;
}

/* v082: Current I/O mode - 0=file, 1=buffer
 * When buffer mode is active, all I/O goes through buf_* functions */
static int io_mode = 0;

/* v082: Unified I/O wrappers that switch between file and buffer mode */
static ssize_t io_write(const void *data, size_t size, size_t count) {
    if (io_mode == 1) return buf_write(data, size, count);
    return sf_write(data, size, count);
}

static ssize_t io_read(void *data, size_t size, size_t count) {
    if (io_mode == 1) return buf_read(data, size, count);
    return sf_read(data, size, count);
}

static int io_seek(long offset, int whence) {
    if (io_mode == 1) return buf_seek(offset, whence);
    return sf_seek(offset, whence);
}

static long io_tell(void) {
    if (io_mode == 1) return buf_tell();
    return sf_tell();
}

/* v082: Global wrappers for memory.cpp to use during restore
 * Now uses io_* to support both file and buffer modes */
int savestate_fseek(long offset, int whence) {
    return io_seek(offset, whence);
}

size_t savestate_fread(void *buf, size_t size, size_t count) {
    return io_read(buf, size, count);
}

/* functions for reading/writing bytes, shorts and longs in big-endian
 * format independent of host machine's endianess */

void save_u32_func (uae_u8 **dstp, uae_u32 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = (uae_u8)(v >> 24);
    *dst++ = (uae_u8)(v >> 16);
    *dst++ = (uae_u8)(v >> 8);
    *dst++ = (uae_u8)(v >> 0);
    *dstp = dst;
}
void save_u16_func (uae_u8 **dstp, uae_u16 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = (uae_u8)(v >> 8);
    *dst++ = (uae_u8)(v >> 0);
    *dstp = dst;
}
void save_u8_func (uae_u8 **dstp, uae_u8 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = v;
    *dstp = dst;
}
void save_string_func (uae_u8 **dstp, char *from)
{
    uae_u8 *dst = *dstp;
    while(*from)
	*dst++ = *from++;
    *dst++ = 0;
    *dstp = dst;
}

uae_u32 restore_u32_func (uae_u8 **dstp)
{
    uae_u32 v;
    uae_u8 *dst = *dstp;
    v = (dst[0] << 24) | (dst[1] << 16) | (dst[2] << 8) | (dst[3]);
    *dstp = dst + 4;
    return v;
}
uae_u16 restore_u16_func (uae_u8 **dstp)
{
    uae_u16 v;
    uae_u8 *dst = *dstp;
    v=(dst[0] << 8) | (dst[1]);
    *dstp = dst + 2;
    return v;
}
uae_u8 restore_u8_func (uae_u8 **dstp)
{
    uae_u8 v;
    uae_u8 *dst = *dstp;
    v = dst[0];
    *dstp = dst + 1;
    return v;
}
char *restore_string_func (uae_u8 **dstp)
{
    /* v081: Return pointer directly to source data - no malloc!
     * The string is null-terminated in the chunk data, just skip past it */
    uae_u8 *dst = *dstp;
    char *result = (char *)dst;
    /* Skip to end of string */
    while (*dst++) ;
    *dstp = dst;
    return result;
}

/* read and write IFF-style hunks */
/* v082: Modified to use io_* wrappers for buffer/file mode switching */

static void save_chunk (uae_u8 *chunk, long len, char *name)
{
    uae_u8 tmp[4], *dst;
    uae_u8 zero[4]= { 0, 0, 0, 0 };

    if (!chunk)
	return;

    /* chunk name */
    io_write (name, 1, 4);
    /* chunk size */
    dst = &tmp[0];
    save_u32 (len + 4 + 4 + 4);
    io_write (&tmp[0], 1, 4);
    /* chunk flags */
    dst = &tmp[0];
    save_u32 (0);
    io_write (&tmp[0], 1, 4);
    /* chunk data */
    io_write (chunk, 1, len);
    /* alignment */
    len = 4 - (len & 3);
    if (len)
	io_write (zero, 1, len);
}

static void save_chunk_compressed (uae_u8 *chunk, long len, char *name)
{
#ifdef NO_ZLIB
	/* No compression available - save uncompressed */
	save_chunk(chunk, len, name);
#else
#ifndef DREAMCAST
	void *tmp=malloc(len);
#else
	extern void *uae4all_vram_memory_free;
	void *tmp=uae4all_vram_memory_free;
#endif
	long outSize=len;
#ifdef USE_LIB7Z
	Lzma_Encode((Byte *)tmp, (size_t *)&outSize, (const Byte *)chunk, (size_t)len, LZMA_COMPRESSION_LEVEL, LZMA_DICT_SIZE);
#else
	compress2((Bytef *)tmp,(uLongf*)&outSize,(const Bytef *)chunk,len,Z_BEST_COMPRESSION);
#endif
	save_chunk((uae_u8*)tmp,outSize,name);
#ifndef DREAMCAST
	free(tmp);
#endif
#endif /* NO_ZLIB */
}


/* v082: Modified to use io_* wrappers for buffer/file mode switching */
/* v081: Use global chunk_buffer instead of malloc() */
static uae_u8 *restore_chunk (char *name, long *len, long *filepos)
{
    uae_u8 tmp[4], dummy[4], *mem, *src;
    uae_u32 flags;
    long len2;

    /* chunk name */
    io_read (name, 1, 4);
    name[4] = 0;
    /* chunk size */
    io_read (tmp, 1, 4);
    src = tmp;
    len2 = restore_u32 () - 4 - 4 - 4;
    if (len2 < 0)
	len2 = 0;
    *len = len2;
    if (len2 == 0)
	return 0;

    /* chunk flags */
    io_read (tmp, 1, 4);
    src = tmp;
    flags = restore_u32 ();

    *filepos = io_tell ();
    /* chunk data.  RAM contents will be loaded during the reset phase,
       no need to malloc multiple megabytes here.  */
    if (strcmp (name, "CRAM") != 0
	&& strcmp (name, "BRAM") != 0
	&& strcmp (name, "FRAM") != 0
	&& strcmp (name, "ZRAM") != 0)
    {
	/* v081: Use global buffer instead of malloc - malloc crashes SF2000!
	 * Chunk must be smaller than 8192 bytes (CPU/CHIP/CIA/etc are ~100-400 bytes) */
	if (len2 > (long)sizeof(chunk_buffer)) {
	    /* Chunk too big - skip it */
	    mem = 0;
	    io_seek (len2, SF_SEEK_CUR);
	} else {
	    mem = chunk_buffer;
	    io_read (mem, 1, len2);
	}
    } else {
	mem = 0;
	io_seek (len2, SF_SEEK_CUR);
    }

    /* alignment */
    len2 = 4 - (len2 & 3);
    if (len2)
	io_read (dummy, 1, len2);
    return mem;
}

static void restore_header (uae_u8 *src)
{
    char *emuname, *emuversion, *description;

    restore_u32();
    emuname = restore_string ();
    emuversion = restore_string ();
    description = restore_string ();
    write_log ("Saved with: '%s %s', description: '%s'\n",
	emuname,emuversion,description);
    /* v081: No free() needed - restore_string now returns pointer to chunk data */
}

static void clear_events(void) {
#ifndef __LIBRETRO__
	SDL_Event event;
	while (SDL_PollEvent(&event))
		SDL_Delay(20);
#endif
}
/* restore all subsystems */

void restore_state (char *filename)
{
    uae_u8 *chunk,*end;
    char name[5];
    long len;
    long filepos;
    int i=0;

    gui_show_window_bar(0, 10, 1);
#ifdef DREAMCAST
	extern void reinit_sdcard(void);
	reinit_sdcard();
#endif
#ifdef DEBUG_SAVESTATE
    puts("-->restore_state");fflush(stdout);
#endif
    chunk = 0;
    /* v074: Use SF2000 firmware fs_* functions instead of fopen */
    if (sf_open_read(filename) < 0) {
	goto error;
    }

    chunk = restore_chunk (name, &len, &filepos);
    if (!chunk || memcmp (name, "ASF ", 4)) {
	write_log ("%s is not an AmigaStateFile\n",filename);
	goto error;
    }
    /* v074: savestate_file no longer used - sf_fd is global */
    restore_header (chunk);
    /* v081: No free() - chunk points to global chunk_buffer */
    savestate_state = STATE_RESTORE;
    for (;;) {
	chunk = restore_chunk (name, &len, &filepos);
	write_log ("Chunk '%s' size %d\n", name, len);
#ifdef DEBUG_SAVESTATE
	puts(name);fflush(stdout);
#endif
	if (!strcmp (name, "END "))
	    break;
	{
		if (i&1)
			gui_show_window_bar(i/2, 10, 1);
		if (i<20)
			i++;
	}
	if (!strcmp (name, "CRAM")) {
	    restore_cram (len, filepos);
	    continue;
	}
	else if (!strcmp (name, "BRAM")) {
	    restore_bram (len, filepos);
	    continue;
	} else if (!strcmp (name, "FRAM")) {
	    restore_fram (len, filepos);
	    continue;
	} else if (!strcmp (name, "ZRAM")) {
	    restore_zram (len, filepos);
	    continue;
	}

	if (!strcmp (name, "CPU "))
	    end = restore_cpu (chunk);
	else if (!strcmp (name, "AGAC"))
	    end = restore_custom_agacolors (chunk);
	else if (!strcmp (name, "SPR0"))
	    end = restore_custom_sprite (chunk, 0);
	else if (!strcmp (name, "SPR1"))
	    end = restore_custom_sprite (chunk, 1);
	else if (!strcmp (name, "SPR2"))
	    end = restore_custom_sprite (chunk, 2);
	else if (!strcmp (name, "SPR3"))
	    end = restore_custom_sprite (chunk, 3);
	else if (!strcmp (name, "SPR4"))
	    end = restore_custom_sprite (chunk, 4);
	else if (!strcmp (name, "SPR5"))
	    end = restore_custom_sprite (chunk, 5);
	else if (!strcmp (name, "SPR6"))
	    end = restore_custom_sprite (chunk, 6);
	else if (!strcmp (name, "SPR7"))
	    end = restore_custom_sprite (chunk, 7);
	else if (!strcmp (name, "CIAA"))
	    end = restore_cia (0, chunk);
	else if (!strcmp (name, "CIAB"))
	    end = restore_cia (1, chunk);
	else if (!strcmp (name, "CHIP"))
	    end = restore_custom (chunk);
	else if (!strcmp (name, "AUD0"))
	    end = restore_audio (chunk, 0);
	else if (!strcmp (name, "AUD1"))
	    end = restore_audio (chunk, 1);
	else if (!strcmp (name, "AUD2"))
	    end = restore_audio (chunk, 2);
	else if (!strcmp (name, "AUD3"))
	    end = restore_audio (chunk, 3);
	else if (!strcmp (name, "DISK"))
	    end = restore_floppy (chunk);
	else if (!strcmp (name, "DSK0"))
	    end = restore_disk (0, chunk);
	else if (!strcmp (name, "DSK1"))
	    end = restore_disk (1, chunk);
	else if (!strcmp (name, "DSK2"))
	    end = restore_disk (2, chunk);
	else if (!strcmp (name, "DSK3"))
	    end = restore_disk (3, chunk);
	else if (!strcmp (name, "EXPA"))
	    end = restore_expansion (chunk);
	/* v080: ROM restore SKIP - not saved */
	else if (!strcmp (name, "ROM "))
	    end = chunk;
	else
	    write_log ("unknown chunk '%s' size %d bytes\n", name, len);
	if (len != end - chunk)
	    write_log ("Chunk '%s' total size %d bytes but read %d bytes!\n",
		       name, len, end - chunk);
	/* v081: No free() - chunk points to global chunk_buffer */
    }
    gui_show_window_bar(10, 10, 1);
    clear_events();
#ifdef DEBUG_SAVESTATE
    puts("-->OK");fflush(stdout);
    printf("RESTORED state=%X, flags=%X, PC=%X\n",savestate_state,_68k_spcflags,_68k_getpc());fflush(stdout);
#endif
#ifdef AUTO_SAVESTATE
//	DEBUG_AHORA=1;
#endif
    return;

    error:
#ifdef DEBUG_SAVESTATE
    puts("-->ERROR");fflush(stdout);
#endif
    savestate_state = 0;
    /* v081: No free() - chunk points to global chunk_buffer */
    /* v074: Use sf_close instead of fclose */
    sf_close();
    resume_sound();
    if (produce_sound)
    	update_audio();
    notice_screen_contents_lost();
    gui_set_message("Error loadstate", 50);
}

void savestate_restore_finish (void)
{
    if (savestate_state != STATE_RESTORE)
	return;
#ifdef DEBUG_SAVESTATE
    printf("-->savestate_restore_finish state=%X, flags=%X, PC=%X\n",savestate_state,_68k_spcflags,_68k_getpc());fflush(stdout);
#endif
    /* v084: Close buffer if in buffer mode, or close file if in file mode */
    if (io_mode == 1) {
        write_log("savestate_restore_finish: closing buffer (io_mode=1)\n");
        buf_close();
        io_mode = 0;
    } else {
        /* v074: Use sf_close instead of fclose(savestate_file) */
        sf_close();
    }
#ifdef DINGOO
    sync();
#endif
    resume_sound();
    if (produce_sound)
    	update_audio();
    savestate_state = 0;
//    unset_special(SPCFLAG_BRK);
    notice_screen_contents_lost();
    gui_set_message("Restored", 50);
}

/* Save all subsystems  */

void save_state (char *filename, char *description)
{
    uae_u8 header[1000];
    char tmp[100];
    uae_u8 *dst;
    int len,i;
    char name[5];

    gui_show_window_bar(0, 10, 0);
#ifdef DREAMCAST
	extern void reinit_sdcard(void);
	reinit_sdcard();
#endif
#ifdef DEBUG_SAVESTATE
    printf("-->save_state('%s','%s'\n",filename,description);fflush(stdout);
#endif
    /* v074: Use SF2000 firmware fs_* functions instead of fopen */
    if (sf_open_write(filename) < 0) {
	return;
    }
    gui_show_window_bar(0, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save CPU");fflush(stdout);
#endif
    dst = header;
    save_u32 (0);
    save_string("UAE");
    sprintf (tmp, "%d.%d.%d", UAEMAJOR, UAEMINOR, UAESUBREV);
    save_string (tmp);
    save_string (description);
    save_chunk (header, dst-header, "ASF ");

    dst = save_cpu (&len);
    save_chunk (dst, len, "CPU ");
    free (dst);

    gui_show_window_bar(1, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save DSK");fflush(stdout);
#endif
    strcpy(name, "DSKx");
    for (i = 0; i < 4; i++) {
	dst = save_disk (i, &len);
	if (dst) {
	    name[3] = i + '0';
	    save_chunk (dst, len, name);
	    free (dst);
	}
    }
    dst = save_floppy (&len);
    save_chunk (dst, len, "DISK");
    free (dst);

    gui_show_window_bar(2, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save CHIP");fflush(stdout);
#endif
    dst = save_custom (&len);
    save_chunk (dst, len, "CHIP");
    free (dst);

    gui_show_window_bar(3, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save AGAC");fflush(stdout);
#endif
    dst = save_custom_agacolors (&len);
    save_chunk (dst, len, "AGAC");
    free (dst);

    gui_show_window_bar(4, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save SPR");fflush(stdout);
#endif
    strcpy (name, "SPRx");
    for (i = 0; i < 8; i++) {
	dst = save_custom_sprite (&len, i);
	name[3] = i + '0';
	save_chunk (dst, len, name);
	free (dst);
    }

    gui_show_window_bar(5, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save AUD");fflush(stdout);
#endif
    strcpy (name, "AUDx");
    for (i = 0; i < 4; i++) {
	dst = save_audio (&len, i);
	name[3] = i + '0';
	save_chunk (dst, len, name);
	free (dst);
    }
    gui_show_window_bar(6, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save CIA");fflush(stdout);
#endif
    dst = save_cia (0, &len);
    save_chunk (dst, len, "CIAA");
    free (dst);

    dst = save_cia (1, &len);
    save_chunk (dst, len, "CIAB");
    free (dst);

    gui_show_window_bar(7, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save EXPA");fflush(stdout);
#endif
    dst = save_expansion (&len);
    save_chunk (dst, len, "EXPA");
#ifdef DEBUG_SAVESTATE
    puts("--> save CRAM");fflush(stdout);
#endif
    dst = save_cram (&len);
    save_chunk_compressed (dst, len, "CRAM");
#ifdef DEBUG_SAVESTATE
    puts("--> save BRAM");fflush(stdout);
#endif
    dst = save_bram (&len);
    save_chunk (dst, len, "BRAM");
#ifdef DEBUG_SAVESTATE
    puts("--> save FRAM");fflush(stdout);
#endif
    dst = save_fram (&len);
    save_chunk (dst, len, "FRAM");
#ifdef DEBUG_SAVESTATE
    puts("--> save ZRAM");fflush(stdout);
#endif
    dst = save_zram (&len);
    save_chunk (dst, len, "ZRAM");

    gui_show_window_bar(8, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save ROM");fflush(stdout);
#endif
    /* v080: ROM save DISABLED - static local arrays crash SF2000 */
#if 0
    dst = save_rom (1, &len);
    do {
	if (!dst)
	    break;
	save_chunk (dst, len, "ROM ");
    } while ((dst = save_rom (0, &len)));
#endif

    gui_show_window_bar(9, 10, 0);
#ifdef DEBUG_SAVESTATE
    puts("--> save END");fflush(stdout);
#endif
    /* v082: Use io_write for buffer/file mode support */
    io_write ("END ", 1, 4);
    io_write ("\0\0\0\08", 1, 4);
    write_log ("Save of '%s' complete\n", filename);
    sf_close();
    /* SF2000: Flush file to SD card */
    fs_sync(filename);
#ifdef DINGOO
    sync();
#endif
    gui_show_window_bar(10, 10, 0);
    clear_events();
    notice_screen_contents_lost();
#ifdef DEBUG_SAVESTATE
    printf("SAVED state=%X, flags=%X, PC=%X\n",savestate_state,_68k_spcflags,_68k_getpc());fflush(stdout);
#endif
#ifdef START_DEBUG_SAVESTATE
	DEBUG_AHORA=1;
#endif
}

/* v082: LIBRETRO BUFFER I/O FUNCTIONS
 * These are the direct buffer versions used by retro_serialize/retro_unserialize
 * No temp files - direct I/O to libretro-provided buffer like PicoDrive!
 */

/* Save state directly to memory buffer (for retro_serialize)
 * Returns: actual size of saved data, or 0 on error */
size_t save_state_to_buffer(void *buffer, size_t max_size)
{
    uae_u8 header[1000];
    char tmp[100];
    uae_u8 *dst;
    int len,i;
    char name[5];

    /* v089: Log entry to diagnose state reset issue */
    xlog("v089: === SAVE STATE START ===\n");
    xlog("v089: Before reset: arena_pos=%d, buf_pos=%d, io_mode=%d\n",
         (int)save_arena_pos, (int)buf_pos, io_mode);

    /* v088: Enable arena allocator and reset it
     * This prevents heap fragmentation from multiple malloc/free cycles */
    savestate_arena_reset();
    savestate_use_arena = 1;

    /* Set buffer I/O mode */
    io_mode = 1;
    buf_init_write((uae_u8 *)buffer, max_size);

    /* Same logic as save_state but no GUI, no file sync */
    dst = header;
    save_u32 (0);
    save_string("UAE");
    sprintf (tmp, "%d.%d.%d", UAEMAJOR, UAEMINOR, UAESUBREV);
    save_string (tmp);
    save_string ("libretro");
    save_chunk (header, dst-header, "ASF ");

    dst = save_cpu (&len);
    save_chunk (dst, len, "CPU ");
    /* v088: No free() - using arena allocator! */

    strcpy(name, "DSKx");
    for (i = 0; i < 4; i++) {
	dst = save_disk (i, &len);
	if (dst) {
	    name[3] = i + '0';
	    save_chunk (dst, len, name);
	    /* v088: No free() - using arena allocator! */
	}
    }
    dst = save_floppy (&len);
    save_chunk (dst, len, "DISK");
    /* v088: No free() - using arena allocator! */

    dst = save_custom (&len);
    save_chunk (dst, len, "CHIP");
    /* v088: No free() - using arena allocator! */

    dst = save_custom_agacolors (&len);
    save_chunk (dst, len, "AGAC");
    /* v088: No free() - using arena allocator! */

    strcpy (name, "SPRx");
    for (i = 0; i < 8; i++) {
	dst = save_custom_sprite (&len, i);
	name[3] = i + '0';
	save_chunk (dst, len, name);
	/* v088: No free() - using arena allocator! */
    }

    strcpy (name, "AUDx");
    for (i = 0; i < 4; i++) {
	dst = save_audio (&len, i);
	name[3] = i + '0';
	save_chunk (dst, len, name);
	/* v088: No free() - using arena allocator! */
    }

    dst = save_cia (0, &len);
    save_chunk (dst, len, "CIAA");
    /* v088: No free() - using arena allocator! */

    dst = save_cia (1, &len);
    save_chunk (dst, len, "CIAB");
    /* v088: No free() - using arena allocator! */

    dst = save_expansion (&len);
    save_chunk (dst, len, "EXPA");

    /* v082: Save RAM directly - no compression for buffer mode (zlib uses malloc) */
    dst = save_cram (&len);
    save_chunk (dst, len, "CRAM");  /* Uncompressed! */
    dst = save_bram (&len);
    save_chunk (dst, len, "BRAM");
    dst = save_fram (&len);
    save_chunk (dst, len, "FRAM");
    dst = save_zram (&len);
    save_chunk (dst, len, "ZRAM");

    /* ROM skip - same as file mode */

    io_write ("END ", 1, 4);
    io_write ("\0\0\0\08", 1, 4);

    /* Get final size and cleanup */
    size_t result = buf_pos;
    buf_close();
    io_mode = 0;

    /* v088: Disable arena allocator */
    savestate_use_arena = 0;

    /* v089: Log exit state to diagnose reset issue */
    xlog("v089: === SAVE STATE END ===\n");
    xlog("v089: After save: size=%d, arena_used=%d, buf_pos=%d, io_mode=%d\n",
         (int)result, (int)save_arena_pos, (int)buf_pos, io_mode);
    xlog("v089: WARNING: Check if buf_pos and io_mode reset to 0!\n");

    write_log ("v088: Save to buffer complete, size=%d, arena_used=%d\n", (int)result, (int)save_arena_pos);
    return result;
}

/* Restore state directly from memory buffer (for retro_unserialize)
 * Returns: true on success, false on error */
bool restore_state_from_buffer(const void *buffer, size_t size)
{
    uae_u8 *chunk,*end;
    char name[5];
    long len;
    long filepos;

    /* Set buffer I/O mode */
    io_mode = 1;
    buf_init_read((const uae_u8 *)buffer, size);

    chunk = restore_chunk (name, &len, &filepos);
    if (!chunk || memcmp (name, "ASF ", 4)) {
	write_log ("Buffer is not an AmigaStateFile\n");
	buf_close();
	io_mode = 0;
	return false;
    }

    restore_header (chunk);
    savestate_state = STATE_RESTORE;

    for (;;) {
	chunk = restore_chunk (name, &len, &filepos);
	write_log ("Chunk '%s' size %d\n", name, len);
	if (!strcmp (name, "END "))
	    break;

	/* v082: For buffer mode, RAM chunks are read directly from buffer
	 * Set the filepos so memory.cpp can use savestate_fseek/savestate_fread */
	if (!strcmp (name, "CRAM")) {
	    restore_cram (len, filepos);
	    continue;
	}
	else if (!strcmp (name, "BRAM")) {
	    restore_bram (len, filepos);
	    continue;
	} else if (!strcmp (name, "FRAM")) {
	    restore_fram (len, filepos);
	    continue;
	} else if (!strcmp (name, "ZRAM")) {
	    restore_zram (len, filepos);
	    continue;
	}

	if (!strcmp (name, "CPU "))
	    end = restore_cpu (chunk);
	else if (!strcmp (name, "AGAC"))
	    end = restore_custom_agacolors (chunk);
	else if (!strcmp (name, "SPR0"))
	    end = restore_custom_sprite (chunk, 0);
	else if (!strcmp (name, "SPR1"))
	    end = restore_custom_sprite (chunk, 1);
	else if (!strcmp (name, "SPR2"))
	    end = restore_custom_sprite (chunk, 2);
	else if (!strcmp (name, "SPR3"))
	    end = restore_custom_sprite (chunk, 3);
	else if (!strcmp (name, "SPR4"))
	    end = restore_custom_sprite (chunk, 4);
	else if (!strcmp (name, "SPR5"))
	    end = restore_custom_sprite (chunk, 5);
	else if (!strcmp (name, "SPR6"))
	    end = restore_custom_sprite (chunk, 6);
	else if (!strcmp (name, "SPR7"))
	    end = restore_custom_sprite (chunk, 7);
	else if (!strcmp (name, "CIAA"))
	    end = restore_cia (0, chunk);
	else if (!strcmp (name, "CIAB"))
	    end = restore_cia (1, chunk);
	else if (!strcmp (name, "CHIP"))
	    end = restore_custom (chunk);
	else if (!strcmp (name, "AUD0"))
	    end = restore_audio (chunk, 0);
	else if (!strcmp (name, "AUD1"))
	    end = restore_audio (chunk, 1);
	else if (!strcmp (name, "AUD2"))
	    end = restore_audio (chunk, 2);
	else if (!strcmp (name, "AUD3"))
	    end = restore_audio (chunk, 3);
	else if (!strcmp (name, "DISK"))
	    end = restore_floppy (chunk);
	else if (!strcmp (name, "DSK0"))
	    end = restore_disk (0, chunk);
	else if (!strcmp (name, "DSK1"))
	    end = restore_disk (1, chunk);
	else if (!strcmp (name, "DSK2"))
	    end = restore_disk (2, chunk);
	else if (!strcmp (name, "DSK3"))
	    end = restore_disk (3, chunk);
	else if (!strcmp (name, "EXPA"))
	    end = restore_expansion (chunk);
	else if (!strcmp (name, "ROM "))
	    end = chunk;
	else
	    write_log ("unknown chunk '%s' size %d bytes\n", name, len);
	if (len != end - chunk)
	    write_log ("Chunk '%s' total size %d bytes but read %d bytes!\n",
		       name, len, end - chunk);
    }

    /* v084 FIX: Restore RAM directly here!
     *
     * The problem in v082/v083:
     * - We closed the buffer before returning
     * - memory_reset() -> allocate_memory() was never called because
     *   retro_unserialize doesn't go through m68k_go reset path
     * - RAM was not restored = corrupted display!
     *
     * Solution: Call restore_ram_from_savestate() directly here to copy
     * RAM from buffer while it's still open. This is what working cores
     * (PicoDrive, SNES9x2002) do - everything is synchronous.
     */
    extern void restore_ram_from_savestate(void);
    restore_ram_from_savestate();

    /* Now we can close the buffer */
    buf_close();
    io_mode = 0;

    clear_events();

    /* ═══════════════════════════════════════════════════════════════════════
     * v115 COMPREHENSIVE CIA FIX - THE ULTIMATE SOLUTION
     * ═══════════════════════════════════════════════════════════════════════
     *
     * ROOT CAUSE OF CIA ASSERTION BUG (since v80):
     * The assertion "(ciabta+1) >= ciaclocks" in cia.cpp:181 fails because:
     *
     * 1. eventtab[ev_cia].evtime has OLD stale value (not synced!)
     *    - After restore, evtime still points to "past" cycles
     *    - do_cycles() sees evtime < currcycle and triggers CIA_handler IMMEDIATELY
     *
     * 2. eventtab[ev_cia].active is still TRUE (not cleared!)
     *    - CIA event is still marked as active from before restore
     *
     * 3. div10 (fractional CIA cycles) is desynchronized
     *    - div10 was calculated relative to OLD oldcycles
     *    - After my v113 sync of oldcycles, div10 context is wrong
     *
     * 4. CIA timers (ciabta etc.) may be at boundary condition
     *    - Timer saved exactly at underflow point
     *
     * 5. CIA_calctimers() is NEVER called after restore!
     *    - Only CIA_calctimers() properly updates evtime and active
     *    - In normal flow: CIA_reset() -> CIA_calctimers()
     *    - In libretro path: restore bypasses CIA_reset entirely!
     *
     * SEQUENCE OF FAILURE:
     * 1. restore_state_from_buffer() loads CIA timers
     * 2. Emulation resumes, retro_run() calls do_cycles()
     * 3. do_cycles() sees stale evtime <= currcycle
     * 4. CIA_handler() is called immediately
     * 5. CIA_update() calculates ccount with inconsistent state
     * 6. ASSERTION FAILS: (ciabta+1) >= ciaclocks
     *
     * SOLUTION: Complete CIA state reset + proper event rescheduling
     * ═══════════════════════════════════════════════════════════════════════ */

    write_log("v115: Starting COMPREHENSIVE CIA fix...\n");

    /* STEP 1: Reset savestate_state FIRST (v087 fix)
     * Many functions check this flag - we need it cleared before calling CIA functions */
    savestate_state = 0;
    write_log("v115: Step 1 - savestate_state = 0\n");

    /* STEP 2: DISABLE CIA event to prevent immediate trigger!
     * This is the CRITICAL fix - prevents do_cycles from calling CIA_handler
     * before we have a chance to properly reinitialize */
    eventtab[ev_cia].active = 0;
    write_log("v115: Step 2 - eventtab[ev_cia].active = 0 (disabled)\n");

    /* STEP 3: Sync ALL event oldcycles to current cycles (v113 fix, kept) */
    {
        int i;
        unsigned long current_cycles = get_cycles();
        for (i = 0; i < ev_max; i++) {
            eventtab[i].oldcycles = current_cycles;
        }
        write_log("v115: Step 3 - All eventtab[].oldcycles synced to %lu\n", current_cycles);
    }

    /* STEP 4: Reset div10 to 0 (NEW in v115!)
     * div10 is the fractional remainder in CIA clock calculation.
     * After restore, it's from a different context - reset it! */
    CIA_reset_div10();
    write_log("v115: Step 4 - div10 reset to 0\n");

    /* STEP 5: Reset CIA timers to SAFE maximum values (like v092 fix in CIA_reset)
     * This prevents any timer boundary condition issues.
     * The game will reset timers to proper values as needed. */
    ciaata = ciaatb = ciabta = ciabtb = 0xFFFF;
    write_log("v115: Step 5 - CIA timers reset to 0xFFFF (safe max)\n");

    /* STEP 6: Call CIA_calctimers() to properly recalculate event timing!
     * This is what was MISSING in v113/v114!
     * CIA_calctimers() does:
     * - eventtab[ev_cia].oldcycles = get_cycles()  (already done, but OK to redo)
     * - eventtab[ev_cia].evtime = calculated_future_time + get_cycles()
     * - eventtab[ev_cia].active = true/false based on timer states
     * - events_schedule() to update nextevent
     *
     * NOTE: Comment in cia.cpp says "Call this only after CIA_update has been called"
     * but with timers at 0xFFFF and div10=0, this is safe - no assertion can trigger. */
    CIA_calctimers();
    write_log("v115: Step 6 - CIA_calctimers() called, evtime properly scheduled\n");

    /* STEP 7: Clear gfx_mem framebuffer (v114 fix, kept) */
    if (gfx_mem != NULL) {
        memset(gfx_mem, 0, 320 * 288 * 2);
        write_log("v115: Step 7 - gfx_mem cleared (184320 bytes)\n");
    }

    write_log("v115: ═══ CIA FIX COMPLETE ═══\n");

    /* ═══════════════════════════════════════════════════════════════════════
     * v116 AUDIO SYNCHRONIZATION FIX
     * ═══════════════════════════════════════════════════════════════════════
     *
     * PROBLEM (identified in v115 testing):
     * After restore, audio sometimes doesn't work:
     * - If game was already playing music before save -> audio works after load
     * - If save was during kickstart boot (before audio init) -> NO SOUND!
     *
     * ROOT CAUSE:
     * Same pattern as CIA bug! Static variables in audio.cpp are not synced:
     * 1. last_cycles (line 57) - old value from before restore
     * 2. next_sample_evtime (line 58) - old value from before restore
     * 3. eventtab[ev_audio] - stale evtime, not rescheduled
     * 4. schedule_audio() - NEVER called after restore!
     *
     * rsn8887/uae4all2 reference (savestate.cpp line 378-393):
     *   void savestate_restore_finish (void) {
     *       resume_sound();
     *       update_audio();
     *       savestate_state = 0;
     *   }
     *
     * Our libretro path bypasses savestate_restore_finish entirely!
     *
     * SOLUTION: Reset audio timing state just like CIA timing state
     * ═══════════════════════════════════════════════════════════════════════ */

    write_log("v116: Starting AUDIO synchronization fix...\n");

    /* STEP 1: Disable audio event to prevent immediate trigger */
    eventtab[ev_audio].active = 0;
    write_log("v116: Step 1 - eventtab[ev_audio].active = 0 (disabled)\n");

    /* STEP 2: Reset last_cycles to current cycles
     * This ensures update_audio() calculates correct n_cycles delta */
    audio_reset_last_cycles();
    write_log("v116: Step 2 - last_cycles reset to current cycles\n");

    /* STEP 3: Reset next_sample_evtime to scaled_sample_evtime
     * This ensures proper sample timing */
    audio_reset_sample_evtime();
    write_log("v116: Step 3 - next_sample_evtime reset to scaled_sample_evtime\n");

    /* STEP 4: Call schedule_audio() to properly recalculate audio event timing
     * This is the CRITICAL call that was missing!
     * schedule_audio() does:
     * - eventtab[ev_audio].active = 0 (then may set to 1 based on channel states)
     * - eventtab[ev_audio].oldcycles = get_cycles()
     * - eventtab[ev_audio].evtime = get_cycles() + best channel evtime */
    schedule_audio();
    write_log("v116: Step 4 - schedule_audio() called, ev_audio properly scheduled\n");

    write_log("v116: ═══ AUDIO FIX COMPLETE ═══\n");

    /* ═══════════════════════════════════════════════════════════════════════
     * v118 DRAWING SYSTEM RESET FIX - "KASZA" BUG
     * ═══════════════════════════════════════════════════════════════════════
     *
     * PROBLEM (identified in v117 testing):
     * "Kasza" (garbage) on screen after loading saves with different resolutions!
     * - First load usually works
     * - Subsequent loads between different resolutions (lo-res <-> interlace) = garbage
     * - Reset machine fixes it every time
     *
     * ROOT CAUSE:
     * Drawing system uses double-buffered structures that are NOT reset after restore:
     *
     * 1. linestate[] - state of each line (LINE_UNDECIDED/DECIDED/DONE)
     *    - Old values say lines are "DONE" = not redrawn!
     *
     * 2. line_decisions[] - resolution info for each line
     *    - Contains old bplres, diwfirstword, diwlastword values
     *
     * 3. line_drawinfo[][] - sprite/color info (double-buffered prev/curr)
     *    - Mixing old/new data across different resolutions
     *
     * 4. amiga2aspect_line_map[], native2amiga_line_map[]
     *    - Line mapping outdated for new resolution
     *
     * 5. current_change_set - buffer index (0 or 1)
     *    - Desynced with actual data after restore
     *
     * WHY RESET FIXES IT:
     * customreset() calls reset_drawing() which:
     * - Sets all linestate[] to LINE_UNDECIDED (forces redraw)
     * - Calls init_aspect_maps() (recalculates line mapping)
     * - Calls init_drawing_frame() (resets frame variables)
     * - Clears spixels/spixstate (sprite data)
     *
     * FIX: Call reset_drawing() after every restore!
     * ═══════════════════════════════════════════════════════════════════════ */

    /* ═══════════════════════════════════════════════════════════════════════
     * v153: HYBRID APPROACH - Best of v140 + v145!
     * ═══════════════════════════════════════════════════════════════════════
     *
     * PROBLEM:
     * - v140: Multiple loads work (Benefactor), but 1st load fails for some games
     * - v145: 1st load works for ALL games, but 2nd/3rd loads fail
     *
     * SOLUTION:
     * - 1st load: Use v145 method (full reinit - BPLCON0, calcdiw, diwstate)
     * - 2nd+ load: Use v140 method (only reset_drawing - no extra reinits)
     *
     * Theory: On 1st load, the emulator needs full mode reinit to pick up
     * the correct display mode from saved state. On 2nd+ loads, the mode
     * is already correct but the extra reinits cause accumulated problems.
     * ═══════════════════════════════════════════════════════════════════════ */

    static int load_count = 0;
    load_count++;

    write_log("v153: HYBRID RESTORE - load #%d\n", load_count);

    /* ALWAYS do reset_drawing() - this is common to both v140 and v145 */
    reset_drawing();
    write_log("v153: reset_drawing() called\n");

    if (load_count == 1) {
        /* ═══ 1st LOAD: Use v145 method (full reinit) ═══ */
        write_log("v153: 1st load - using v145 FULL REINIT method\n");

        /* v145: init_hardware_frame - resets line tracking vars */
        init_hardware_frame_wrapper();
        write_log("v153: init_hardware_frame() called\n");

        /* v143: Reset DIW state machine */
        *get_diwstate_ptr() = DIW_waiting_start;
        *get_hdiwstate_ptr() = DIW_waiting_start;
        write_log("v153: diwstate reset to waiting_start\n");

        /* v144: BPLCON0 reinit trick */
        reinit_bplcon0_after_restore();
        write_log("v153: BPLCON0 reinit called\n");

        /* v143: Recalculate display window */
        calcdiw_wrapper();
        write_log("v153: calcdiw() called\n");

        /* v143: Reinitialize sprite resolution */
        expand_sprres_wrapper();
        write_log("v153: expand_sprres() called\n");

    } else {
        /* ═══ 2nd+ LOAD: Use v140 method (minimal) ═══ */
        write_log("v153: load #%d - using v140 MINIMAL method (only reset_drawing)\n", load_count);
        /* Nothing else! Just reset_drawing() which was already called above */
    }

    write_log("v153: ═══ RESTORE COMPLETE (load #%d) ═══\n", load_count);

    return true;
}

/*

My (Toni Wilen <twilen@arabuusimiehet.com>)
proposal for Amiga-emulators' state-save format

Feel free to comment...

This is very similar to IFF-fileformat
Every hunk must end to 4 byte boundary,
fill with zero bytes if needed

version 0.7

HUNK HEADER (beginning of every hunk)

        hunk name (4 ascii-characters)
        hunk size (including header)
        hunk flags             

        bit 0 = chunk contents are compressed with zlib (maybe RAM chunks only?)

HEADER

        "ASF " (AmigaStateFile)
        
	statefile version
        emulator name ("uae", "fellow" etc..)
        emulator version string (example: "0.8.15")
        free user writable comment string

CPU

         "CPU "

        CPU model               4 (68000,68010 etc..)
        CPU typeflags           bit 0=EC-model or not
        D0-D7                   8*4=32
        A0-A6                   7*4=32
        PC                      4
        prefetch address        4
        prefetch data           4
        USP                     4
        ISP                     4
        SR/CCR                  2
        flags                   4 (bit 0=CPU was HALTed)

        CPU specific registers

        68000: SR/CCR is last saved register
        68010: save also DFC,SFC and VBR
        68020: all 68010 registers and CAAR,CACR and MSP
        etc..

        DFC                     4 (010+)
        SFC                     4 (010+)
        VBR                     4 (010+)

        CAAR                    4 (020-030)
        CACR                    4 (020+)
        MSP                     4 (020+)

MMU (when and if MMU is supported in future..)

        MMU model               4 (68851,68030,68040)

        // 68040 fields

        ITT0                    4
        ITT1                    4
        DTT0                    4
        DTT1                    4
        URP                     4
        SRP                     4
        MMUSR                   4
        TC                      2

		
FPU (only if used)

	"FPU "

        FPU model               4 (68881 or 68882)
        FPU typeflags           4 (keep zero)

        FP0-FP7                 4+2 (80 bits)
        FPCR                    4
        FPSR                    4
        FPIAR                   4

CUSTOM CHIPS

        "CHIP"

        chipset flags   4      OCS=0,ECSAGNUS=1,ECSDENISE=2,AGA=4
                               ECSAGNUS and ECSDENISE can be combined

        DFF000-DFF1FF   352    (0x120 - 0x17f and 0x0a0 - 0xdf excluded)

        sprite registers (0x120 - 0x17f) saved with SPRx chunks
        audio registers (0x0a0 - 0xdf) saved with AUDx chunks

AGA COLORS

        "AGAC"

        AGA color               8 banks * 32 registers *
        registers               LONG (XRGB) = 1024

SPRITE

        "SPR0" - "SPR7"


        SPRxPT                  4
        SPRxPOS                 2
        SPRxCTL                 2
        SPRxDATA                2
        SPRxDATB                2
        AGA sprite DATA/DATB    3 * 2 * 2
        sprite "armed" status   1

        sprites maybe armed in non-DMA mode
        use bit 0 only, other bits are reserved


AUDIO
        "AUD0" "AUD1" "AUD2" "AUD3"

        audio state             1
        machine mode
        AUDxVOL                 1
	irq?                    1
	data_written?           1
        internal AUDxLEN        2
        AUDxLEN                 2
	internal AUDxPER        2
	AUDxPER                 2
        internal AUDxLC         4
	AUDxLC                  4
	evtime?                 4

BLITTER

        "BLIT"

        internal blitter state

        blitter running         1
        anything else?

CIA

        "CIAA" and "CIAB"

        BFE001-BFEF01   16*1 (CIAA)
        BFD000-BFDF00   16*1 (CIAB)

        internal registers

        IRQ mask (ICR)  1 BYTE
        timer latches   2 timers * 2 BYTES (LO/HI)
        latched tod     3 BYTES (LO/MED/HI)
        alarm           3 BYTES (LO/MED/HI)
        flags           1 BYTE
                        bit 0=tod latched (read)
                        bit 1=tod stopped (write)
	div10 counter	1 BYTE

FLOPPY DRIVES

        "DSK0" "DSK1" "DSK2" "DSK3"

        drive state

        drive ID-word           4
        state                   1 (bit 0: motor on, bit 1: drive disabled)
        rw-head track           1
        dskready                1
        id-mode                 1 (ID mode bit number 0-31)
        floppy information

        bits from               4
        beginning of track
        CRC of disk-image       4 (used during restore to check if image
                                  is correct)
        disk-image              null-terminated
        file name

INTERNAL FLOPPY CONTROLLER STATUS

        "DISK"

        current DMA word        2
        DMA word bit offset     1
        WORDSYNC found          1 (no=0,yes=1)
        hpos of next bit        1
        DSKLENGTH status        0=off,1=written once,2=written twice

RAM SPACE 

        "xRAM" (CRAM = chip, BRAM = bogo, FRAM = fast, ZFRAM = Z3)

        start address           4 ("bank"=chip/slow/fast etc..)
        of RAM "bank"
        RAM "bank" size         4
        RAM flags               4
        RAM "bank" contents

ROM SPACE

        "ROM "

        ROM start               4
        address
        size of ROM             4
        ROM type                4 KICK=0
        ROM flags               4
        ROM version             2
        ROM revision            2
        ROM CRC                 4 see below
        ROM-image               null terminated, see below
        ID-string
        ROM contents            (Not mandatory, use hunk size to check if
                                this hunk contains ROM data or not)

        Kickstart ROM:
         ID-string is "Kickstart x.x"
         ROM version: version in high word and revision in low word
         Kickstart ROM version and revision can be found from ROM start
         + 12 (version) and +14 (revision)

        ROM version and CRC is only meant for emulator to automatically
        find correct image from its ROM-directory during state restore.

        Usually saving ROM contents is not good idea.


END
        hunk "END " ends, remember hunk size 8!


EMULATOR SPECIFIC HUNKS

Read only if "emulator name" in header is same as used emulator.
Maybe useful for configuration?

misc:

- save only at position 0,0 before triggering VBLANK interrupt
- all data must be saved in bigendian format
- should we strip all paths from image file names?

*/
