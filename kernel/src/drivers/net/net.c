#include <chardev.h>
#include <libk.h>
#include <sched.h>
#include <string.h>
#include <vfs.h>

#include "net/e1000.h"
#include "net/net.h"

#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ARP_HW_ETHER 1u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY 2u

#define IPV4_VER_IHL 0x40u
#define IPV4_PROTO_ICMP 1u

#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO 8u

#define ETH_MIN 14u
#define ARP_PLAIN 28u
#define ARP_PAD_TO 60u
#define IPV4_MIN 20u
#define ICMP_MIN 8u

#define PING_PAYLOAD 24u
#define PING_ID 0x4D58u
#define PING_SEQ_START 1u
#define PING_TX_RETRY_MAX 3u

#define NET_MAX_FRAME 1522u
#define NET_RX_BUF 1600u

#define ARP_WAIT_LOOPS 30000000u
#define PING_WAIT_LOOPS 30000000u
#define PING_RETX_LOOPS 3000000u
#define POLL_SPIN_BURST 256u
#define NET_TX_RING 32u
#define NET_RX_EXTRA_LIMIT 64u

#define CMD_NONE 0u
#define CMD_PING 1u
#define CMD_STATUS 2u

#define RES_NONE 0u
#define RES_PENDING 1u
#define RES_READY 2u

#define RES_STATUS_OK 0u
#define RES_STATUS_NETDOWN 1u
#define RES_STATUS_BADADDR 2u
#define RES_STATUS_NOTOWNER 3u
#define RES_STATUS_ARPTIMEOUT 4u
#define RES_STATUS_TXFAIL 5u
#define RES_STATUS_TIMEOUT 6u

static struct {
    bool inited;
    uint32_t ip;
    uint32_t gw;
    uint32_t mask;
    uint32_t open_owner;
    uint16_t seq_next;
    uint8_t cmd;
    uint32_t cmd_seq;
    uint8_t result_state;
    uint8_t result_status;
    uint32_t result_addr;
    uint32_t result_seq;
    uint32_t result_off;
    char result_buf[192];
} net;

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
    uint8_t pad[ARP_PAD_TO - ARP_PLAIN - 2 * 6 - 2];
} __attribute__((packed));

struct ipv4_frame {
    uint8_t dmac[6];
    uint8_t smac[6];
    uint16_t ethertype_be;
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len_be;
    uint16_t id_be;
    uint16_t flags_frag_be;
    uint8_t ttl;
    uint8_t proto;
    uint16_t hdr_csum_be;
    uint8_t src[4];
    uint8_t dst[4];
    uint8_t icmp[ICMP_MIN + PING_PAYLOAD];
} __attribute__((packed));

static uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint16_t ethertype_of(const uint8_t *f) {
    return (uint16_t)((f[12] << 8) | f[13]);
}

static void octets_from_ip(uint32_t ip, uint8_t *out) {
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

static uint16_t ip_checksum(const uint8_t *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1u) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
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

static bool ip_eq(uint32_t a, const uint8_t *b) {
    return ((a >> 24) & 0xFFu) == b[0] && ((a >> 16) & 0xFFu) == b[1] &&
           ((a >> 8) & 0xFFu) == b[2] && (a & 0xFFu) == b[3];
}

static void arp_build_request(uint8_t *f, uint16_t *len, uint32_t target_ip) {
    uint8_t my[6];
    struct arp_frame *a = (struct arp_frame *)f;
    e1000_get_mac(my);
    memset(f, 0, ARP_PAD_TO);
    memset(a->dmac, 0xFF, 6);
    memcpy(a->smac, my, 6);
    a->ethertype_be = be16(ETHERTYPE_ARP);
    a->htype_be = be16(ARP_HW_ETHER);
    a->ptype_be = be16(ETHERTYPE_IPV4);
    a->hlen = 6;
    a->plen = 4;
    a->oper_be = be16(ARP_OP_REQUEST);
    memcpy(a->sha, my, 6);
    octets_from_ip(net.ip, a->spa);
    memset(a->tha, 0, 6);
    octets_from_ip(target_ip, a->tpa);
    *len = ARP_PAD_TO;
}

static bool arp_field_ok(const uint8_t *f, uint16_t len) {
    if (len < ETH_MIN + ARP_PLAIN) return false;
    if (ethertype_of(f) != ETHERTYPE_ARP) return false;
    uint16_t htype = (uint16_t)((f[14] << 8) | f[15]);
    uint16_t ptype = (uint16_t)((f[16] << 8) | f[17]);
    uint16_t oper = (uint16_t)((f[20] << 8) | f[21]);
    if (htype != ARP_HW_ETHER || ptype != ETHERTYPE_IPV4) return false;
    if (f[18] != 6 || f[19] != 4) return false;
    return oper == ARP_OP_REPLY;
}

static bool arp_reply_ok(const uint8_t *f, uint16_t len, uint32_t query_ip) {
    uint8_t my_mac[6];
    if (!arp_field_ok(f, len)) return false;
    if (!mac_valid(&f[22]) || (f[22] & 1u)) return false;
    if (memcmp(&f[6], &f[22], 6) != 0) return false;
    e1000_get_mac(my_mac);
    if (memcmp(f, my_mac, 6) != 0) return false;
    if (!ip_eq(query_ip, &f[28])) return false;
    if (memcmp(&f[32], my_mac, 6) != 0) return false;
    return ip_eq(net.ip, &f[38]);
}

static int arp_exchange(uint32_t target_ip, uint8_t mac_out[6]) {
    uint8_t f[NET_MAX_FRAME];
    uint8_t r[NET_RX_BUF];
    uint16_t len = 0;
    uint32_t spins = 0;
    uint32_t extra = 0;
    int rc = -2;

    for (uint32_t attempt = 0; attempt < PING_TX_RETRY_MAX; attempt++) {
        arp_build_request(f, &len, target_ip);
        if (e1000_send(f, len) != 0) {
            rc = -1;
            continue;
        }
        spins = 0;
        extra = 0;
        while (spins < ARP_WAIT_LOOPS) {
            int n = e1000_poll(r, sizeof(r));
            if (n > 0) {
                if (arp_reply_ok(r, (uint16_t)n, target_ip)) {
                    memcpy(mac_out, &r[22], 6);
                    return 0;
                }
                if (++extra > NET_RX_EXTRA_LIMIT) break;
                continue;
            }
            if (n < 0) return -1;
            spins += POLL_SPIN_BURST;
        }
    }
    return rc;
}

static uint16_t icmp_checksum(const uint8_t *icmp, size_t len) {
    return ip_checksum(icmp, len);
}

static void icmp_build_echo(uint8_t *icmp, size_t *len, uint16_t seq) {
    memset(icmp, 0, ICMP_MIN + PING_PAYLOAD);
    icmp[0] = (uint8_t)ICMP_ECHO;
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    icmp[4] = (uint8_t)(PING_ID >> 8);
    icmp[5] = (uint8_t)(PING_ID & 0xFFu);
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)(seq & 0xFFu);
    for (uint32_t i = 0; i < PING_PAYLOAD; i++)
        icmp[ICMP_MIN + i] = (uint8_t)('A' + (i % 26));
    uint16_t csum = icmp_checksum(icmp, ICMP_MIN + PING_PAYLOAD);
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)(csum & 0xFFu);
    *len = ICMP_MIN + PING_PAYLOAD;
}

static void ipv4_build_echo_frame(struct ipv4_frame *p, const uint8_t dmac[6],
                                  uint32_t dst_ip, uint16_t seq) {
    uint8_t my_mac[6];
    size_t icmp_len = 0;
    size_t total = IPV4_MIN + ICMP_MIN + PING_PAYLOAD;

    e1000_get_mac(my_mac);
    memset(p, 0, sizeof(*p));
    memcpy(p->dmac, dmac, 6);
    memcpy(p->smac, my_mac, 6);
    p->ethertype_be = be16(ETHERTYPE_IPV4);
    p->ver_ihl = IPV4_VER_IHL | (IPV4_MIN / 4);
    p->tos = 0;
    p->total_len_be = be16((uint16_t)total);
    p->id_be = be16(seq);
    p->flags_frag_be = be16(0x4000u);
    p->ttl = 64;
    p->proto = IPV4_PROTO_ICMP;
    octets_from_ip(net.ip, p->src);
    octets_from_ip(dst_ip, p->dst);
    icmp_build_echo(p->icmp, &icmp_len, seq);
    p->hdr_csum_be = 0;
    p->hdr_csum_be = be16(ip_checksum((const uint8_t *)&p->ver_ihl, IPV4_MIN));
}

static bool ipv4_reply_ok(const uint8_t *f, uint16_t flen, uint32_t peer_ip,
                          uint16_t seq, const uint8_t *payload,
                          uint32_t payload_len) {
    uint8_t my_mac[6];
    uint16_t total;
    uint16_t flags_frag;
    uint16_t csum_verify;

    if (flen < ETH_MIN + IPV4_MIN + ICMP_MIN) return false;
    if (ethertype_of(f) != ETHERTYPE_IPV4) return false;

    const uint8_t *ip = f + ETH_MIN;
    if (ip[0] != (IPV4_VER_IHL | (IPV4_MIN / 4))) return false;
    if (ip[9] != IPV4_PROTO_ICMP) return false;

    total = (uint16_t)((ip[2] << 8) | ip[3]);
    if (total < IPV4_MIN + ICMP_MIN) return false;
    if ((size_t)total > (size_t)flen - ETH_MIN) return false;
    if ((size_t)total != ICMP_MIN + PING_PAYLOAD + IPV4_MIN) return false;

    flags_frag = (uint16_t)((ip[6] << 8) | ip[7]);
    if ((flags_frag & 0x1FFFu) != 0) return false;
    if ((flags_frag & 0x2000u) != 0) return false;

    csum_verify = ip_checksum(ip, IPV4_MIN);
    if (csum_verify != 0) return false;

    if (!ip_eq(peer_ip, ip + 12)) return false;
    if (!ip_eq(net.ip, ip + 16)) return false;

    const uint8_t *icmp = ip + IPV4_MIN;
    uint16_t icmp_len = (uint16_t)(total - IPV4_MIN);
    if (icmp_len != ICMP_MIN + PING_PAYLOAD) return false;
    if (icmp[0] != ICMP_ECHO_REPLY || icmp[1] != 0) return false;
    if (icmp[4] != (uint8_t)(PING_ID >> 8) ||
        icmp[5] != (uint8_t)(PING_ID & 0xFFu))
        return false;
    if ((uint16_t)((icmp[6] << 8) | icmp[7]) != seq) return false;

    uint16_t ic = ip_checksum(icmp, icmp_len);
    if (ic != 0) return false;

    if (payload != NULL && payload_len > 0) {
        if (payload_len != PING_PAYLOAD) return false;
        for (uint32_t i = 0; i < PING_PAYLOAD; i++) {
            if (icmp[ICMP_MIN + i] != (uint8_t)('A' + (i % 26))) return false;
        }
    }

    e1000_get_mac(my_mac);
    if (memcmp(f, my_mac, 6) != 0) return false;
    if (!mac_valid(&f[6])) return false;
    return true;
}

static bool cmd_ping_run(uint32_t ip, uint8_t res_status[1],
                         uint32_t *out_seq) {
    uint8_t dmac[6];
    uint8_t f[NET_MAX_FRAME];
    uint8_t r[NET_RX_BUF];
    uint16_t seq;
    uint16_t len;
    uint32_t spins;
    uint32_t extra;

    if (!e1000_present()) {
        res_status[0] = RES_STATUS_NETDOWN;
        return false;
    }
    if (arp_exchange(ip, dmac) != 0) {
        res_status[0] = RES_STATUS_ARPTIMEOUT;
        return false;
    }

    seq = net.seq_next;
    if (seq == 0) seq = PING_SEQ_START;
    net.seq_next = (uint16_t)(seq + 1);

    for (uint32_t attempt = 0; attempt < PING_TX_RETRY_MAX; attempt++) {
        struct ipv4_frame *tx = (struct ipv4_frame *)f;
        ipv4_build_echo_frame(tx, dmac, ip, seq);
        len = (uint16_t)(ETH_MIN + IPV4_MIN + ICMP_MIN + PING_PAYLOAD);
        if (e1000_send(f, len) != 0) {
            res_status[0] = RES_STATUS_TXFAIL;
            return false;
        }
        spins = 0;
        extra = 0;
        while (spins < PING_WAIT_LOOPS) {
            int n = e1000_poll(r, sizeof(r));
            if (n > 0) {
                if (ipv4_reply_ok(r, (uint16_t)n, ip, seq, NULL, 0)) {
                    res_status[0] = RES_STATUS_OK;
                    *out_seq = seq;
                    return true;
                }
                if (++extra > NET_RX_EXTRA_LIMIT) break;
                continue;
            }
            if (n < 0) {
                res_status[0] = RES_STATUS_TXFAIL;
                return false;
            }
            spins += POLL_SPIN_BURST;
        }
    }
    res_status[0] = RES_STATUS_TIMEOUT;
    return false;
}

static void cmd_status_render(char *out, size_t cap) {
    uint8_t mac[6];
    e1000_get_mac(mac);
    snprintf(out, cap,
             "net: %s\nip %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n"
              "mac %x:%x:%x:%x:%x:%x\n"
             "link %s static (no DHCP)\n",
             net.inited ? "up" : "down",
             (unsigned)((net.ip >> 24) & 0xFFu), (unsigned)((net.ip >> 16) & 0xFFu),
             (unsigned)((net.ip >> 8) & 0xFFu), (unsigned)(net.ip & 0xFFu),
             (unsigned)((net.mask >> 24) & 0xFFu), (unsigned)((net.mask >> 16) & 0xFFu),
             (unsigned)((net.mask >> 8) & 0xFFu), (unsigned)(net.mask & 0xFFu),
             (unsigned)((net.gw >> 24) & 0xFFu), (unsigned)((net.gw >> 16) & 0xFFu),
             (unsigned)((net.gw >> 8) & 0xFFu), (unsigned)(net.gw & 0xFFu),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             e1000_present() ? "present" : "absent");
}

static bool parse_ipv4_strict(const char *s, size_t n, uint32_t *out) {
    uint32_t octets[4];
    uint32_t val = 0;
    size_t digits = 0;
    int idx = 0;
    size_t i = 0;

    if (n == 0 || n > 15) return false;
    for (;;) {
        if (i < n && s[i] >= '0' && s[i] <= '9') {
            val = val * 10u + (uint32_t)(s[i] - '0');
            digits++;
            if (digits > 3) return false;
            if (val > 255u) return false;
            i++;
            if (i < n) continue;
            if (idx != 3) return false;
            octets[idx++] = val;
            break;
        }
        if (s[i] == '.' && idx < 3 && digits > 0) {
            octets[idx++] = val;
            val = 0;
            digits = 0;
            i++;
            if (i >= n) return false;
            continue;
        }
        return false;
    }
    if (idx != 4) return false;
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

static int netdev_write(const char *buf, size_t n) {
    task_t *cur = sched_get_current();

    if (buf == NULL || n == 0) return -1;
    if (!net.inited || !e1000_present()) return -1;

    if (net.cmd != CMD_NONE && net.result_state == RES_PENDING) {
        if (cur == NULL || net.open_owner != (uint32_t)cur->pid) return -1;
        return 0;
    }

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;

    if (n == 6 && memcmp(buf, "status", 6) == 0) {
        if (cur == NULL) return -1;
        if (net.result_state == RES_PENDING && net.open_owner != (uint32_t)cur->pid)
            return -1;
        net.open_owner = cur->pid;
        net.cmd = CMD_STATUS;
        net.cmd_seq++;
        net.result_state = RES_PENDING;
        net.result_status = RES_STATUS_OK;
        return (int)n;
    }
    if (n > 5 && memcmp(buf, "ping ", 5) == 0) {
        uint32_t ip = 0;
        if (!parse_ipv4_strict(buf + 5, n - 5, &ip)) return -1;
        if (cur == NULL) return -1;
        if (net.result_state == RES_PENDING && net.open_owner != (uint32_t)cur->pid)
            return -1;
        net.open_owner = cur->pid;
        net.cmd = CMD_PING;
        net.cmd_seq++;
        net.result_state = RES_PENDING;
        net.result_addr = ip;
        return (int)n;
    }
    return -1;
}

static int netdev_read(char *buf, size_t n) {
    task_t *cur = sched_get_current();
    char line[192];
    size_t len;
    uint8_t st[1];
    uint32_t seq = 0;

    if (buf == NULL || n == 0) return -1;
    if (!net.inited) return -1;
    if (cur == NULL || net.open_owner != (uint32_t)cur->pid) return -1;

    if (net.result_state == RES_READY) {
        len = strlen(net.result_buf);
        if (net.result_off >= len) {
            net.result_state = RES_NONE;
            net.cmd = CMD_NONE;
            net.result_off = 0;
            net.result_buf[0] = 0;
            return 0;
        }
        if (n > len - net.result_off) n = len - net.result_off;
        memcpy(buf, net.result_buf + net.result_off, n);
        net.result_off += n;
        return (int)n;
    }
    if (net.result_state != RES_PENDING || net.cmd == CMD_NONE) return 0;

    if (net.cmd == CMD_STATUS) {
        cmd_status_render(line, sizeof(line));
        net.cmd = CMD_NONE;
        if (sizeof(net.result_buf) < strlen(line) + 1) return -1;
        memcpy(net.result_buf, line, strlen(line) + 1);
        net.result_status = RES_STATUS_OK;
        net.result_state = RES_READY;
        net.result_off = 0;
        len = strlen(net.result_buf);
        if (n > len) n = len;
        memcpy(buf, net.result_buf, n);
        net.result_off = n;
        return (int)n;
    }

    if (cmd_ping_run(net.result_addr, st, &seq)) {
        snprintf(line, sizeof(line), "reply from %u.%u.%u.%u seq=%u\n",
                 (unsigned)((net.result_addr >> 24) & 0xFFu),
                 (unsigned)((net.result_addr >> 16) & 0xFFu),
                 (unsigned)((net.result_addr >> 8) & 0xFFu),
                 (unsigned)(net.result_addr & 0xFFu),
                 (unsigned)seq);
        net.result_status = RES_STATUS_OK;
    } else {
        switch (st[0]) {
            case RES_STATUS_NETDOWN:
                snprintf(line, sizeof(line), "net down\n");
                break;
            case RES_STATUS_BADADDR:
                snprintf(line, sizeof(line), "bad address\n");
                break;
            case RES_STATUS_ARPTIMEOUT:
                snprintf(line, sizeof(line), "no arp reply from %u.%u.%u.%u\n",
                         (unsigned)((net.result_addr >> 24) & 0xFFu),
                         (unsigned)((net.result_addr >> 16) & 0xFFu),
                         (unsigned)((net.result_addr >> 8) & 0xFFu),
                         (unsigned)(net.result_addr & 0xFFu));
                break;
            case RES_STATUS_TXFAIL:
                snprintf(line, sizeof(line), "tx error\n");
                break;
            default:
                snprintf(line, sizeof(line), "timeout %u.%u.%u.%u\n",
                         (unsigned)((net.result_addr >> 24) & 0xFFu),
                         (unsigned)((net.result_addr >> 16) & 0xFFu),
                         (unsigned)((net.result_addr >> 8) & 0xFFu),
                         (unsigned)(net.result_addr & 0xFFu));
                break;
        }
    }
    net.cmd = CMD_NONE;
    if (sizeof(net.result_buf) < strlen(line) + 1) return -1;
    memcpy(net.result_buf, line, strlen(line) + 1);
    net.result_state = RES_READY;
    net.result_off = 0;
    len = strlen(net.result_buf);
    if (n > len) n = len;
    memcpy(buf, net.result_buf, n);
    net.result_off = n;
    return (int)n;
}

static int netdev_read_w(char *buf, size_t n) {
    return netdev_read(buf, n);
}

static struct chardev net_chardev = {
    "net", netdev_read_w, netdev_write
};

int net_init(void) {
    memset(&net, 0, sizeof(net));
    net.ip = 0x0A00020Fu;
    net.mask = 0xFFFFFF00u;
    net.gw = 0x0A000202u;
    net.cmd = CMD_NONE;
    net.result_state = RES_NONE;
    net.seq_next = PING_SEQ_START;
    net.open_owner = 0;
    net.result_buf[0] = 0;
    if (e1000_present()) {
        net.inited = true;
        vfs_mount_dev("net", &net_chardev);
        kprintf("[NET] /dev/net mounted ip=10.0.2.15/24 gw=10.0.2.2\n");
        return 0;
    }
    kprintf("[NET] NIC absent; /dev/net not mounted\n");
    return -1;
}

bool net_ready(void) {
    return net.inited && e1000_present();
}

uint32_t net_get_ip(void) {
    return net.ip;
}

uint32_t net_get_gw(void) {
    return net.gw;
}

uint32_t net_get_mask(void) {
    return net.mask;
}

void net_get_mac(uint8_t mac_out[6]) {
    e1000_get_mac(mac_out);
}

int net_ping(uint32_t ip, uint32_t poll_budget) {
    uint8_t st[1];
    uint32_t seq = 0;
    (void)poll_budget;
    if (!net_ready()) return NET_PING_UNAVAILABLE;
    if (!cmd_ping_run(ip, st, &seq)) {
        switch (st[0]) {
            case RES_STATUS_NETDOWN: return NET_PING_UNAVAILABLE;
            case RES_STATUS_ARPTIMEOUT: return NET_PING_ARP_TIMEOUT;
            case RES_STATUS_TXFAIL: return NET_PING_TX_ERROR;
            default: return NET_PING_TIMEOUT;
        }
    }
    return NET_PING_OK;
}

bool net_arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    if (!net_ready()) return false;
    return arp_exchange(ip, mac_out) == 0;
}

void net_poll(void) {
    uint8_t r[NET_RX_BUF];
    if (!net_ready()) return;
    for (uint32_t i = 0; i < 8u; i++) {
        int n = e1000_poll(r, sizeof(r));
        if (n <= 0) break;
    }
}

void net_status(char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    cmd_status_render(buf, cap);
}
