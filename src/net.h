#ifndef NET_H
#define NET_H
#include <stdint.h>
/* QEMU's user-mode network only: fixed 10.0.2.15/24, router .2, DNS .3.
 * One in-flight job. All calls except the three documented convenience
 * wrappers return immediately; call net_poll() from the desktop loop. */
#define NET_URL_MAX 256
#define NET_HOST_MAX 128
#define NET_HTTP_BODY_MAX 32768 /* Browser presentation buffer; keep bounded. */
#define NET_HTTP_TRANSFER_MAX 2097153 /* 2 MiB file plus convenience NUL. */

enum { NET_HTTP_IDLE, NET_HTTP_RESOLVING, NET_HTTP_CONNECTING,
       NET_HTTP_RECEIVING, NET_HTTP_DONE, NET_HTTP_ERROR };
typedef struct {
    int state, status, truncated;
    unsigned length, request_id;
    char error[80], content_type[64], location[NET_URL_MAX];
} NetHttpResult;
typedef struct {
    int available, link_up;
    uint8_t mac[6];
    uint32_t address, gateway, dns; /* host-order IPv4: 10.0.2.15 = 0x0a00020f */
    unsigned rx_packets, tx_packets, dropped_packets, rx_errors, tx_errors;
} NetStatus;

void net_init(void);
void net_poll(void); /* At most 32 Ethernet frames and bounded timer work. */
const NetStatus *net_status(void);
int net_busy(void);
const char *net_last_error(void);
int net_http_start(const char *url, char *body, unsigned capacity);
int net_http_busy(void);
const NetHttpResult *net_http_result(void);
void net_cancel(void);
/* Convenience calls keep input collection alive via platform_poll(), but do
 * not dispatch desktop actions. They are bounded by a 15-second deadline. */
int net_http_get(const char *url, char *body, unsigned capacity);
int net_resolve(const char *host, uint32_t *address);
int net_ping(const char *host, unsigned *milliseconds);
int net_parse_ipv4(const char *text, uint32_t *address);
void net_format_ipv4(uint32_t address, char out[16]);
#endif
