/*
 * AHCI (Advanced Host Controller Interface) driver.
 * Supports SATA device read via port MMIO.
 */
#ifndef MYUNIX_AHCI_H
#define MYUNIX_AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AHCI generic host control registers (ABAR + offset) */
#define AHCI_GHC      0x004  /* Global HBA Control */
#define AHCI_IS       0x008  /* Interrupt Status */
#define AHCI_PI       0x00C  /* Ports Implemented */
#define AHCI_CAP      0x000  /* HBA Capabilities */
#define AHCI_CAP2     0x024  /* HBA Capabilities Extended */
#define AHCI_VS       0x010  /* Version */

/* Port registers (per-port, base = ABAR + 0x100 + port*0x80) */
#define AHCI_PORT_CLB   0x00  /* Command List Base Address (low 32) */
#define AHCI_PORT_CLBU  0x04  /* Command List Base Address (high 32) */
#define AHCI_PORT_FB    0x08  /* FIS Base Address (low 32) */
#define AHCI_PORT_FBU   0x0C  /* FIS Base Address (high 32) */
#define AHCI_PORT_IS    0x10  /* Interrupt Status */
#define AHCI_PORT_IE    0x14  /* Interrupt Enable */
#define AHCI_PORT_CMD   0x18  /* Command and Status */
#define AHCI_PORT_TFD   0x20  /* Task File Data */
#define AHCI_PORT_SIG   0x24  /* Signature */
#define AHCI_PORT_SSTS  0x28  /* SATA Status */
#define AHCI_PORT_SCTL  0x2C  /* SATA Control */
#define AHCI_PORT_SERR  0x30  /* SATA Error */
#define AHCI_PORT_SACT  0x34  /* SATA Active */
#define AHCI_PORT_CI    0x38  /* Command Issue */
#define AHCI_PORT_TFD_BSY 0x80
#define AHCI_PORT_TFD_DRQ 0x08
#define AHCI_PORT_SCTL_DET_MASK 0x0000000F
#define AHCI_PORT_SCTL_DET_SPINUP 1
#define AHCI_PORT_SSTS_DET_MASK  0x0000000F

#define AHCI_CMD_ST    (1u << 0)   /* Start */
#define AHCI_CMD_FRE   (1u << 4)   /* FIS Receive Enable */
#define AHCI_CMD_FR    (1u << 14)  /* FIS Receive Running */
#define AHCI_CMD_CR    (1u << 15)  /* Command List Running */

#define AHCI_GHC_AE    (1u << 31)  /* AHCI Enable */
#define AHCI_GHC_HR    (1u << 0)   /* HBA Reset */

#define AHCI_PORT_IS_TFES (1u << 30) /* Task File Error Status */

/* HBA Port Signature */
#define AHCI_SIG_ATA    0x00000101
#define AHCI_SIG_ATAPI  0xEB140101

/* Command FIS (host -> device, 20 bytes) */
struct hba_cmdfis {
    uint8_t  fis_type;   /* 0x27 = H2D */
    uint8_t  pmport;
    uint8_t  command;
    uint8_t  feature;
    uint8_t  lba_low;
    uint8_t  lba_mid;
    uint8_t  lba_high;
    uint8_t  device;
    uint8_t  lba_low_exp;
    uint8_t  lba_mid_exp;
    uint8_t  lba_high_exp;
    uint8_t  feature_exp;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved[4];
} __attribute__((packed));

/* Command header (in command list, 32 bytes each) */
struct hba_cmdh {
    uint16_t opts;        /* CFL, W, P, R, B, C, R (reserved), PMP, DT */
    uint16_t prdtl;       /* Physical Region Descriptor Table Length */
    uint32_t prdbc;       /* Physical Region Descriptor Byte Count */
    uint64_t ctba;        /* Command Table Base Address */
    uint32_t reserved[4];
} __attribute__((packed));

/* Physical Region Descriptor Table entry (16 bytes) */
struct hba_prdt_entry {
    uint64_t dba;        /* Data Base Address */
    uint32_t reserved;
    uint32_t info;       /* byte count (bits 0..30), interrupt on complete (bit 31) */
} __attribute__((packed));

/* Command table (1 command, + PRDT entries) */
struct hba_cmd_table {
    uint8_t  cfis[64];             /* Command FIS (padded to 64 bytes per AHCI spec) */
    uint8_t  acmd[16];             /* ATAPI Command (16 bytes) */
    uint8_t  reserved[48];         /* Reserved (48 bytes) */
    struct hba_prdt_entry prdt[];  /* PRDT starts at offset 128 */
} __attribute__((packed));

/* Port init info */
struct ahci_port {
    volatile void *cl;           /* Command list (256 bytes, aligned) */
    volatile void *fis;          /* FIS receive area (256 bytes, aligned) */
    struct hba_cmd_table *cmd_table;
    uint64_t cl_phys, fis_phys, ct_phys;
    int      slot;               /* last used command slot */
    bool     active;
};

#define AHCI_MAX_PORTS 32

bool ahci_init(void);
int  ahci_read_sectors(int port, uint64_t lba, uint32_t count, void *buf);
int  ahci_port_count(void);

#endif /* MYUNIX_AHCI_H */
