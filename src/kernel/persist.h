#ifndef PERSIST_H
#define PERSIST_H
/* Drive A, 80 cylinders, two heads. 18-sector legacy disks are read-only
 * at the filesystem layer; 36-sector disks hold two full snapshots. */
void disk_configure(unsigned sectors_per_track);
unsigned disk_sector_count(void);
int disk_read(unsigned int lba, void *buf, int sectors);
int disk_write(unsigned int lba, const void *buf, int sectors);
#endif
