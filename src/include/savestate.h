 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Save/restore emulator state
  *
  * (c) 1999-2001 Toni Wilen
  */


/* functions to save byte,word or long word
 * independent of CPU's endianess */

extern void save_u16_func (uae_u8 **, uae_u16);
extern void save_u32_func (uae_u8 **, uae_u32);
extern void save_u8_func (uae_u8 **, uae_u8);

extern uae_u16 restore_u16_func (uae_u8 **);
extern uae_u32 restore_u32_func (uae_u8 **);
extern uae_u8 restore_u8_func (uae_u8 **);

extern void save_string_func (uae_u8 **, char*);
extern char *restore_string_func (uae_u8 **);

#define save_u16(x) save_u16_func (&dst, (x))
#define save_u32(x) save_u32_func (&dst, (x))
#define save_u8(x) save_u8_func (&dst, (x))

#define restore_u16() restore_u16_func (&src)
#define restore_u32() restore_u32_func (&src)
#define restore_u8() restore_u8_func (&src)

#define save_string(x) save_string_func (&dst, (x))
#define restore_string() restore_string_func (&src)

void savestate_restore_finish (void);

/* v088: Arena allocator for save functions - avoids malloc/free fragmentation */
extern int savestate_use_arena;
extern void *savestate_arena_alloc(size_t size);
extern void savestate_arena_reset(void);

/* save, restore and initialize routines for Amiga's subsystems */

extern uae_u8 *restore_cpu (uae_u8 *);
extern uae_u8 *save_cpu (int *);

extern uae_u8 *restore_fpu (uae_u8 *);
extern uae_u8 *save_fpu (int *);

extern uae_u8 *restore_disk (int, uae_u8 *);
extern uae_u8 *save_disk (int, int *);
extern uae_u8 *restore_floppy (uae_u8 *src);
extern uae_u8 *save_floppy (int *len);
extern void DISK_save_custom  (uae_u32 *pdskpt, uae_u16 *pdsklen, uae_u16 *pdsksync, uae_u16 *pdskdatr, uae_u16 *pdskbytr);
extern void DISK_restore_custom  (uae_u32 pdskpt, uae_u16 pdsklength, uae_u16 pdskdatr, uae_u16 pdskbytr);

extern uae_u8 *restore_custom (uae_u8 *);
extern uae_u8 *save_custom (int *);

extern uae_u8 *restore_custom_sprite (uae_u8 *src, int num);
extern uae_u8 *save_custom_sprite (int *len, int num);

extern uae_u8 *restore_custom_agacolors (uae_u8 *src);
extern uae_u8 *save_custom_agacolors (int *len);

extern uae_u8 *restore_custom_blitter (uae_u8 *src);
extern uae_u8 *save_custom_blitter (int *len);

extern uae_u8 *restore_audio (uae_u8 *, int);
extern uae_u8 *save_audio (int *, int);

extern uae_u8 *restore_cia (int, uae_u8 *);
extern uae_u8 *save_cia (int, int *);

extern uae_u8 *restore_expansion (uae_u8 *);
extern uae_u8 *save_expansion (int *);

extern void restore_cram (int, long);
extern void restore_bram (int, long);
extern void restore_fram (int, long);
extern void restore_zram (int, long);
extern uae_u8 *save_cram (int *);
extern uae_u8 *save_bram (int *);
extern uae_u8 *save_fram (int *);
extern uae_u8 *save_zram (int *);

extern uae_u8 *restore_rom (uae_u8 *);
extern uae_u8 *save_rom (int, int *);

extern void save_state (char *filename, char *description);
extern void restore_state (char *filename);

/* v082: Libretro direct buffer I/O functions (no temp files!) */
extern size_t save_state_to_buffer(void *buffer, size_t max_size);
extern bool restore_state_from_buffer(const void *buffer, size_t size);

extern void custom_save_state (void);

#define STATE_SAVE 1
#define STATE_RESTORE 2
#define STATE_DOSAVE 4
#define STATE_DORESTORE 8

extern int savestate_state;
extern char *savestate_filename;
/* v074: savestate_file no longer used - using firmware fs_* functions */
/* extern FILE *savestate_file; */

/* v074: Global wrappers for memory.cpp to use during restore */
extern int savestate_fseek(long offset, int whence);
extern size_t savestate_fread(void *buf, size_t size, size_t count);
