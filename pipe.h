/* pipe.h — ring-buffer pipes with blocking R/W */
#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"

#define MAX_PIPES   8
#define PIPE_BUFSZ  8192

void pipe_init(void);

/* Allocate a pipe; returns pipe_id >= 0, or -1. Sets readers=writers=1. */
int  pipe_alloc(void);

void pipe_incref(int pipe_id, int is_write);
void pipe_decref(int pipe_id, int is_write);

/* Blocking read/write. Returns bytes, 0 on EOF (read), -1 on error. */
long pipe_read(int pipe_id, char *dst, size_t len);
long pipe_write(int pipe_id, const char *src, size_t len);

#endif
