/*
 * SF2000 Diagnostic Display
 * Shows initialization progress on screen - cumulative log
 */

#ifndef SF2000_DIAG_H
#define SF2000_DIAG_H

#ifdef SF2000

#ifdef __cplusplus
extern "C" {
#endif

/* External functions from sf2000_multicore debug.c */
extern void dbg_show_noblock(unsigned short background_color, const char *fmt, ...);
extern void xlog(const char *fmt, ...);
extern void lcd_bsod(const char *fmt, ...);
extern void dly_tsk(int ms);

/* Cumulative diagnostic buffer - shows last N steps */
#define DIAG_MAX_LINES 12
#define DIAG_LINE_LEN 32

extern char diag_lines[DIAG_MAX_LINES][DIAG_LINE_LEN];
extern int diag_line_idx;

/* Add line to diagnostic - RE-ENABLED in v089 for save state debugging */
static inline void diag_add(const char *msg) {
    /* v089: Re-enabled xlog for save state diagnostics */
    xlog("UAE4ALL: %s\n", msg);
    (void)msg; // Suppress unused warning
}

#define DIAG(msg) diag_add(msg)

/* Fatal error - blue screen of death */
#define DIAG_FATAL(msg) lcd_bsod("UAE4ALL FATAL:\n%s", msg)

#ifdef __cplusplus
}
#endif

#else /* not SF2000 */

#define DIAG(msg)
#define DIAG_FATAL(msg)

#endif /* SF2000 */

#endif /* SF2000_DIAG_H */
