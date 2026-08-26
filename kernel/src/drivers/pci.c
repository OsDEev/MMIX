/*
 * PCI bus enumeration.
 * Scans bus 0..255, device 0..31, function 0..7 for valid devices.
 */
#include <io.h>
#include <libk.h>
#include <pci.h>

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count = 0;

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31)
        | ((uint32_t)bus  << 16)
        | ((uint32_t)dev  << 11)
        | ((uint32_t)func << 8)
        | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t val = pci_config_read32(bus, dev, func, off);
    return (uint16_t)((val >> ((off & 2) * 8)) & 0xFFFF);
}

static uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t val = pci_config_read32(bus, dev, func, off);
    return (uint8_t)((val >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1u << 31)
        | ((uint32_t)bus  << 16)
        | ((uint32_t)dev  << 11)
        | ((uint32_t)func << 8)
        | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* Decode BAR size: write all-1s, read back the size, restore original. */
static uint32_t pci_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int bar_index) {
    uint8_t off = 0x10 + bar_index * 4;
    uint32_t original = pci_config_read32(bus, dev, func, off);
    pci_config_write32(bus, dev, func, off, 0xFFFFFFFF);
    uint32_t size = pci_config_read32(bus, dev, func, off);
    pci_config_write32(bus, dev, func, off, original);
    if (size == 0 || size == 0xFFFFFFFF) return 0;
    if (original & 1) {
        /* I/O space BAR: size in lower 2 bits masked */
        return ~(size & 0xFFFFFFFC) + 1;
    } else {
        /* Memory BAR: size in lower 4 bits masked */
        return ~(size & 0xFFFFFFF0) + 1;
    }
}

static void pci_scan_bus(uint8_t bus);

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vid = pci_config_read16(bus, dev, func, 0x00);
    if (vid == PCI_NO_DEVICE) return;

    if (device_count >= PCI_MAX_DEVICES) return;

    struct pci_device *d = &devices[device_count];
    d->bus  = bus;
    d->dev  = dev;
    d->func = func;
    d->vendor_id = vid;
    d->device_id = pci_config_read16(bus, dev, func, 0x02);

    uint32_t class_word = pci_config_read32(bus, dev, func, 0x08);
    d->class    = (class_word >> 24) & 0xFF;
    d->subclass = (class_word >> 16) & 0xFF;
    d->prog_if = (class_word >> 8) & 0xFF;

    d->irq_line = pci_config_read8(bus, dev, func, 0x3C);

    /* Read BARs */
    for (int i = 0; i < 6; i++) {
        uint32_t bar = pci_config_read32(bus, dev, func, 0x10 + i * 4);
        d->bar[i] = bar;
        d->mmio[i] = (bar & 1) == 0;
        d->bar_size[i] = pci_bar_size(bus, dev, func, i);
    }

    d->present = true;
    device_count++;

    kprintf("[PCI] %u:%u.%u vend=0x%x dev=0x%x class=0x%x:%02x irq=%d\n",
            bus, dev, func, vid, d->device_id, d->class, d->subclass, d->irq_line);

    /* If multi-function device, scan remaining functions */
    if (func == 0) {
        uint8_t header_type = pci_config_read8(bus, dev, func, 0x0E);
        if (header_type & 0x80) {
            for (int f = 1; f < 8; f++)
                pci_scan_function(bus, dev, f);
        }
    }
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint16_t vid = pci_config_read16(bus, dev, 0, 0x00);
    if (vid == PCI_NO_DEVICE) return;
    pci_scan_function(bus, dev, 0);
}

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++)
        pci_scan_device(bus, dev);
}

void pci_init(void) {
    device_count = 0;

    /* Scan bus 0 first (most common) */
    pci_scan_bus(0);

    /* Check if host bridge is multi-bridge (scan bus 1 too) */
    pci_scan_bus(1);

    kprintf("[PCI] Found %d device(s)\n", device_count);
}

const struct pci_device *pci_find_device(uint8_t class, uint8_t subclass) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class == class && devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

const struct pci_device *pci_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}
