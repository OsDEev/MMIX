#include <kheap.h>
#include <pipe.h>
#include <sched.h>
#include <string.h>

struct pipe *pipe_create(void) {
    struct pipe *p = kmalloc(sizeof(struct pipe));
    if (p == NULL) return NULL;
    memset(p, 0, sizeof(*p));
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_ref(struct pipe *p, int rd, int wr) {
    if (p == NULL) return;
    p->readers += rd;
    p->writers += wr;
}

/* Wake every task parked on this pipe. */
static void pipe_wake(struct pipe *p) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = sched_task_at(i);
        if (t != NULL && t->state == TASK_BLOCKED && t->wait_pipe == p) {
            t->wait_pipe = NULL;
            sched_wake(t);
        }
    }
}

void pipe_unref(struct pipe *p, int rd, int wr) {
    if (p == NULL) return;
    p->readers -= rd;
    p->writers -= wr;

    /* EOF / EPIPE transitions must wake parked tasks. */
    pipe_wake(p);

    if (p->readers <= 0 && p->writers <= 0) {
        kfree(p);
    }
}

/* Returns true when a pending fatal signal should abort the caller. */
static bool sig_abort(task_t *t) {
    return t->sig_pending != 0;
}

int pipe_read(struct pipe *p, void *buf, size_t n) {
    task_t *cur = sched_get_current();

    for (;;) {
        if (p->len > 0) {
            size_t got = (n < p->len) ? n : p->len;
            for (size_t i = 0; i < got; i++) {
                ((uint8_t *)buf)[i] = p->buf[p->tail];
                p->tail = (p->tail + 1) % sizeof(p->buf);
            }
            p->len -= got;
            pipe_wake(p); /* writers may have room now */
            return (int)got;
        }

        if (p->writers == 0) return 0; /* EOF */
        if (sig_abort(cur)) return -1;

        cur->wait_pipe = p;
        cur->state = TASK_BLOCKED;
        sched_yield();
        cur->wait_pipe = NULL;
    }
}

int pipe_write(struct pipe *p, const void *buf, size_t n) {
    task_t *cur = sched_get_current();

    size_t done = 0;
    while (done < n) {
        if (p->readers == 0) return -1; /* EPIPE */
        if (sig_abort(cur)) return -1;

        if (p->len < sizeof(p->buf)) {
            while (done < n && p->len < sizeof(p->buf)) {
                p->buf[p->head] = ((const uint8_t *)buf)[done++];
                p->head = (p->head + 1) % sizeof(p->buf);
                p->len++;
            }
            pipe_wake(p); /* readers may have data now */
            continue;
        }

        cur->wait_pipe = p;
        cur->state = TASK_BLOCKED;
        sched_yield();
        cur->wait_pipe = NULL;
    }
    return (int)done;
}
