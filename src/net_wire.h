#ifndef NET_WIRE_H
#define NET_WIRE_H
#include "net.h"
#include <stdint.h>
uint16_t net_read16(const uint8_t *p);
uint32_t net_read32(const uint8_t *p);
void net_write16(uint8_t *p, unsigned value);
void net_write32(uint8_t *p, uint32_t value);
uint16_t net_checksum(const uint8_t *data, unsigned length);
uint16_t net_transport_checksum(uint32_t source, uint32_t dest, unsigned proto,
                               const uint8_t *data, unsigned length);
int net_parse_url(const char *url, char host[NET_HOST_MAX], unsigned *port,
                  char path[NET_URL_MAX]);
/* DNS parser validates transaction/question and bounded compressed names.
 * 1 answer, 0 irrelevant/no usable answer, -1 malformed/server failure. */
int net_dns_query(uint8_t *out, unsigned capacity, unsigned id, const char *host);
int net_dns_answer(const uint8_t *data, unsigned length, unsigned id,
                   const char *host, uint32_t *address);

typedef struct {
    char headers[4096], line[128];
    unsigned header_length, line_length, body_length, capacity;
    uint32_t remaining, chunk_remaining;
    int headers_done, chunked, chunk_state, has_length, done, failed;
    unsigned trailer_bytes;
    char *body;
    NetHttpResult *result;
} NetHttpParser;
void net_http_parser_init(NetHttpParser *p, char *body, unsigned capacity,
                          NetHttpResult *result);
/* 1 complete, 0 more bytes needed, -1 parse error. */
int net_http_parser_feed(NetHttpParser *p, const uint8_t *data, unsigned length);
int net_http_parser_eof(NetHttpParser *p);
#endif
