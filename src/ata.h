#ifndef ATA_H
#define ATA_H
/* Optional primary-master ATA disk, polled 28-bit LBA PIO. No partitioning.
 * Probe: 1 usable ATA disk, 0 absent, -1 present but unsupported/unreadable. */
int ata_probe(void);
unsigned ata_sector_count(void);
int ata_read(unsigned lba, void *buffer, int sectors);
int ata_write(unsigned lba, const void *buffer, int sectors);
int ata_flush(void);
#endif
