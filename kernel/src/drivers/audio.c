/*
 * Sound Blaster 16 audio driver (ISA).
 *
 * Fixed resources: DSP I/O base 0x220, IRQ 5, 8-bit DMA channel 1.
 * Output format: unsigned 8-bit mono PCM at AUDIO_HZ (22.05 kHz), played via
 * single-cycle 8-bit DMA transfers into a 4 KiB silence-safe buffer.
 * Exposed to userspace as /dev/audio (write = play PCM).
 */
#include <audio.h>
#include <chardev.h>
#include <idt.h>
#include <io.h>
#include <libk.h>
#include <pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vfs.h>

/* --- SB16 ports (base 0x220) -------------------------------------------- */
#define SB16_BASE           0x220
#define SB16_MIXER_ADDR     (SB16_BASE + 0x4)
#define SB16_MIXER_DATA     (SB16_BASE + 0x5)
#define SB16_DSP_RESET      (SB16_BASE + 0x6)
#define SB16_DSP_READ       (SB16_BASE + 0xA)
#define SB16_DSP_STATUS     (SB16_BASE + 0xC)
#define SB16_DSP_WRITE      (SB16_BASE + 0xC)
#define SB16_DSP_ACK        (SB16_BASE + 0xE)  /* read: IRQ ack + data rdy */

#define DSP_STATUS_IN_READY 0x80
#define DSP_STATUS_BUSY     0x80

#define DSP_CMD_OUT8       0x14  /* 8-bit DMA output, single cycle */
#define DSP_CMD_SET_TC     0x40  /* set 8-bit time constant */
#define DSP_CMD_VERSION    0xE1

/* --- Primary 8237 DMA controller (8-bit channel 1) ---------------------- */
#define DMA1_ADDR   0x02
#define DMA1_COUNT  0x03
#define DMA1_MASK   0x0A
#define DMA1_MODE   0x0B
#define DMA1_FF     0x0C
#define DMA1_PAGE   0x83
#define DMA_MODE_CH1_WRITE_SINGLE 0x49

#define AUDIO_DMA_SIZE 4096

static volatile bool sb16_present = false;
static volatile bool sb16_done = false;
static volatile bool sb16_busy = false;
static uint8_t *dma_buf;
static uint32_t dma_buf_phys;

static void sb16_wait_write(void) {
    while (inb(SB16_DSP_STATUS) & DSP_STATUS_BUSY) io_wait();
}

static void sb16_write_cmd(uint8_t v) {
    sb16_wait_write();
    outb(SB16_DSP_WRITE, v);
}

static bool sb16_reset(void) {
    outb(SB16_DSP_RESET, 1);
    for (int i = 0; i < 1000; i++) io_wait();
    outb(SB16_DSP_RESET, 0);

    for (int i = 0; i < 100000; i++) {
        io_wait();
        if (inb(SB16_DSP_ACK) & DSP_STATUS_IN_READY) {
            return inb(SB16_DSP_READ) == 0xAA;
        }
    }
    return false;
}

static void sb16_read_version(void) {
    sb16_write_cmd(DSP_CMD_VERSION);
    for (int i = 0; i < 100000; i++) {
        io_wait();
        if (inb(SB16_DSP_ACK) & DSP_STATUS_IN_READY) {
            uint8_t maj = inb(SB16_DSP_READ);
            for (int j = 0; j < 100000; j++) {
                io_wait();
                if (inb(SB16_DSP_ACK) & DSP_STATUS_IN_READY) {
                    kprintf("[AUDIO] SB16 DSP v%u.%u\n", maj,
                            inb(SB16_DSP_READ));
                    return;
                }
            }
            return;
        }
    }
}

static void sb16_set_rate(uint16_t hz) {
    uint8_t tc = (uint8_t)(256u - 1000000u / hz);
    sb16_write_cmd(DSP_CMD_SET_TC);
    sb16_write_cmd(tc);
}

static void mixer_write(uint8_t reg, uint8_t val) {
    outb(SB16_MIXER_ADDR, reg);
    io_wait();
    outb(SB16_MIXER_DATA, val);
    io_wait();
}

static void sb16_irq(void) {
    (void)inb(SB16_DSP_ACK);
    sb16_done = true;
}

static void dma_start_8bit(uint16_t count) {
    outb(DMA1_MASK, 0x04 | 0x01);            /* mask channel 1 */
    outb(DMA1_FF, 0);
    outb(DMA1_ADDR, dma_buf_phys & 0xFF);
    outb(DMA1_ADDR, (dma_buf_phys >> 8) & 0xFF);
    outb(DMA1_FF, 0);
    outb(DMA1_COUNT, count & 0xFF);
    outb(DMA1_COUNT, count >> 8);
    outb(DMA1_PAGE, (dma_buf_phys >> 16) & 0xFF);
    outb(DMA1_MODE, DMA_MODE_CH1_WRITE_SINGLE);
    outb(DMA1_MASK, 0x01);                   /* unmask channel 1 */
}

static void sb16_start_out(uint16_t count) {
    sb16_write_cmd(DSP_CMD_OUT8);
    sb16_write_cmd(count & 0xFF);
    sb16_write_cmd(count >> 8);
}

static void sb16_wait_done(void) {
    sb16_done = false;
    for (uint32_t w = 0; w < 20000; w++) {
        if (sb16_done) return;
        outb(DMA1_FF, 0);
        uint16_t cur = (uint16_t)(inb(DMA1_COUNT) |
                                  ((uint16_t)inb(DMA1_COUNT) << 8));
        if (cur == 0) return;
        __asm__ volatile("sti; hlt");
    }
    kprintf("[AUDIO] playback timeout\n");
}

int audio_write(const char *buf, size_t n) {
    if (!sb16_present) return (int)n;
    if (buf == NULL || n == 0) return (int)n;

    while (sb16_busy) __asm__ volatile("sti; hlt");
    sb16_busy = true;

    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > AUDIO_DMA_SIZE) chunk = AUDIO_DMA_SIZE;
        memcpy(dma_buf, buf + off, chunk);
        dma_start_8bit((uint16_t)(chunk - 1));
        sb16_start_out((uint16_t)(chunk - 1));
        sb16_wait_done();
        off += chunk;
    }

    sb16_busy = false;
    return (int)n;
}

static int audio_read(char *buf, size_t n) {
    (void)buf;
    (void)n;
    return 0;
}

static struct chardev dev_audio = {"audio", audio_read, audio_write};

int audio_init(void) {
    if (!sb16_reset()) {
        kprintf("[AUDIO] no SB16 on 0x%x\n", SB16_BASE);
        return -1;
    }

    sb16_read_version();

    mixer_write(0x22, 0xFF);  /* master volume, both channels */
    mixer_write(0x04, 0xFF);  /* voice volume */

    sb16_set_rate(AUDIO_HZ);

    for (int a = 0; a < 8; a++) {
        void *b = pmm_alloc_dma(1);
        if (b == NULL) break;
        uint64_t pa = virt_to_phys(b);
        if (pa < (16ull * 1024 * 1024)) {
            dma_buf = b;
            dma_buf_phys = (uint32_t)pa;
            break;
        }
        pmm_free(b, 1);
    }
    if (dma_buf == NULL) {
        kprintf("[AUDIO] no <16MiB DMA buffer\n");
        return -1;
    }

    irq_register_handler(5, sb16_irq);
    vfs_mount_dev("audio", &dev_audio);
    sb16_present = true;

    kprintf("[AUDIO] SB16 0x%x IRQ5 DMA1 %u Hz %u-byte buffer\n",
            SB16_BASE, AUDIO_HZ, AUDIO_DMA_SIZE);
    return 0;
}