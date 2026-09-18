/*
 * UHCI host controller driver + USB HID boot keyboard.
 *
 * Tested against QEMU:  -device piix4-usb-uhci -device usb-kbd
 *
 * Model: a single reclaimable queue-head list with one TD armed at a
 * time.  Endpoint-0 control transfers run as sequential single-TD
 * phases (SETUP -> DATA -> STATUS); the interrupt-IN endpoint keeps a
 * permanently armed TD whose data toggle is maintained by the HC
 * across events (device NAKs do not complete the transfer).
 */
#include <io.h>
#include <idt.h>
#include <kbd.h>
#include <libk.h>
#include <pci.h>
#include <pmm.h>
#include <string.h>
#include <usb.h>

/* --- UHCI I/O register offsets (from the I/O base) --- */
#define REG_USBCMD    0x00
#define REG_USBSTS    0x02
#define REG_USBINTR   0x04
#define REG_FRNUM     0x06
#define REG_FRBASEADD 0x08
#define REG_SOFMOD    0x0A
#define REG_PORTSC1   0x0C
#define REG_PORTSC2   0x10

#define CMD_RUN     0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF      0x0040

#define STS_USBINT    0x0001
#define STS_USBERR    0x0002
#define STS_HCHALTED  0x0020

#define INTR_TI       0x0001   /* Timeout/CRC interrupt enable */
#define INTR_IOCE     (1u << 2) /* Interrupt-on-Complete enable (0x0004) */

#define PORT_CCS      0x0001
#define PORT_PR       0x0100
#define PORT_EN       0x0004

#define TD_TERMINATED 0x00000001u
#define TD_QH         0x00000002u
#define CTRL_ACTIVE   (1u << 23)   /* TD Active bit (control/status word) */

/* Final device address of the keyboard (assigned by SET_ADDRESS). */
#define KBD_ADDR 1
#define KBD_EP   1

/* TD control/status bits (UHCI spec / Linux uhci-hcd.h). */
#define CTRL_STALLED  (1u << 22)   /* device returned STALL */
#define CTRL_BABBLE   (1u << 20)   /* device babbled */
#define CTRL_NAK      (1u << 19)   /* NAK received (not an error) */
#define CTRL_CRCTIMEO (1u << 18)   /* CRC/timeout (QEMU sets it for NODEV) */

/* --- DMA descriptors (physical addresses, < 4 GiB) --- */

struct uhci_qh {
    uint32_t link;     /* horizontal: bit1 = QH, bit0 = terminate */
    uint32_t element;  /* current TD, or terminate */
    uint32_t _pad[6];
};

struct uhci_td {
    uint32_t link;     /* horizontal: link pointer (TD_REQ: bit0 terminate) */
    uint32_t control;  /* maxlen, IOC, error counter; bit23 = Active */
    uint32_t token;    /* pid, device addr, endpoint, data toggle */
    uint32_t buffer;   /* physical address of the data buffer */
};

#define MAXLEN(n)  ((uint32_t)((n) & 0x7FF))
#define IOC        (1u << 24)
#define ERRCNT3    (0b11u << 27)

/* UHCI TD token layout: PID (7:0), device addr (14:8), endpoint (18:15),
 * data toggle (23) and max length (31:21, value = len - 1). */
#define TOKEN(pid, addr, ep, toggle, maxlen) \
    ((uint32_t)(pid) | (((uint32_t)(addr) & 0x7F) << 8) | \
     (((uint32_t)(ep) & 0xF) << 15) | (((uint32_t)(toggle) & 1) << 23) | \
     (((uint32_t)((maxlen) - 1) & 0x7FF) << 21))

/* --- Driver state --- */

static uint16_t uhci_io = 0;
static volatile uint32_t *frame_list;

static struct uhci_qh *qh_root, *qh_ep0, *qh_int;
static struct uhci_td *td_ep0, *td_int;

static uint8_t *buf_setup;       /* 8-byte SETUP packet */
static uint8_t *buf_data;        /* EP0 data-stage buffer */
static uint8_t *buf_kbd;         /* keyboard interrupt-IN buffer */
static uint16_t kbd_mps = 8;
static bool kbd_ready;
static uint8_t dev_addr;         /* device's current USB address (0 until SET_ADDRESS) */

static volatile bool ep0_done;
static volatile bool ep0_busy;
static volatile bool kbd_busy;   /* interrupt-IN transfer currently queued */

static void uhci_arm(struct uhci_qh *qh, struct uhci_td *td);
static void kbd_report(struct hid_boot_kbd_report *r);
static void uhci_reenumerate(void);

/* --- Controller register access --- */

static inline uint16_t r16(uint16_t off) { return inw(uhci_io + off); }
static inline void w16(uint16_t off, uint16_t v) { outw(uhci_io + off, v); }
static inline void w32(uint16_t off, uint32_t v) { outl(uhci_io + off, v); }

static void uhci_run_wait(void) {
    for (unsigned i = 0; i < 100000; i++) {
        if (!(r16(REG_USBSTS) & STS_HCHALTED)) return;
    }
    kprintf("[UHCI] controller did not start (USBSTS=0x%x)\n",
            (unsigned)r16(REG_USBSTS));
}

/* --- Interrupt handling --- */

static void uhci_process_kbd(void) {
    uint32_t tc = td_int->control;
    if (tc & (CTRL_STALLED | CTRL_BABBLE | CTRL_CRCTIMEO)) {
        kprintf("[UHCI] int-IN error ctrl=0x%x\n", tc);
        uhci_reenumerate();
        return;
    }
    kbd_report((struct hid_boot_kbd_report *)buf_kbd);
    /* Re-arm the same TD: the HC kept its data toggle in sync. */
    uhci_arm(qh_int, td_int);
}

void uhci_irq_handler(void) {
    uint16_t sts = r16(REG_USBSTS);
    if (sts == 0) return;
    w16(REG_USBSTS, sts); /* write-1-to-clear */

    if (sts & (STS_USBINT | STS_USBERR)) {
        if (ep0_busy && (td_ep0->control & CTRL_ACTIVE) == 0)
            ep0_done = true;
        if (kbd_busy && (td_int->control & CTRL_ACTIVE) == 0)
            uhci_process_kbd();
    }
}

/* --- Queue/TD helpers --- */

static void uhci_arm(struct uhci_qh *qh, struct uhci_td *td) {
    td->control |= CTRL_ACTIVE;
    qh->element = (uint32_t)virt_to_phys(td);
}

static void uhci_disarm(struct uhci_qh *qh, struct uhci_td *td) {
    qh->element = TD_TERMINATED;
    td->control &= ~CTRL_ACTIVE;
}

/* Wait for a single phase TD to complete.  The HC clears the Active bit
 * in the TD control word, so that bit is the only authoritative completion
 * signal; the IOC IRQ is merely a wake-up hint, never a completion by
 * itself (a stale flag set by the *previous* phase's interrupt must not
 * trick the wait into returning before QEMU has executed the current TD). */
static int uhci_wait(struct uhci_td *td) {
    uint16_t f0 = r16(REG_FRNUM);
    for (unsigned f = 0; f < 20000000; f++) {
        uint32_t cs = *(volatile uint32_t *)&td->control;
        if ((cs & CTRL_ACTIVE) == 0) return 0;
        if (ep0_done) ep0_done = false;   /* consume hint, keep polling */
        __asm__ volatile("pause" ::: "memory");
    }
    kprintf("[UHCI] phase timeout: link=0x%x ctrl=0x%x tok=0x%x buf=0x%x "
            "USBSTS=0x%x FRNUM=%u->%u P2=0x%x\n",
            (unsigned)td->link, (unsigned)td->control, (unsigned)td->token,
            (unsigned)td->buffer, (unsigned)r16(REG_USBSTS),
            (unsigned)f0, (unsigned)r16(REG_FRNUM), (unsigned)r16(REG_PORTSC2));
    return -1;
}

/* --- Endpoint-0 control transfers --- */

static int uhci_ep0_phase(uint8_t pid, uint16_t maxlen, uint8_t toggle,
                          const void *data, bool write) {
    if (pid == USB_PID_SETUP) {
        memcpy(buf_setup, data, 8);
        td_ep0->buffer = (uint32_t)virt_to_phys(buf_setup);
    } else if (write && maxlen > 0 && data != NULL) {
        memcpy(buf_data, data, maxlen);
        td_ep0->buffer = (uint32_t)virt_to_phys(buf_data);
    } else {
        td_ep0->buffer = (uint32_t)virt_to_phys(buf_data);
    }
    td_ep0->token = TOKEN(pid, dev_addr, 0, toggle, maxlen);
    td_ep0->control = MAXLEN(maxlen) | IOC | ERRCNT3;
    ep0_done = false;   /* drop any stale hint before arming this phase */
    ep0_busy = true;
    uhci_arm(qh_ep0, td_ep0);
    int r = uhci_wait(td_ep0);
    uint32_t tc = td_ep0->control;
    /* A completed phase is only a success when the error bits stayed clear;
     * a STALL / timeout / babble clears Active too and must fail the whole
     * transfer (QEMU STALLs the status when its control state is out of
     * sync, and NODEV shows up as CTRL_CRCTIMEO). */
    if (r == 0 && (tc & (CTRL_STALLED | CTRL_BABBLE | CTRL_CRCTIMEO))) {
        kprintf("[UHCI] phase pid=0x%02x FAILED ctrl=0x%x\n", pid, tc);
        r = -1;
    }
    uhci_disarm(qh_ep0, td_ep0);
    ep0_busy = false;
    return r;
}

/*
 * Full control transfer: SETUP / [DATA] / STATUS.
 * dir_in = direction of the data stage (data-less transfers use OUT).
 */
static int uhci_ctrl(const uint8_t setup[8], void *data, uint16_t len,
                     bool dir_in) {
    /* USB control transfer: SETUP is always DATA0; the DATA stage (if any)
     * is DATA1; STATUS is always DATA1.  QEMU (per USB spec) applies the
     * SET_ADDRESS value only when the STATUS phase succeeds, so every phase
     * of this transfer still targets the OLD address (dev_addr is bumped by
     * the caller afterwards). */
    if (uhci_ep0_phase(USB_PID_SETUP, 8, 0, setup, true) != 0) return -1;
    if (len > 0) {
        if (uhci_ep0_phase(dir_in ? USB_PID_IN : USB_PID_OUT, len, 1,
                           data, !dir_in) != 0)
            return -1;
        if (dir_in) memcpy(data, buf_data, len);
    }
    if (uhci_ep0_phase(dir_in ? USB_PID_OUT : USB_PID_IN, 1, 0,
                       NULL, false) != 0)
        return -1;
    return 0;
}

/* --- HID keyboard report handling --- */

static char hid_usage_to_ascii(uint8_t usage, bool shift) {
    if (usage >= 0x04 && usage <= 0x1D) {
        char c = (char)('a' + (usage - 0x04));
        return shift ? (char)(c - ('a' - 'A')) : c;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char digits[] = "1234567890";
        static const char syms[]   = "!@#$%^&*()";
        return shift ? syms[usage - 0x1E] : digits[usage - 0x1E];
    }
    switch (usage) {
        case 0x28: return '\n';
        case 0x29: return 0;
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
    }
    return 0;
}

void kbd_report(struct hid_boot_kbd_report *r) {
    bool shift = (r->modifiers & (1u << 1)) != 0;
    char c = hid_usage_to_ascii(r->keys[0], shift);
    if (c != 0) kbd_usb_push(c);
}

/* --- Configuration descriptor walk --- */

static void uhci_parse_config(const uint8_t *cfg, uint16_t len) {
    for (uint16_t off = 0; off + 2 < len;) {
        uint8_t len8 = cfg[off];
        if (len8 < 2) break;
        uint8_t dt = cfg[off + 1];
        if (dt == USB_DT_INTERFACE) {
            const struct usb_interface_descriptor *i =
                (const struct usb_interface_descriptor *)&cfg[off];
            if (i->bInterfaceClass == USB_CLASS_HID &&
                i->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
                i->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD)
                kprintf("[UHCI] HID boot keyboard interface found\n");
        } else if (dt == USB_DT_ENDPOINT) {
            const struct usb_endpoint_descriptor *e =
                (const struct usb_endpoint_descriptor *)&cfg[off];
            if ((e->bEndpointAddress & USB_EP_IN) &&
                (e->bmAttributes & 0x03) == 0x03) {
                kprintf("[UHCI] interrupt IN ep 0x%x mps=%u interval=%u\n",
                        (unsigned)e->bEndpointAddress,
                        (unsigned)e->wMaxPacketSize, (unsigned)e->bInterval);
                kbd_mps = (uint16_t)e->wMaxPacketSize;
            }
        }
        off += len8;
    }
}

/* --- Boot-time endpoint-0 messaging --- */

static int uhci_boot_setup(void) {
    /* GET_DESCRIPTOR(device): the device is still at USB address 0. */
    uint8_t p1[8] = { USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
                      USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_DEVICE,
                      0, 0, 8, 0 };
    dev_addr = 0;
    int r = uhci_ctrl(p1, buf_data, 8, true);

    /* SET_ADDRESS(1) (token still targets addr 0). */
    uint8_t p2[8] = { USB_TYPE_STD | USB_RECIP_DEV, USB_REQ_SET_ADDRESS,
                      1, 0, 0, 0, 0, 0 };
    if (r == 0) r = uhci_ctrl(p2, NULL, 0, false);
    if (r == 0) dev_addr = KBD_ADDR;

    /* GET_DESCRIPTOR(device), full (now with the assigned address). */
    uint8_t p3[8] = { USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
                      USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_DEVICE,
                      0, 0, 18, 0 };
    if (r == 0) r = uhci_ctrl(p3, buf_data, 18, true);

    /* GET_DESCRIPTOR(config): 9-byte header, then full length. */
    uint8_t p4[8] = { USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
                      USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_CONFIG,
                      0, 0, 9, 0 };
    if (r == 0) r = uhci_ctrl(p4, buf_data, 9, true);
    if (r == 0) {
        uint16_t tot = (uint16_t)(buf_data[2] | ((uint16_t)buf_data[3] << 8));
        uint8_t p5[8] = { USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
                          USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_CONFIG,
                          0, 0, (uint8_t)tot, (uint8_t)(tot >> 8) };
        r = uhci_ctrl(p5, buf_data, tot, true);
    }

    /* SET_CONFIGURATION(1). */
    uint8_t p6[8] = { USB_TYPE_STD | USB_RECIP_DEV, USB_REQ_SET_CONFIGURATION,
                      1, 0, 0, 0, 0, 0 };
    if (r == 0) r = uhci_ctrl(p6, NULL, 0, false);

    /* SET_IDLE(0) - HID class request on the interface. */
    uint8_t p7[8] = { USB_TYPE_CLASS | USB_RECIP_IF, USB_REQ_SET_IDLE,
                      0, 0, 0, 0, 0, 0 };
    if (r == 0) r = uhci_ctrl(p7, NULL, 0, false);

    return r;
}

/*
 * The keyboard's interrupt-IN TD came back with an error status.  QEMU
 * clears ACTIVE and sets the timeout/CRC bit when a token misses the
 * device (NODEV) - e.g. after a port reset destroyed the device's address -
 * so before re-arming we must re-run the endpoint-0 dialog to re-assign it.
 * Called from ISR context; guarded against re-entrance.
 */
static void uhci_reenumerate(void) {
    static bool in_progress;
    if (in_progress) return;
    in_progress = true;

    kbd_busy = false;
    kbd_ready = false;
    uhci_disarm(qh_int, td_int);   /* stop the NODEV error storm */

    bool ok = uhci_boot_setup() == 0;
    if (ok) {
        td_int->token = TOKEN(USB_PID_IN, dev_addr, KBD_EP, 0, kbd_mps);
        td_int->control = MAXLEN(kbd_mps) | IOC | ERRCNT3;
        td_int->buffer = (uint32_t)virt_to_phys(buf_kbd);
        kbd_ready = true;
        kbd_busy = true;
        uhci_arm(qh_int, td_int);
        kprintf("[UHCI] re-enumerated, int-IN re-armed (addr=%u)\n",
                (unsigned)dev_addr);
    } else {
        kprintf("[UHCI] re-enumeration failed, int-IN left disarmed\n");
    }
    in_progress = false;
}

/* --- Driver entry --- */

int uhci_init(void) {
    const struct pci_device *pc = pci_find_device(0x0C, 0x03);
    if (pc == NULL) {
        kprintf("[UHCI] no controller found\n");
        return -1;
    }

    kprintf("[UHCI] dev %u:%u.%u irq=%u\n",
            pc->bus, pc->dev, pc->func, pc->irq_line);
    for (int b = 0; b < 6; b++)
        kprintf("[UHCI] BAR%d=0x%x mmio=%d size=0x%x\n", b,
                (unsigned)pc->bar[b], pc->mmio[b],
                (unsigned)pc->bar_size[b]);

    /* UHCI control registers live in an I/O BAR (BAR4 on QEMU's PIIX3). */
    int iobar = -1;
    for (int b = 0; b < 6; b++) {
        if (!pc->mmio[b] && pc->bar[b] && pc->bar[b] != 0xFFFFFFFF) {
            iobar = b;
            break;
        }
    }
    if (iobar < 0) {
        kprintf("[UHCI] no I/O BAR\n");
        return -1;
    }
    uhci_io = (uint16_t)(pc->bar[iobar] & 0xFFFC);

    uint32_t cmd = pci_config_read32(pc->bus, pc->dev, pc->func, 0x04);
    pci_config_write32(pc->bus, pc->dev, pc->func, 0x04, cmd | 0x3);

    /* DMA allocations (each its own page, no 4K-boundary crossing). */
    frame_list = (volatile uint32_t *)pmm_alloc_dma(1);
    qh_root    = (struct uhci_qh *)pmm_alloc_dma(1);
    qh_ep0     = (struct uhci_qh *)pmm_alloc_dma(1);
    qh_int     = (struct uhci_qh *)pmm_alloc_dma(1);
    td_ep0     = (struct uhci_td *)pmm_alloc_dma(1);
    td_int     = (struct uhci_td *)pmm_alloc_dma(1);
    buf_setup  = (uint8_t *)pmm_alloc_dma(1);
    buf_data   = (uint8_t *)pmm_alloc_dma(1);
    buf_kbd    = (uint8_t *)pmm_alloc_dma(1);
    if (frame_list == NULL || qh_root == NULL || qh_ep0 == NULL ||
        qh_int == NULL || td_ep0 == NULL || td_int == NULL ||
        buf_setup == NULL || buf_data == NULL || buf_kbd == NULL) {
        kprintf("[UHCI] DMA allocation failed\n");
        return -1;
    }

    w16(REG_USBCMD, CMD_HCRESET);
    while (r16(REG_USBCMD) & CMD_HCRESET) { }

    memset((void *)(uintptr_t)frame_list, 0, PAGE_SIZE);
    memset(qh_root, 0, PAGE_SIZE);
    memset(qh_ep0, 0, PAGE_SIZE);
    memset(qh_int, 0, PAGE_SIZE);
    memset(td_ep0, 0, PAGE_SIZE);
    memset(td_int, 0, PAGE_SIZE);
    memset(buf_setup, 0, PAGE_SIZE);
    memset(buf_data, 0, PAGE_SIZE);
    memset(buf_kbd, 0, PAGE_SIZE);

    td_ep0->link = TD_TERMINATED;
    td_int->link = TD_TERMINATED;

    /* Schedule: every frame slot -> qh_root -> qh_ep0 -> qh_int -> back. */
    qh_root->element = TD_TERMINATED;
    qh_root->link = (uint32_t)virt_to_phys(qh_ep0) | TD_QH;
    qh_ep0->element = TD_TERMINATED;
    qh_ep0->link = (uint32_t)virt_to_phys(qh_int) | TD_QH;
    qh_int->element = TD_TERMINATED;
    qh_int->link = (uint32_t)virt_to_phys(qh_root) | TD_QH;

    for (unsigned i = 0; i < 1024; i++)
        frame_list[i] = (uint32_t)virt_to_phys(qh_root) | TD_QH;

    w32(REG_FRBASEADD, (uint32_t)virt_to_phys((void *)(uintptr_t)frame_list));
    w16(REG_USBCMD, CMD_RUN | CMD_CF);
    uhci_run_wait();
    w16(REG_USBINTR, INTR_TI | INTR_IOCE);

    kprintf("[UHCI] running, FRNUM=%u\n", (unsigned)r16(REG_FRNUM));
    kprintf("[UHCI] root.elem=0x%x root.link=0x%x ep0.elem=0x%x int.elem=0x%x\n",
            (unsigned)qh_root->element, (unsigned)qh_root->link,
            (unsigned)qh_ep0->element, (unsigned)qh_int->element);

    uint16_t p1 = r16(REG_PORTSC1);
    uint16_t p2 = r16(REG_PORTSC2);
    kprintf("[UHCI] PORTSC1=0x%x PORTSC2=0x%x\n", (unsigned)p1, (unsigned)p2);

    /* Give the controller a moment to report any attached device. */
    if (!(p1 & PORT_CCS) && !(p2 & PORT_CCS)) {
        for (unsigned i = 0; i < 4000; i++) {
            __asm__ volatile("sti; hlt" ::: "memory");
            p1 = r16(REG_PORTSC1);
            p2 = r16(REG_PORTSC2);
            if ((p1 & PORT_CCS) || (p2 & PORT_CCS)) break;
        }
    }

    uint16_t port_reg;
    if (p1 & PORT_CCS) {
        port_reg = REG_PORTSC1;
    } else if (p2 & PORT_CCS) {
        port_reg = REG_PORTSC2;
    } else {
        kprintf("[UHCI] no device on either port\n");
        return -1;
    }

    /* Reset the connected port; this enables the device (PED). */
    w16(port_reg, r16(port_reg) | PORT_PR);
    for (unsigned i = 0; i < 50000; i++) {
        __asm__ volatile("sti; hlt" ::: "memory");
        if (!(r16(port_reg) & PORT_PR)) break;
    }
    /* Enable the port explicitly (QEMU latches EN only if a device is
     * connected; real hardware usually sets PED on its own). */
    w16(port_reg, r16(port_reg) | PORT_EN);
    p1 = r16(REG_PORTSC1);
    p2 = r16(REG_PORTSC2);
    kprintf("[UHCI] after port reset: P1=0x%x P2=0x%x\n",
            (unsigned)p1, (unsigned)p2);

    if (!(p1 & PORT_CCS) && !(p2 & PORT_CCS)) {
        kprintf("[UHCI] device vanished after reset\n");
        return -1;
    }
    kprintf("[UHCI] device connected (P1=0x%x P2=0x%x)\n",
            (unsigned)p1, (unsigned)p2);

    irq_register_handler(pc->irq_line, uhci_irq_handler);

    if (uhci_boot_setup() != 0) {
        kprintf("[UHCI] configuration failed\n");
        return -1;
    }
    kprintf("[UHCI] endpoint-0 setup complete\n");

    /* buf_data currently holds the config descriptor. */
    uhci_parse_config(buf_data, 0x80);

    /* Re-fetch the device descriptor for a pretty banner. */
    {
        uint8_t p8[8] = { USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
                          USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_DEVICE,
                          0, 0, 18, 0 };
        if (uhci_ctrl(p8, buf_data, 18, true) == 0) {
            struct usb_device_descriptor *dd =
                (struct usb_device_descriptor *)buf_data;
            kprintf("[UHCI] device vid=0x%x pid=0x%x class=%u subclass=%u\n",
                    dd->idVendor, dd->idProduct, dd->bDeviceClass,
                    dd->bDeviceSubClass);
        }
    }

    td_int->token = TOKEN(USB_PID_IN, dev_addr, KBD_EP, 0, kbd_mps);
    td_int->control = MAXLEN(kbd_mps) | IOC | ERRCNT3;
    td_int->buffer = (uint32_t)virt_to_phys(buf_kbd);
    uhci_arm(qh_int, td_int);
    kbd_ready = true;
    kbd_busy = true;

    kprintf("[UHCI] HID boot keyboard ready\n");
    return 0;
}