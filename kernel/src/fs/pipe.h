#ifndef MYUNIX_PIPE_H
#define MYUNIX_PIPE_H

#include <stddef.h>
#include <stdint.h>

#include "../proc/sched.h"

/* Anonymous pipe: fixed-size kernel ring buffer with reader/writer counts. */
struct pipe {
    uint8_t buf[256];
    size_t head, tail, len;
    int readers, writers;
};

struct pipe *pipe_create(void);
void pipe_ref(struct pipe *p, int rd, int wr);
void pipe_unref(struct pipe *p, int rd, int wr);

/* Blocking byte-stream semantics; return -1 on EPIPE/abort. */
int pipe_read(struct pipe *p, void *buf, size_t n);
int pipe_write(struct pipe *p, const void *buf, size_t n);

#endif /* MYUNIX_PIPE_H */
