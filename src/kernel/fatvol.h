#ifndef FATVOL_H
#define FATVOL_H
#include "kernel/fat.h"

/* FAT volumes on the exchange disks (secondary ATA channel), mounted at
 * /disk1 (master) and /disk2 (slave) in the paths programs use. The native
 * volume never holds these names' contents: a mounted volume hides a native
 * root folder of the same name. See docs/FAT.md. */

#define FATVOL_MAX 2

/* Probes the exchange disks and mounts the FAT volumes found. */
void fatvol_init(void);
/* The mounted volume at index, or 0. */
FatVolume *fatvol_get(int index);
/* "disk1", "disk2". */
const char *fatvol_name(int index);
/* The volume an absolute, normalized path ("/disk1/GAMES") is on, with
 * *rest set past the mount name ("" or "/GAMES"); -1 when it is on none. */
int fatvol_route(const char *path, const char **rest);
/* After a change: the volume is synced (FSInfo, clean mark) once idle. */
void fatvol_changed(int index);
int fatvol_sync(int index);
/* Kernel loop: syncs volumes a second after their last change. */
void fatvol_poll(void);
#endif
