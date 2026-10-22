/* Exchange-disk FAT volumes for programs. See fatvol.h. */
#include "kernel/fatvol.h"
#include "kernel/ata.h"
#include "kernel/fs.h"
#include "kernel/kernel.h"
#include "kernel/platform.h"

#define SYNC_DELAY_MS 1000u

static struct {
    FatVolume volume;
    int mounted;
    int unsynced;
    uint32_t changed_at;
} volumes[FATVOL_MAX];

static const char *const names[FATVOL_MAX] = {"disk1", "disk2"};

static int unit_read(void *context, uint32_t lba, void *buffer, uint32_t sectors) {
    int unit = (int)(uintptr_t)context;
    /* LBA28 transfers: at most 256 sectors per command. */
    while (sectors) {
        uint32_t n = sectors > 256 ? 256 : sectors;
        if (ata_unit_read(unit, lba, buffer, (int)n) < 0) return -1;
        lba += n; sectors -= n; buffer = (char *)buffer + n * 512u;
    }
    return 0;
}
static int unit_write(void *context, uint32_t lba, const void *buffer, uint32_t sectors) {
    int unit = (int)(uintptr_t)context;
    while (sectors) {
        uint32_t n = sectors > 256 ? 256 : sectors;
        if (ata_unit_write(unit, lba, buffer, (int)n) < 0) return -1;
        lba += n; sectors -= n; buffer = (const char *)buffer + n * 512u;
    }
    return 0;
}
static int unit_flush(void *context) { return ata_unit_flush((int)(uintptr_t)context) < 0 ? -1 : 0; }

void fatvol_init(void) {
    for (int i = 0; i < FATVOL_MAX; ++i) {
        int unit = i + 1;
        if (ata_unit_probe(unit) != 1) continue;
        FatDevice device = {unit_read, unit_write, unit_flush, (void *)(uintptr_t)unit, ata_unit_sectors(unit)};
        uint32_t start, sectors;
        if (fat_find(&device, &start, &sectors) || fat_mount(&volumes[i].volume, &device, start, sectors)) {
            kprint_debug("FAT: no volume on ");
            kprint_debug(names[i]);
            serial_write('\n');
            continue;
        }
        volumes[i].volume.now = fs_clock;
        volumes[i].mounted = 1;

        kprint_debug("FAT: mounted /");
        kprint_debug(names[i]);
        kprint_debug(volumes[i].volume.was_dirty ? " (not cleanly unmounted)\n" : "\n");
    }
}

FatVolume *fatvol_get(int index) {
    return index >= 0 && index < FATVOL_MAX && volumes[index].mounted ? &volumes[index].volume : 0;
}
const char *fatvol_name(int index) { return index >= 0 && index < FATVOL_MAX ? names[index] : ""; }

int fatvol_route(const char *path, const char **rest) {
    if (*path != '/') return -1;
    for (int i = 0; i < FATVOL_MAX; ++i) {
        if (!volumes[i].mounted) continue;
        const char *a = path + 1, *b = names[i];
        while (*b && *a == *b) ++a, ++b;
        if (*b || (*a && *a != '/')) continue;
        *rest = a;
        return i;
    }
    return -1;
}

void fatvol_changed(int index) {
    if (!fatvol_get(index)) return;
    volumes[index].unsynced = 1;
    volumes[index].changed_at = timer_milliseconds();
}

int fatvol_sync(int index) {
    FatVolume *volume = fatvol_get(index);
    if (!volume) return FAT_ERR_INVALID;
    int result = fat_sync(volume);
    if (!result) volumes[index].unsynced = 0;
    return result;
}

void fatvol_poll(void) {
    for (int i = 0; i < FATVOL_MAX; ++i)
        if (volumes[i].mounted && volumes[i].unsynced &&
            timer_milliseconds() - volumes[i].changed_at >= SYNC_DELAY_MS)
            fatvol_sync(i);
}
