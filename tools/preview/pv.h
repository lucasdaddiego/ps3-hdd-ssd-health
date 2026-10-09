/* The preview's own hooks between stubs.c (the world: script, time,
 * snapshots), gfx_soft.c (the back end of the app's renderer) and
 * gfx_trace.c (PV_GFXTRACE). */
#ifndef PV_H
#define PV_H

void pv_start(void);                         /* PV_RES and PV_SCRIPT, before the video calls */
int pv_drawing(void);                        /* 1 in a frame that gets a snapshot: only these are rasterized */
void pv_flip(void);                          /* the end of a frame: snapshots, time, the script's EXIT */
void pv_trace_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* one PV_GFXTRACE line */

#endif
