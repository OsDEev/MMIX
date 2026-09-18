#include <io.h>
#include <libk.h>
#include <pmap.h>
#include <pmm.h>
#include <pci.h>
#include <string.h>

#include "e1000.h"

#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EEPROM   0x0014
#define E1000_REG_IMC      0x00D8
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818

#define E1000_CTRL_RST     (1u << 26)
#define E1000_CTRL_SLU     (1u << 6)
#define E1000_CTRL_LRST    (1u << 3)

#define E1000_STATUS_LU    (1u << 1)

#define E1000_RCTL_EN      (1u << 1)
#define E1000_RCTL_BAM     (1u << 15)

#define E1000_TCTL_EN      (1u << 1)
#define E1000_TCTL_PSP     (1u << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_COLD_FD (0x40u << E1000_TCTL_COLD_SHIFT)

#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)
#define E1000_TXD_STAT_DD  (1u << 0)

#define E1000_RXD_STAT_DD  (1u << 0)
#define E1000_RXD_STAT_EOP (1u << 1)

#define E1000_RXD_ERR_CE  (1u << 0)
#define E1000_RXD_ERR_SE  (1u << 1)
#define E1000_RXD_ERR_SEQ (1u << 2)
#define E1000_RXD_ERR_CXE (1u << 4)
#define E1000_RXD_ERR_RXE (1u << 7)

#define E1000_RX_RING 32
#define E1000_TX_RING 32
#define E1000_RX_BUF  2048
#define E1000_TX_BUF  2048
#define E1000_EEPROM_TIMEOUT 500000
#define E1000_RESET_TIMEOUT  10000000
#define E1000_TX_TIMEOUT     10000000

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    volatile uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    volatile uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

_Static_assert(sizeof(struct e1000_rx_desc) == 16, "RX descriptor size");
_Static_assert(sizeof(struct e1000_tx_desc) == 16, "TX descriptor size");
_Static_assert(offsetof(struct e1000_tx_desc, cmd) == 11, "TX command offset");
_Static_assert(offsetof(struct e1000_tx_desc, status) == 12, "TX status offset");
_Static_assert(offsetof(struct e1000_rx_desc, status) == 12, "RX status offset");

static volatile uint8_t *regs;
static bool nic_present;
static bool nic_up;
static uint8_t mac[6];

static struct e1000_rx_desc *rx_ring;
static struct e1000_tx_desc *tx_ring;
static bool rx_discard;
static uint64_t rx_ring_phys;
static uint64_t tx_ring_phys;
static uint8_t *rx_bufs;
static uint8_t *tx_bufs;
static uint64_t rx_bufs_phys;
static uint64_t tx_bufs_phys;
static uint16_t rx_next;
static uint16_t tx_tail;

#define E1000_MMIO_VA 0xFFFFFFFF90000000ULL

static inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(regs + off);
}

static inline void wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(regs + off) = val;
}

static inline void e1000_mfence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static inline bool e1000_spin(uint32_t off, uint32_t mask, bool wait_set,
                              uint32_t max_loops) {
    for (uint32_t i = 0; i < max_loops; i++) {
        uint32_t v = rd(off);
        if (wait_set ? ((v & mask) != 0) : ((v & mask) == 0))
            return true;
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

static uint16_t e1000_eeprom_read(uint8_t word) {
    wr(E1000_REG_EEPROM, 1u | ((uint32_t)word << 8));
    if (!e1000_spin(E1000_REG_EEPROM, 1u << 4, true, E1000_EEPROM_TIMEOUT))
        return 0xFFFFu;
    return (uint16_t)(rd(E1000_REG_EEPROM) >> 16);
}

static void e1000_pci_enable(const struct pci_device *d) {
    uint32_t cmd = pci_config_read32(d->bus, d->dev, d->func, 0x04);
    cmd |= (1u << 0) | (1u << 1) | (1u << 2);
    pci_config_write32(d->bus, d->dev, d->func, 0x04, cmd);
}

static bool e1000_map_mmio(const struct pci_device *d) {
    int bar = -1;
    for (int i = 0; i < 6; i++) {
        if (d->mmio[i] && d->bar_size[i] > 0) {
            bar = i;
            break;
        }
    }
    if (bar < 0) {
        kprintf("[E1000] no MMIO BAR\n");
        return false;
    }

    uint64_t phys = d->bar[bar] & 0xFFFFFFF0ULL;
    uint32_t size = d->bar_size[bar];
    if (size < 0x4000) {
        kprintf("[E1000] BAR%d too small (%u)\n", bar, (unsigned)size);
        return false;
    }
    if (d->bar[bar] & (1u << 3)) {
        kprintf("[E1000] BAR%d is 64-bit, unsupported\n", bar);
        return false;
    }

    uint64_t va = E1000_MMIO_VA;
    for (uint32_t i = 0; i < size; i += PAGE_SIZE) {
        pmap_map(kernel_pml4, va + i, phys + i,
                 PTE_WRITE | PTE_NOCACHE | PTE_PRESENT);
    }
    e1000_mfence();
    regs = (volatile uint8_t *)va;
    kprintf("[E1000] MMIO BAR%d phys=0x%x size=0x%x va=0x%x\n",
            bar, (unsigned)phys, (unsigned)size, (unsigned)va);
    return true;
}

static bool e1000_alloc_rings(void) {
    rx_ring = pmm_alloc_dma(1);
    tx_ring = pmm_alloc_dma(1);
    rx_bufs = pmm_alloc_dma(16);
    tx_bufs = pmm_alloc_dma(16);
    if (rx_ring == NULL || tx_ring == NULL ||
        rx_bufs == NULL || tx_bufs == NULL) {
        kprintf("[E1000] ring allocation failed\n");
        if (rx_ring) pmm_free(rx_ring, 1);
        if (tx_ring) pmm_free(tx_ring, 1);
        if (rx_bufs) pmm_free(rx_bufs, 16);
        if (tx_bufs) pmm_free(tx_bufs, 16);
        rx_ring = NULL;
        tx_ring = NULL;
        rx_bufs = NULL;
        tx_bufs = NULL;
        return false;
    }
    memset(rx_ring, 0, PAGE_SIZE);
    memset(tx_ring, 0, PAGE_SIZE);
    memset(rx_bufs, 0, 16 * PAGE_SIZE);
    memset(tx_bufs, 0, 16 * PAGE_SIZE);
    rx_ring_phys = virt_to_phys(rx_ring);
    tx_ring_phys = virt_to_phys(tx_ring);
    rx_bufs_phys = virt_to_phys(rx_bufs);
    tx_bufs_phys = virt_to_phys(tx_bufs);
    return true;
}

static void e1000_setup_rx(void) {
    for (int i = 0; i < E1000_RX_RING; i++) {
        rx_ring[i].addr = rx_bufs_phys + (uint64_t)i * E1000_RX_BUF;
        rx_ring[i].length = 0;
        rx_ring[i].csum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;
    }
    e1000_mfence();
    wr(E1000_REG_RDBAL, (uint32_t)(rx_ring_phys & 0xFFFFFFFFu));
    wr(E1000_REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    wr(E1000_REG_RDLEN, E1000_RX_RING * 16);
    wr(E1000_REG_RDH, 0);
    wr(E1000_REG_RDT, E1000_RX_RING - 1);
    rx_next = 0;
}

static void e1000_setup_tx(void) {
    for (int i = 0; i < E1000_TX_RING; i++) {
        tx_ring[i].addr = tx_bufs_phys + (uint64_t)i * E1000_TX_BUF;
        tx_ring[i].length = 0;
        tx_ring[i].cso = 0;
        tx_ring[i].cmd = 0;
        tx_ring[i].status = E1000_TXD_STAT_DD;
        tx_ring[i].css = 0;
        tx_ring[i].special = 0;
    }
    e1000_mfence();
    wr(E1000_REG_TDBAL, (uint32_t)(tx_ring_phys & 0xFFFFFFFFu));
    wr(E1000_REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    wr(E1000_REG_TDLEN, E1000_TX_RING * 16);
    wr(E1000_REG_TDH, 0);
    wr(E1000_REG_TDT, 0);
    tx_tail = 0;
}

static void e1000_read_mac(void) {
    static const uint8_t fallback[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint16_t w;
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        w = e1000_eeprom_read((uint8_t)i);
        if (w == 0xFFFFu && rd(E1000_REG_EEPROM) == 0xFFFFu) {
            ok = false;
            break;
        }
        mac[i * 2]     = (uint8_t)(w & 0xFF);
        mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    if (!ok || (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 &&
                mac[3] == 0 && mac[4] == 0 && mac[5] == 0)) {
        for (int i = 0; i < 6; i++) mac[i] = fallback[i];
        kprintf("[E1000] MAC read failed, using fallback\n");
    }
}

int e1000_init(void) {
    const struct pci_device *d = NULL;
    for (int i = 0; i < PCI_MAX_DEVICES; i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (dev == NULL) break;
        if (dev->vendor_id == E1000_VID &&
            dev->device_id == E1000_DID_82540EM) {
            d = dev;
            break;
        }
    }
    if (d == NULL) {
        kprintf("[E1000] no 8086:100e device\n");
        return -1;
    }
    kprintf("[E1000] 82540EM at %u:%u.%u\n", d->bus, d->dev, d->func);

    if (!e1000_map_mmio(d)) return -1;

    e1000_pci_enable(d);
    e1000_mfence();

    wr(E1000_REG_IMC, 0xFFFFFFFFu);
    e1000_mfence();

    uint32_t ctrl = rd(E1000_REG_CTRL);
    wr(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    e1000_mfence();
    if (!e1000_spin(E1000_REG_CTRL, E1000_CTRL_RST, false,
                    E1000_RESET_TIMEOUT)) {
        kprintf("[E1000] reset did not complete\n");
        return -1;
    }

    wr(E1000_REG_IMC, 0xFFFFFFFFu);

    ctrl = rd(E1000_REG_CTRL);
    ctrl &= ~E1000_CTRL_LRST;
    ctrl |= E1000_CTRL_SLU;
    wr(E1000_REG_CTRL, ctrl);
    e1000_mfence();

    if (!e1000_alloc_rings()) return -1;

    e1000_read_mac();
    kprintf("[E1000] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    e1000_setup_tx();

    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (0x0Fu << E1000_TCTL_CT_SHIFT) |
                    E1000_TCTL_COLD_FD;
    wr(E1000_REG_TCTL, tctl);
    e1000_mfence();

    e1000_setup_rx();

    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM;
    wr(E1000_REG_RCTL, rctl);
    e1000_mfence();

    nic_present = true;
    nic_up = true;
    kprintf("[E1000] ready (status=0x%x, link=%s)\n",
            rd(E1000_REG_STATUS),
            (rd(E1000_REG_STATUS) & E1000_STATUS_LU) ? "up" : "down");
    return 0;
}

bool e1000_present(void) {
    return nic_present;
}

void e1000_get_mac(uint8_t mac_out[6]) {
    if (mac_out == NULL) return;
    for (int i = 0; i < 6; i++) mac_out[i] = mac[i];
}

int e1000_send(const void *frame, size_t len) {
    if (!nic_up || frame == NULL) return -1;
    if (len == 0 || len > E1000_TX_BUF) return -1;

    if (!(tx_ring[tx_tail].status & E1000_TXD_STAT_DD)) return -1;
    e1000_mfence();

    memcpy(tx_bufs + (size_t)tx_tail * E1000_TX_BUF, frame, len);

    struct e1000_tx_desc *d = &tx_ring[tx_tail];
    d->addr = tx_bufs_phys + (uint64_t)tx_tail * E1000_TX_BUF;
    d->length = (uint16_t)len;
    d->cso = 0;
    d->cmd = (uint8_t)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
                       E1000_TXD_CMD_RS);
    d->status = 0;
    d->css = 0;
    d->special = 0;
    e1000_mfence();

    uint16_t next = (uint16_t)((tx_tail + 1) % E1000_TX_RING);
    wr(E1000_REG_TDT, next);
    tx_tail = next;

    uint16_t slot = (uint16_t)((next + E1000_TX_RING - 1) % E1000_TX_RING);
    uint32_t spins = 0;
    while (!(tx_ring[slot].status & E1000_TXD_STAT_DD)) {
        if (++spins > E1000_TX_TIMEOUT) {
            kprintf("[E1000] TX timeout (DD never set)\n");
            return -1;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    e1000_mfence();
    return (tx_ring[slot].status & 0x06u) ? -1 : 0;
}

int e1000_poll(void *buf, size_t cap) {
    if (!nic_up || buf == NULL || cap == 0) return -1;

    struct e1000_rx_desc *d = &rx_ring[rx_next];
    if (!(d->status & E1000_RXD_STAT_DD)) return 0;
    e1000_mfence();

    uint16_t len = d->length;
    bool eop = (d->status & E1000_RXD_STAT_EOP) != 0;
    uint8_t errs = d->errors;
    uint16_t consumed = rx_next;
    bool ok = !rx_discard && eop && len != 0 && len <= E1000_RX_BUF && len <= cap &&
              (errs & (E1000_RXD_ERR_CE | E1000_RXD_ERR_SE |
                       E1000_RXD_ERR_SEQ | E1000_RXD_ERR_CXE |
                       E1000_RXD_ERR_RXE)) == 0;

    rx_discard = !eop;
    int rc = 0;
    if (ok) {
        memcpy(buf, rx_bufs + (size_t)consumed * E1000_RX_BUF, (size_t)len);
        rc = (int)len;
    }

    d->status = 0;
    d->errors = 0;
    d->length = 0;
    rx_next = (uint16_t)((rx_next + 1) % E1000_RX_RING);
    e1000_mfence();
    wr(E1000_REG_RDT, consumed);
    return rc;
}
