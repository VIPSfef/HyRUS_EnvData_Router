#ifndef MONITOR_FRAME_H
#define MONITOR_FRAME_H

#include <stddef.h>
#include <stdio.h>

/* Bounded monitor-owned storage, independent of terminal dimensions. The
 * dashboard and settings have fewer rows than this even with every device. */
enum { MF_FRAME_BYTES=65536, MF_MAX_ROWS=128 };
typedef struct {
    char text[MF_FRAME_BYTES];
    size_t used;
    int overflow;
} mf_frame;

void mf_reset(mf_frame *frame);
size_t mf_storage_bytes(void);
/* Append atomically; overflow invalidates the frame, never a partial escape. */
int mf_append(mf_frame *frame,const char *text,size_t length);
/* Single monitor thread. Compare complete rows including their colour, build
 * a bounded update, then write once. No full-screen erase or scrolling LF.
 * previous=NULL or repaint repaints the viewport, including stale lower rows.
 * Callers retain previous only after success; redirected logs bypass this. */
int mf_present(FILE *out,const mf_frame *current,const mf_frame *previous,
               unsigned rows,unsigned columns,int repaint);

#endif
