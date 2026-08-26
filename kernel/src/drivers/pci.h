/*
 * PCI bus enumeration.
 * Scans for devices and stores info for later driver attachment.
 */
#ifndef MYUNIX_PCI_H
#define MYUNIX_PCI_H

#include <stdbool.h>
#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_NO_DEVICE 0xFFFF

/* PCI class codes */
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_AHCI      0x06

struct pci_device {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class, subclass, prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];       /* BAR0..BAR5 (io or mmio base, unprocessed) */
    bool     mmio[6];      /* true if BAR is memory-mapped (bit 0 = 0) */
    uint32_t bar_size[6];  /* decoded size (may be 0 if unprobed) */
    bool     present;
};

#define PCI_MAX_DEVICES 32

void pci_init(void);
const struct pci_device *pci_find_device(uint8_t class, uint8_t subclass);
const struct pci_device *pci_get_device(int index);

/* Config space access (for driver initialization). */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

#endif /* MYUNIX_PCI_H */
