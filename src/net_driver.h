#ifndef NET_DRIVER_H
#define NET_DRIVER_H
#include <stdint.h>
#define NET_MTU 1500u
#define NET_FRAME_MAX 1518u
#define NET_RX_RING 8192u
#define NET_RX_MEMORY (NET_RX_RING+16u+NET_FRAME_MAX)
#define NET_TX_MEMORY (4u*1536u)
int net_driver_init(uint8_t *rx, uint8_t *tx, uint8_t mac[6]);
int net_driver_link(void);
/* recv: length, 0 when empty, -1 after discarding/recovering a bad frame. */
int net_driver_recv(uint8_t *frame, unsigned capacity);
/* send: 1 sent, 0 all descriptors busy, -1 invalid / hardware error. */
int net_driver_send(const uint8_t *frame, unsigned length);
#endif
