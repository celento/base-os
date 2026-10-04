# QEMU networking

BaseOS includes a small, original, polling IPv4 client stack for QEMU's emulated
RTL8139 adapter. It does not replace the kernel with Linux or another OS.

## Run

`make run` and `make headless` select `-nic user,model=rtl8139`. The configuration
is deliberately fixed to QEMU's default user network:

- guest `10.0.2.15/24`
- gateway and host-loopback alias `10.0.2.2`
- DNS proxy `10.0.2.3`

An absent adapter leaves the desktop usable and reports an offline state. This
version does not implement DHCP, Wi-Fi, real-hardware provisioning, IPv6, TLS,
certificate verification, or modern browser JavaScript. Do not send credentials
or private information over its unencrypted HTTP client.

## Implemented path

PCI configuration mechanism 1 discovers vendor/device `10ec:8139`, enables I/O
and bus mastering, and initializes the RTL8139 legacy 8 KB receive ring and four
transmit buffers. Hardware interrupts remain masked. Each `net_poll()` processes
at most 32 frames, checks link state, and advances one client operation. Invalid
lengths/checksums and fragmented IPv4 packets are dropped. Receive overflow
resets the receive engine. Ethernet, ARP, IPv4, ICMP echo, DNS A/CNAME responses,
a single outbound TCP connection, and HTTP GET run in the custom C kernel.

TCP supports a three-way handshake, checksummed sequence/acknowledgment
validation, duplicate/overlapping data, bounded retransmission of SYN/request,
and a FIN exchange. Out-of-order data is not queued: duplicate acknowledgments
request retransmission. This is a bounded educational client, not a complete
RFC-conformant general-purpose TCP stack; congestion control, multiple sockets,
TCP DNS, IP fragmentation/reassembly, and arbitrary uploads are not provided.
There is no cryptographic source authentication or DNSSEC.

HTTP accepts `http://host[:port]/path` and handles Content-Length,
chunked transfer encoding, and close-delimited responses. Header storage is
4,096 bytes; hostnames are at most 127 characters; the path is at most 255
characters; response buffers are at most 32,768 bytes including their NUL.
The result explicitly reports truncation. Chunk trailers are limited to 2 KB.
Compressed content and interim responses are rejected. Redirect status and
Location are exposed to the caller, rather than silently followed by the stack.
No cookies, credentials, forms, scripting, or subresources are fetched.

Every client operation has a 15-second overall deadline; DNS/SYN/echo retries
finish earlier when the peer repeatedly fails to answer. The synchronous
convenience APIs collect input through `platform_poll()` while waiting. The
browser should use the asynchronous API so the desktop continues dispatching
normal actions.

## Integration API

`net_init()` runs after platform/memory initialization. Call `net_poll()` once
per desktop loop. The network arena occupies `NET_BASE` / `NET_CAPACITY`; DMA
buffers and protocol state fit within 64 KB. HTTP callers own their body buffer
until completion or cancellation.

`net_http_start(url, body, capacity)` returns zero when accepted, minus one when
busy or invalid. Check `net_busy()` before starting: only one DNS, ping, or HTTP
operation can run at once. A busy rejection preserves the active result.
`net_http_result()` exposes state, status, body length, content type, Location,
error, truncation, and a monotonically increasing request ID. Callers should
record that ID and only consume or cancel their own operation. Starting another
accepted request replaces the shared result; it never changes an earlier
caller's body buffer. `net_cancel()` aborts the active operation.

`net_http_get()`, `net_resolve()`, and `net_ping()` are blocking, bounded Terminal
convenience calls. `net_status()` supplies MAC/IP/link state and packet counters;
`net_last_error()` describes the most recent operation failure.

## Verification

`python3 tools/network_test.py build` creates a disposable disk, starts a local
HTTP fixture, and uses the real QEMU RTL8139/user-network path to verify gateway
echo, HTTP, chunked and close framing, a 20 KB multi-packet response, explicit
truncation, incomplete-response rejection, cancellation, and recovery. It saves
serial output and a packet capture in its printed temporary directory. It does
not boot or change the normal persistent image.

`--public-dns` additionally requests `example.com` through QEMU's DNS proxy.
That optional check depends on the host's upstream DNS/network policy; a timeout
is not reported as success. The development cloud currently supplies no DNS
reply to that optional test. Public HTTP/TLS connectivity is not claimed.

## Sources and attribution

The network code is original BaseOS code under the repository's MIT terms.
Register values and device behavior were checked against the MIT-licensed
[QEMU RTL8139 emulation](https://github.com/qemu/qemu/blob/master/hw/net/rtl8139.c),
originally copyright 2006 Igor Kovalenko. No QEMU implementation code is copied.
Protocol references are [RFC 791](https://www.rfc-editor.org/rfc/rfc791),
[RFC 826](https://www.rfc-editor.org/rfc/rfc826),
[RFC 792](https://www.rfc-editor.org/rfc/rfc792),
[RFC 1035](https://www.rfc-editor.org/rfc/rfc1035),
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293), and
[RFC 9112](https://www.rfc-editor.org/rfc/rfc9112).
