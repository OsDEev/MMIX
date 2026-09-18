#ifndef MYUNIX_E1000_H
#define MYUNIX_E1000_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E1000_VID 0x8086
#define E1000_DID_82540EM 0x100E

int  e1000_init(void);
bool e1000_present(void);
void e1000_get_mac(uint8_t mac_out[6]);
/* Send one Ethernet frame; returns 0 on success, -1 on error. */
int  e1000_send(const void *frame, size_t len);
/* Poll RX ring; returns frame length, 0 if nothing available.
 * Frame is copied into buf (cap bytes). */
int  e1000_poll(void *buf, size_t cap);

#endif /* MYUNIX_E1000_H */
