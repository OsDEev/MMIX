#include <kbd.h>
#include <libk.h>
#include <pipe.h>
#include <sched.h>

/*
 * PS/2 keyboard, scancode set 1, translated through a simple US layout.
 * Printable ASCII, Backspace and Enter are delivered to userspace.
 * Ctrl+C raises SIGINT in every user process instead.
 */

#define KBD_DATA 0x60
#define KBD_STATUS 0x64

#define KB_BUFFER_SIZE 128

static char kb_buf[KB_BUFFER_SIZE];
static volatile size_t kb_head = 0, kb_tail = 0;
static bool shift = false;
static bool ctrl = false;

static const char scancode_map[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ', 0
};

static char translate(uint8_t sc) {
    char c = scancode_map[sc];
    if (c >= 'a' && c <= 'z' && !shift) return c;
    if (c >= 'a' && c <= 'z' && shift) return (char)(c - 'a' + 'A');
    if (shift) {
        switch (c) {
            case '1': return '!'; case '2': return '@'; case '3': return '#';
            case '4': return '$'; case '5': return '%'; case '6': return '^';
            case '7': return '&'; case '8': return '*'; case '9': return '(';
            case '0': return ')'; case '-': return '_'; case '=': return '+';
            case '[': return '{'; case ']': return '}'; case '\\': return '|';
            case ';': return ':'; case '\'': return '"'; case ',': return '<';
            case '.': return '>'; case '/': return '?'; case '`': return '~';
        }
    }
    return c;
}

static inline void buf_push(char c) {
    size_t next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next == kb_tail) return; /* full: drop */
    kb_buf[kb_head] = c;
    kb_head = next;
}

extern long console_fg_pgid;

/* Ctrl+C: SIGINT to the console foreground process group. */
static void raise_sigint(void) {
    kb_head = kb_tail; /* flush pending input */

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = sched_task_at(i);
        if (t == NULL || t->state == TASK_FREE || t->pid < 1) continue;
        if (t->pgid != console_fg_pgid) continue;
        t->sig_pending |= (1u << SIGINT);
        if (t->state == TASK_BLOCKED && t->wait_pipe != NULL) {
            t->wait_pipe = NULL;
            sched_wake(t);
        }
    }
}

void kbd_interrupt(void) {
    uint8_t status;
    __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)KBD_STATUS));
    if (!(status & 1)) return;

    uint8_t sc;
    __asm__ volatile("inb %1, %0" : "=a"(sc) : "Nd"((uint16_t)KBD_DATA));

    if (sc == 0x2A || sc == 0x36) { shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return; }
    if (sc == 0x1D) { ctrl = true; return; }
    if (sc == 0x9D) { ctrl = false; return; }
    if (sc & 0x80) return;        /* key release */
    if (sc == 0xE0) return;       /* extended prefix: ignore for now */

    char c = translate(sc & 0x7F);
    if (c == 0) return;

    if (ctrl && (c == 'c' || c == 'C')) {
        raise_sigint();
        return;
    }

    buf_push(c);
}

bool kbd_poll(char *out) {
    if (kb_head == kb_tail) return false;
    *out = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return true;
}

int kbd_getchar(void) {
    char c;
    for (;;) {
        task_t *cur = sched_get_current();
        if (cur != NULL && cur->sig_pending != 0) {
            return -1; /* interrupted */
        }
        if (g_resched_requested) {
            g_resched_requested = false;
            sched_yield();
        }
        if (kbd_poll(&c)) return (unsigned char)c;
        __asm__ volatile("sti");
        __asm__ volatile("hlt");
    }
}
