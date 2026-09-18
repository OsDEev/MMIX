#include <io.h>
#include <libk.h>
#include <string.h>

#include "net/e1000.h"
#include "e1000_test.h"

#define ETHERTYPE_ARP 0x0806
#define ARP_HW_ETHER 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ARP_REPLY_WAIT_LOOPS 40000000u
#define ARP_INTER_TX_LOOPS 200000u
#define ARP_POLL_INTERVAL 200u
#define ARP_MAX_FRAME 1518u
#define ARP_MIN_FRAME 60u
#define E1000_TEST_EXCHANGES 96u

struct arp_frame {
    uint8_t dmac[6];
    uint8_t smac[6];
    uint16_t ethertype_be;
    uint16_t htype_be;
    uint16_t ptype_be;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper_be;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
    uint8_t pad[ARP_MIN_FRAME - 42];
} __attribute__((packed));

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint8_t my_mac[6];
static uint16_t my_ip_l;
static uint16_t my_ip_h;
static uint16_t gw_ip_l;
static uint16_t gw_ip_h;
static uint16_t arp_tx_id;
static uint16_t arp_rx_id;
static uint32_t arp_ok_count;
static uint32_t arp_fail_count;

static void ip_to_be16(uint32_t ip, uint16_t *hi, uint16_t *lo) {
    *hi = (uint16_t)((ip >> 16) & 0xFFFFu);
    *lo = (uint16_t)(ip & 0xFFFFu);
}

static bool ip_eq(uint32_t a, const uint8_t *b) {
    return ((a >> 24) & 0xFFu) == b[0] && ((a >> 16) & 0xFFu) == b[1] &&
           ((a >> 8) & 0xFFu) == b[2] && (a & 0xFFu) == b[3];
}

static bool mac_valid(const uint8_t *m) {
    bool zero = true;
    bool ff = true;
    for (int i = 0; i < 6; i++) {
        if (m[i] != 0) zero = false;
        if (m[i] != 0xFF) ff = false;
    }
    return !zero && !ff;
}

static bool ethertype_is(const uint8_t *f, uint16_t et) {
    uint16_t v = (uint16_t)((f[12] << 8) | f[13]);
    return v == et;
}

static bool arp_field_ok(const uint8_t *f, uint16_t len) {
    if (len < 42) return false;
    if (!ethertype_is(f, ETHERTYPE_ARP)) return false;
    uint16_t htype = (uint16_t)((f[14] << 8) | f[15]);
    uint16_t ptype = (uint16_t)((f[16] << 8) | f[17]);
    uint16_t oper = (uint16_t)((f[20] << 8) | f[21]);
    return htype == ARP_HW_ETHER && ptype == 0x0800 && f[18] == 6 &&
           f[19] == 4 && oper == ARP_OP_REPLY;
}

static bool arp_payload_ok(const uint8_t *f) {
    if (memcmp(f, my_mac, 6) != 0) return false;
    if (!mac_valid(&f[22]) || (f[22] & 1)) return false;
    if (memcmp(&f[6], &f[22], 6) != 0) return false;
    if (!ip_eq(0x0A000202u, &f[28])) return false;
    if (memcmp(&f[32], my_mac, 6) != 0) return false;
    return ip_eq(0x0A00020Fu, &f[38]);
}

static void arp_build_request(uint8_t *f, uint16_t *len) {
    struct arp_frame *a = (struct arp_frame *)f;
    memset(f, 0, ARP_MIN_FRAME);
    memset(a->dmac, 0xFF, 6);
    memcpy(a->smac, my_mac, 6);
    a->ethertype_be = bswap16(ETHERTYPE_ARP);
    a->htype_be = bswap16(ARP_HW_ETHER);
    a->ptype_be = bswap16(0x0800);
    a->hlen = 6;
    a->plen = 4;
    a->oper_be = bswap16(ARP_OP_REQUEST);
    memcpy(a->sha, my_mac, 6);
    a->spa[0] = 10; a->spa[1] = 0; a->spa[2] = 2; a->spa[3] = 15;
    memset(a->tha, 0, 6);
    a->tpa[0] = 10; a->tpa[1] = 0; a->tpa[2] = 2; a->tpa[3] = 2;
    *len = ARP_MIN_FRAME;
}

static bool arp_collect_reply(void) {
    uint8_t r[ARP_MAX_FRAME];
    uint32_t spins = 0;
    while (spins < ARP_REPLY_WAIT_LOOPS) {
        int n = e1000_poll(r, sizeof(r));
        if (n > 0) {
            if (arp_field_ok(r, (uint16_t)n) && arp_payload_ok(r)) return true;
            continue;
        }
        if (n < 0) return false;
        if (++spins % ARP_POLL_INTERVAL == 0) io_wait();
    }
    return false;
}

static void arp_send_one(void) {
    uint8_t f[ARP_MAX_FRAME];
    uint16_t len = 0;
    arp_build_request(f, &len);
    if (e1000_send(f, len) != 0) {
        arp_fail_count++;
        kprintf("[E1000TEST] send failed (tx=%u ok=%u)\n",
                (unsigned)arp_tx_id, (unsigned)arp_ok_count);
        return;
    }
    arp_tx_id++;
    if (arp_collect_reply()) {
        arp_ok_count++;
        arp_rx_id++;
    } else {
        arp_fail_count++;
        kprintf("[E1000TEST] no valid reply (tx=%u ok=%u fail=%u)\n",
                (unsigned)arp_tx_id, (unsigned)arp_ok_count,
                (unsigned)arp_fail_count);
    }
}

void e1000_test(void) {
    arp_ok_count = 0;
    arp_fail_count = 0;
    arp_tx_id = 0;
    arp_rx_id = 0;
    kprintf("[E1000TEST] start: %u ARP exchanges\n",
            (unsigned)E1000_TEST_EXCHANGES);
    if (!e1000_present()) {
        kprintf("[E1000TEST] FAIL: NIC not present\n");
        return;
    }
    e1000_get_mac(my_mac);
    kprintf("[E1000TEST] my MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    ip_to_be16(0x0A00020Fu, &my_ip_h, &my_ip_l);
    ip_to_be16(0x0A000202u, &gw_ip_h, &gw_ip_l);
    for (uint32_t i = 0; i < E1000_TEST_EXCHANGES; i++) {
        arp_send_one();
        if (arp_fail_count != 0) break;
        for (uint32_t j = 0; j < ARP_INTER_TX_LOOPS; j++) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    kprintf("[E1000TEST] done: ok=%u fail=%u tx=%u rx=%u\n",
            (unsigned)arp_ok_count, (unsigned)arp_fail_count,
            (unsigned)arp_tx_id, (unsigned)arp_rx_id);
    if (arp_ok_count == E1000_TEST_EXCHANGES && arp_fail_count == 0) {
        kprintf("[E1000TEST] PASS: all %u ARP exchanges OK\n",
                (unsigned)E1000_TEST_EXCHANGES);
    } else {
        kprintf("[E1000TEST] FAIL: %u of %u exchanges OK\n",
                (unsigned)arp_ok_count, (unsigned)E1000_TEST_EXCHANGES);
    }
}
