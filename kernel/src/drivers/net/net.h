#ifndef MYUNIX_NET_H
#define MYUNIX_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_POLL_BUDGET 4000000u
#define NET_PING_OK 0
#define NET_PING_UNAVAILABLE -1
#define NET_PING_ADDRESS -2
#define NET_PING_ARP_TIMEOUT -3
#define NET_PING_TX_ERROR -4
#define NET_PING_TIMEOUT -5
#define NET_PING_BUSY -6

int net_init(void);
bool net_ready(void);
uint32_t net_get_ip(void);
uint32_t net_get_gw(void);
uint32_t net_get_mask(void);
void net_get_mac(uint8_t mac_out[6]);
int net_ping(uint32_t ip, uint32_t poll_budget);
bool net_arp_resolve(uint32_t ip, uint8_t mac_out[6]);
void net_poll(void);
void net_status(char *buf, size_t cap);

#endif
