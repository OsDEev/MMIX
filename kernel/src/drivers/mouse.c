#include <io.h>
#include <libk.h>
#include <mouse.h>
#include <tty.h>

/*
 * PS/2 mouse driver.
 *
 * The PS/2 auxiliary device (mouse) shares the controller at ports
 * 0x60/0x64. We enable it during init and then read 3-byte packets
 * from IRQ12. The packet format is:
 *   byte 0: [Yov][Xov][Ys][Xs][1][MB][RB][LB]
 *   byte 1: X movement (signed, 8-bit)
 *   byte 2: Y movement (signed, 8-bit, positive = up in PS/2 = down on screen)
 */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

/* PS/2 controller commands */
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_AUX   0xA7
#define PS2_CMD_ENABLE_AUX    0xA8
#define PS2_CMD_TEST_AUX      0xA9
#define PS2_CMD_SELF_TEST     0xAA

/* Mouse commands */
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE_DATA  0xF4
#define MOUSE_CMD_GET_ID       0xF2
#define MOUSE_CMD_SET_SAMPLE   0xE6

#define MOUSE_PACKET_SIZE 3

static volatile uint8_t packet_buf[MOUSE_PACKET_SIZE];
static volatile int packet_idx = 0;

static struct mouse_state mouse;
static int32_t mouse_min_x = 0, mouse_min_y = 0;
static int32_t mouse_max_x = 799, mouse_max_y = 599;

/* Wait for controller input buffer to be empty before writing. */
static void ps2_wait_input(void) {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return;
    }
}

/* Wait for controller output buffer to have data. */
static void ps2_wait_output(void) {
    for (int i = 0; i < 10000; i++) {
        if (inb(PS2_STATUS) & 0x01) return;
    }
}

/* Write to the PS/2 controller, reading back ACKs from the mouse. */
static void ps2_write_aux(uint8_t data) {
    ps2_wait_input();
    outb(PS2_CMD, 0xD4);        /* tell controller: next byte goes to aux */
    ps2_wait_input();
    outb(PS2_DATA, data);
    ps2_wait_output();
    (void)inb(PS2_DATA);         /* consume ACK or garbage */
}

/* Read the current PS/2 controller config byte. */
static uint8_t ps2_read_config(void) {
    ps2_wait_input();
    outb(PS2_CMD, PS2_CMD_READ_CONFIG);
    ps2_wait_output();
    return inb(PS2_DATA);
}

/* Write the PS/2 controller config byte. */
static void ps2_write_config(uint8_t cfg) {
    ps2_wait_input();
    outb(PS2_CMD, PS2_CMD_WRITE_CONFIG);
    ps2_wait_input();
    outb(PS2_DATA, cfg);
}

void mouse_init(void) {
    /* Set initial cursor position to screen center. */
    mouse.x = (int32_t)tty_width() / 2;
    mouse.y = (int32_t)tty_height() / 2;
    mouse.dx = mouse.dy = 0;
    mouse.buttons = 0;
    mouse.valid = false;
    mouse_max_x = (int32_t)tty_width() - 1;
    mouse_max_y = (int32_t)tty_height() - 1;

    /* Enable the auxiliary (mouse) device on the PS/2 controller. */
    uint8_t cfg = ps2_read_config();
    cfg |= 0x02;   /* enable IRQ12 (auxiliary) */
    cfg &= ~0x20;  /* disable mouse clock scalling (for data reporting) */
    ps2_write_config(cfg);

    /* Enable the auxiliary device. */
    ps2_wait_input();
    outb(PS2_CMD, PS2_CMD_ENABLE_AUX);
    ps2_wait_output();
    (void)inb(PS2_DATA);

    /* Reset the mouse. */
    ps2_write_aux(MOUSE_CMD_SET_DEFAULTS);
    ps2_write_aux(MOUSE_CMD_ENABLE_DATA);

    kprintf("[MOUSE] Initialized (cursor at %d,%d)\n", mouse.x, mouse.y);
}

void mouse_interrupt(void) {
    uint8_t status;
    __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)PS2_STATUS));
    if (!(status & 0x01)) return;

    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"((uint16_t)PS2_DATA));

    packet_buf[packet_idx++] = data;

    if (packet_idx >= MOUSE_PACKET_SIZE) {
        packet_idx = 0;

        uint8_t b0 = packet_buf[0];
        int8_t  dx = (int8_t)packet_buf[1];
        int8_t  dy = (int8_t)packet_buf[2];

        /* Check for overflow bits -- ignore the packet if set. */
        if (!(b0 & 0x40) && !(b0 & 0x80)) {
            mouse.buttons = b0 & 0x07;
            mouse.dx = dx;
            mouse.dy = -dy; /* PS/2 Y is inverted relative to screen */
            mouse.x += dx;
            mouse.y += mouse.dy;

            /* Clamp to screen bounds. */
            if (mouse.x < mouse_min_x) mouse.x = mouse_min_x;
            if (mouse.x > mouse_max_x) mouse.x = mouse_max_x;
            if (mouse.y < mouse_min_y) mouse.y = mouse_min_y;
            if (mouse.y > mouse_max_y) mouse.y = mouse_max_y;

            mouse.valid = true;
        }
    }
}

void mouse_get_state(struct mouse_state *out) {
    out->x = mouse.x;
    out->y = mouse.y;
    out->dx = mouse.dx;
    out->dy = mouse.dy;
    out->buttons = mouse.buttons;
    out->valid = mouse.valid;
}
