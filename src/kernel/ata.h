#ifndef ATA_H
#define ATA_H
/* Optional primary-master ATA disk, polled 28-bit LBA PIO. No partitioning.
 * Probe: 1 usable ATA disk, 0 absent, -1 unsupported/unreadable or a request
 * is active. An active-request probe does not touch hardware or request state. */
int ata_probe(void);
unsigned ata_sector_count(void);
/* Probed and usable, including while an accepted request waits for hardware.
 * A transport error makes this false until an explicit successful probe. */
int ata_ready(void);

enum AtaProgress {
    ATA_PROGRESS_ERROR = -1,
    ATA_PROGRESS_IDLE = 0,
    ATA_PROGRESS_WAIT = 1,
    ATA_PROGRESS_MORE = 2,
    ATA_PROGRESS_DONE = 3
};
/* One controller-owned request. Return 0 when accepted, -1 when rejected.
 * Rejection never changes the existing request or its terminal result.
 * Buffers must stay allocated and unchanged (except by a read) until DONE or
 * ERROR; no cancellation is supported. Submission performs no port I/O.
 * A valid zero-sector transfer completes immediately and permits NULL and
 * lba == ata_sector_count(); invalid/unready/overlapping requests still fail.
 * Writes finish their ATA command; only an explicit flush is a cache barrier. */
int ata_request_read(unsigned lba, void *buffer, int sectors);
int ata_request_write(unsigned lba, const void *buffer, int sectors);
int ata_request_flush(void);
int ata_request_active(void);
/* Advance without waiting: at most sector_budget complete 512-byte transfers.
 * Zero permits status checks and command/flush progress, never data transfer.
 * WAIT needs hardware progress or deadline expiry; MORE can run immediately
 * (or needs a nonzero budget). Call regularly, even while waiting: two-second
 * protocol-phase deadlines survive yields and use the interrupt-driven clock.
 * Only an observed hardware wait can time out; ready data/completion remains
 * valid after a caller scheduling gap. Zero-budget polls do not reset deadlines.
 * No platform_poll, application dispatch, IRQ changes, allocation or sleep.
 * DONE/ERROR persist until another accepted request or an idle explicit probe.
 * All APIs have one cooperative caller; they are not interrupt/reentrant APIs. */
enum AtaProgress ata_poll(unsigned sector_budget);

/* Blocking compatibility wrappers. A successful read/write means the complete
 * command finished, and a successful flush means the cache barrier completed.
 * These never turn acceptance of an asynchronous request into success. */
int ata_read(unsigned lba, void *buffer, int sectors);
int ata_write(unsigned lba, const void *buffer, int sectors);
int ata_flush(void);
#endif
