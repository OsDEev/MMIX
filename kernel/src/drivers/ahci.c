/*
 * AHCI driver (v2).
 * Detects HBA via PCI, initializes ports, provides synchronous sector read.
 * Uses WBINVD for cache coherency to ensure DMA buffers are visible to HBA.
 */
#include <ahci.h>
#include <blockdev.h>
#include <kheap.h>
#include <libk.h>
#include <pmap.h>
#include <pmm.h>
#include <pci.h>
#include <string.h>

extern volatile struct limine_hhdm_request hhdm_request;

static volatile uint32_t *hba;
static uint64_t hba_phys;
static struct ahci_port ports[AHCI_MAX_PORTS];
static int num_ports = 0;

static inline uint32_t hba_read(uint32_t reg) {
    return *(volatile uint32_t *)((uint8_t *)hba + reg);
}

static inline void hba_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)hba + reg) = val;
}

static inline uint32_t port_read(int port, uint32_t reg) {
    return *(volatile uint32_t *)((uint8_t *)hba + 0x100 + port * 0x80 + reg);
}

static inline void port_write(int port, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)hba + 0x100 + port * 0x80 + reg) = val;
}

static inline void io_mfence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* Flush a range from CPU cache (write-back + invalidate) for specific lines. */
static inline void flush_range(void *addr, size_t len) {
    uint8_t *p = (uint8_t *)((uintptr_t)addr & ~(uint64_t)(64 - 1));
    uint8_t *end = (uint8_t *)(((uintptr_t)addr + len + 63) & ~(uint64_t)(64 - 1));
    for (; p < end; p += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
    }
}

static bool spin_tfd(int port, uint8_t mask, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 1000; i++) {
        if (!(port_read(port, AHCI_PORT_TFD) & mask))
            return true;
    }
    return false;
}

static void port_stop(int p) {
    uint32_t cmd = port_read(p, AHCI_PORT_CMD);
    cmd &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);
    port_write(p, AHCI_PORT_CMD, cmd);
    io_mfence();
    for (int i = 0; i < 5000000; i++) {
        uint32_t c = port_read(p, AHCI_PORT_CMD);
        if (!(c & (AHCI_CMD_FR | AHCI_CMD_CR)))
            return;
    }
    kprintf("[AHCI] Port %d: CMD still running (0x%x)\n",
            p, port_read(p, AHCI_PORT_CMD));
}

static void port_start(int p) {
    while (port_read(p, AHCI_PORT_CMD) & AHCI_CMD_CR)
        ;
    uint32_t cmd = port_read(p, AHCI_PORT_CMD);
    cmd |= AHCI_CMD_FRE | AHCI_CMD_ST;
    port_write(p, AHCI_PORT_CMD, cmd);
}

static bool port_init(int p) {
    kprintf("[AHCI] Port %d: stopping...\n", p);
    port_stop(p);

    uint32_t ssts = port_read(p, AHCI_PORT_SSTS);
    uint8_t det = ssts & 0x0F;
    kprintf("[AHCI] Port %d: initial SSTS det=%d\n", p, det);

    if (det != 3) {
        port_write(p, AHCI_PORT_SCTL, 1);
        io_mfence();
        for (volatile int i = 0; i < 10000000; i++) {}
        port_write(p, AHCI_PORT_SCTL, 0);
        io_mfence();

        bool found = false;
        for (int tries = 0; tries < 500; tries++) {
            ssts = port_read(p, AHCI_PORT_SSTS);
            det = ssts & 0x0F;
            if (det == 3) { found = true; break; }
            for (volatile int i = 0; i < 100000; i++) {}
        }
        if (!found) {
            kprintf("[AHCI] Port %d: link not ready (det=%d)\n", p, det);
            return false;
        }
    }
    kprintf("[AHCI] Port %d: link ready (det=%d)\n", p, det);

    /* Clear any stale error/port state */
    port_write(p, AHCI_PORT_SERR, ~0u);
    port_write(p, AHCI_PORT_IS, ~0u);
    io_mfence();

    /* Allocate DMA-safe command list (1 KiB aligned, 1 page) */
    void *cl_mem = pmm_alloc_dma(1);
    if (!cl_mem) { kprintf("[AHCI] Port %d: CL alloc fail\n", p); return false; }
    memset(cl_mem, 0, PAGE_SIZE);
    ports[p].cl = cl_mem;
    ports[p].cl_phys = virt_to_phys(cl_mem);

    /* Allocate DMA-safe FIS receive area (256 bytes, 1 page) */
    void *fis_mem = pmm_alloc_dma(1);
    if (!fis_mem) { kprintf("[AHCI] Port %d: FIS alloc fail\n", p); return false; }
    memset(fis_mem, 0, PAGE_SIZE);
    ports[p].fis = fis_mem;
    ports[p].fis_phys = virt_to_phys(fis_mem);

    /* Allocate DMA-safe command table (1 page, covers FIS + PRDT) */
    void *ct_mem = pmm_alloc_dma(1);
    if (!ct_mem) { kprintf("[AHCI] Port %d: CT alloc fail\n", p); return false; }
    memset(ct_mem, 0, PAGE_SIZE);
    ports[p].cmd_table = (struct hba_cmd_table *)ct_mem;
    ports[p].ct_phys = virt_to_phys(ct_mem);

    /* Program port registers (MMIO writes, no cache flush needed). */

    port_write(p, AHCI_PORT_CLB,  (uint32_t)ports[p].cl_phys);
    port_write(p, AHCI_PORT_CLBU, (uint32_t)(ports[p].cl_phys >> 32));
    port_write(p, AHCI_PORT_FB,   (uint32_t)ports[p].fis_phys);
    port_write(p, AHCI_PORT_FBU,  (uint32_t)(ports[p].fis_phys >> 32));
    io_mfence();

    /* Verify CLB was written correctly */
    uint32_t read_clb = port_read(p, AHCI_PORT_CLB);
    uint32_t read_clbu = port_read(p, AHCI_PORT_CLBU);
    uint64_t read_cl_addr = read_clb | ((uint64_t)read_clbu << 32);
    if (read_cl_addr != ports[p].cl_phys) {
        kprintf("[AHCI] Port %d: CLB mismatch! wrote=0x%lx read=0x%lx\n",
                p, (unsigned long)ports[p].cl_phys, (unsigned long)read_cl_addr);
    }

    port_start(p);

    uint32_t cmd_readback = port_read(p, AHCI_PORT_CMD);
    kprintf("[AHCI] Port %d: CMD=0x%x (ST=%d FRE=%d)\n",
            p, cmd_readback,
            !!(cmd_readback & AHCI_CMD_ST),
            !!(cmd_readback & AHCI_CMD_FRE));

    ports[p].slot = 0;
    ports[p].active = true;
    return true;
}

int ahci_read_sectors(int port, uint64_t lba, uint32_t count, void *buf) {
    if (port < 0 || port >= num_ports || !ports[port].active) {
        kprintf("[AHCI] read: invalid port %d\n", port);
        return -1;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = count;

    void *bounce = pmm_alloc_dma(1);
    if (!bounce) {
        kprintf("[AHCI] read: DMA bounce alloc failed\n");
        return -1;
    }
    uint64_t bounce_phys = virt_to_phys(bounce);

    while (remaining > 0) {
        if (!spin_tfd(port, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ, 10000)) {
            kprintf("[AHCI] read: TFD timeout TFD=0x%x\n",
                    port_read(port, AHCI_PORT_TFD));
            pmm_free(bounce, 1);
            return -1;
        }

        int slot = 0;
        uint16_t xfer = (remaining > 256) ? 256 : (uint16_t)remaining;
        uint32_t bytes = xfer * 512;

        /* ---- Build Command Table ---- */
        struct hba_cmd_table *ct = (struct hba_cmd_table *)phys_to_virt(ports[port].ct_phys);
        memset(ct, 0, sizeof(struct hba_cmd_table));

        /* H2D Register FIS (20 bytes at offset 0 of CT) */
        struct hba_cmdfis *fis = (struct hba_cmdfis *)ct->cfis;
        fis->fis_type  = 0x27;          /* H2D */
        fis->pmport    = (1u << 7);     /* C=1: command FIS */
        fis->command   = 0x25;          /* READ DMA EXT */
        fis->feature   = 0;
        fis->device    = (1u << 6);     /* LBA mode */
        fis->count     = xfer;
        fis->lba_low       = (uint8_t)(lba & 0xFF);
        fis->lba_mid       = (uint8_t)((lba >> 8) & 0xFF);
        fis->lba_high      = (uint8_t)((lba >> 16) & 0xFF);
        fis->lba_low_exp   = (uint8_t)((lba >> 24) & 0xFF);
        fis->lba_mid_exp   = (uint8_t)((lba >> 32) & 0xFF);
        fis->lba_high_exp  = (uint8_t)((lba >> 40) & 0xFF);
        fis->icc      = 0;
        fis->control  = 0;

        /* PRDT entry at offset 128 of CT */
        struct hba_prdt_entry *prdt = ct->prdt;
        prdt[0].dba      = bounce_phys;
        prdt[0].reserved = 0;
        prdt[0].info     = (bytes - 1) | (1u << 31);  /* interrupt on complete */

        /* ---- Build Command Header (slot 0) ---- */
        struct hba_cmdh *hdr = (struct hba_cmdh *)phys_to_virt(ports[port].cl_phys);
        memset(&hdr[slot], 0, sizeof(struct hba_cmdh));
        hdr[slot].opts  = 5;             /* CFL=5 DWORDs (20-byte H2D FIS) */
        hdr[slot].prdtl = 1;
        hdr[slot].prdbc = 0;
        hdr[slot].ctba  = ports[port].ct_phys;

        /* ---- Flush CL + CT so HBA sees our writes ---- */
        flush_range(&hdr[slot], sizeof(struct hba_cmdh));
        flush_range(ct, sizeof(struct hba_cmd_table) + sizeof(struct hba_prdt_entry));
        io_mfence();

        /* ---- Issue command ---- */
        port_write(port, AHCI_PORT_IS, ~0u);  /* clear all interrupt bits */
        port_write(port, AHCI_PORT_CI, (1u << slot));

        /* ---- Poll for completion ---- */
        bool ok = false;
        for (int poll = 0; poll < 10000000; poll++) {
            if (!(port_read(port, AHCI_PORT_CI) & (1u << slot))) {
                ok = true;
                break;
            }
            uint32_t is = port_read(port, AHCI_PORT_IS);
            if (is & AHCI_PORT_IS_TFES) {
                kprintf("[AHCI] read: TFES! IS=0x%x TFD=0x%x\n",
                        is, port_read(port, AHCI_PORT_TFD));
                pmm_free(bounce, 1);
                return -1;
            }
        }

        if (!ok) {
            kprintf("[AHCI] read: TIMEOUT lba=%lu CI=0x%x IS=0x%x TFD=0x%x\n",
                    (unsigned long)lba,
                    port_read(port, AHCI_PORT_CI),
                    port_read(port, AHCI_PORT_IS),
                    port_read(port, AHCI_PORT_TFD));

            pmm_free(bounce, 1);
            return -1;
        }

        /* ---- Command complete: flush bounce buffer so CPU reads fresh DMA data ---- */
        flush_range(bounce, bytes);

        memcpy(dst, bounce, bytes);
        dst       += bytes;
        lba       += xfer;
        remaining -= xfer;
    }

    pmm_free(bounce, 1);
    return count;
}

int ahci_port_count(void) { return num_ports; }

bool ahci_init(void) {
    const struct pci_device *pci = pci_find_device(PCI_CLASS_MASS_STORAGE,
                                                   PCI_SUBCLASS_AHCI);
    if (!pci) {
        kprintf("[AHCI] No AHCI controller found\n");
        return false;
    }

    kprintf("[AHCI] Found at PCI %u:%u.%u\n", pci->bus, pci->dev, pci->func);

    /* Enable bus mastering + memory space access */
    uint32_t cmd = pci_config_read32(pci->bus, pci->dev, pci->func, 0x04);
    cmd |= (1u << 1) | (1u << 2);
    pci_config_write32(pci->bus, pci->dev, pci->func, 0x04, cmd);

    if (!pci->mmio[5]) {
        kprintf("[AHCI] BAR5 is not MMIO\n");
        return false;
    }

    hba_phys = pci->bar[5] & 0xFFFFFFF0;

    /* Map ABAR as uncacheable MMIO */
    uint64_t hba_va = 0xFFFFFFFFA0000000ULL;
    uint64_t pages = (0x10000 + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        pmap_map(kernel_pml4, hba_va + i * PAGE_SIZE,
                 hba_phys + i * PAGE_SIZE,
                 PTE_WRITE | PTE_NOCACHE | PTE_PRESENT);
    }
    hba = (volatile uint32_t *)hba_va;

    kprintf("[AHCI] ABAR mapped: phys=0x%lx size=%u\n", hba_phys, 0x10000);

    /* HBA reset */
    uint32_t ghc = hba_read(AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    hba_write(AHCI_GHC, ghc);

    ghc = hba_read(AHCI_GHC);
    if (!(ghc & AHCI_GHC_AE)) {
        ghc |= AHCI_GHC_HR;
        hba_write(AHCI_GHC, ghc);
        for (int i = 0; i < 100000; i++) {
            ghc = hba_read(AHCI_GHC);
            if (!(ghc & AHCI_GHC_HR)) break;
        }
        ghc = hba_read(AHCI_GHC);
        ghc |= AHCI_GHC_AE;
        hba_write(AHCI_GHC, ghc);
    }

    uint32_t ver = hba_read(AHCI_VS);
    kprintf("[AHCI] Version %u.%u.%u\n",
            (ver >> 20) & 0xFFF, (ver >> 16) & 0xF, ver & 0xF);

    /* Disable global interrupts — we poll */
    hba_write(AHCI_IS, ~0u);

    uint32_t pi = hba_read(AHCI_PI);
    kprintf("[AHCI] PI=0x%08x\n", pi);
    num_ports = 0;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t sig = port_read(p, AHCI_PORT_SIG);
        uint32_t ssts = port_read(p, AHCI_PORT_SSTS);
        uint8_t det = ssts & 0x0F;

        kprintf("[AHCI] Port %d: sig=0x%08x SSTS=0x%08x (det=%d)\n",
                p, sig, ssts, det);

        if (det != 3) {
            ports[p].active = false;
            continue;
        }

        if (!port_init(p)) {
            kprintf("[AHCI] Port %d: init failed\n", p);
            continue;
        }

        kprintf("[AHCI] Port %d initialized\n", p);
        num_ports = p + 1;
    }

    if (num_ports == 0) {
        kprintf("[AHCI] No SATA ports with devices\n");
        return false;
    }

    for (int p = 0; p < num_ports; p++) {
        if (!ports[p].active) continue;
        char name[8];
        name[0] = 's';
        name[1] = 'd';
        name[2] = 'a' + p;
        name[3] = '\0';
        blockdev_register(name, p, 2097152, ahci_read_sectors, NULL);
    }

    return true;
}
