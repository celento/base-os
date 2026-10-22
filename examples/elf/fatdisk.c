/* Files on a FAT exchange disk through the C library:
 *   build/toolchain/bin/i686-baseos-gcc -O2 fatdisk.c -o fatdisk
 * Expects a FAT volume on the secondary master (/disk1) holding README.TXT
 * ("hello from the host\n") and GAMES/OLD SAVE.DAT. Leaves NEW/ with a big
 * file and a renamed note for the host to check. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *what) {
    printf("fatdisk: %s failed (errno %d)\n", what, errno);
    return 1;
}

static unsigned char block[65536];
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 2654435761u >> 24); }

static int listed(const char *path, const char *name) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int found = 0;
    for (struct dirent *entry; (entry = readdir(dir));)
        if (!strcmp(entry->d_name, name)) found = 1;
    closedir(dir);
    return found;
}

int main(void) {
    if (!listed("/", "disk1")) return fail("disk1 in /");
    char text[64] = {0};
    int fd = open("/disk1/readme.txt", O_RDONLY);
    if (fd < 0 || read(fd, text, sizeof text - 1) != 20 || strcmp(text, "hello from the host\n"))
        return fail("read README.TXT");
    close(fd);

    if (mkdir("/disk1/New", 0755)) return fail("mkdir");
    if (mkdir("/disk1/New", 0755) == 0 || errno != EEXIST) return fail("mkdir twice");
    if (chdir("/disk1/New")) return fail("chdir");
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/disk1/New")) return fail("getcwd");

    /* 3 MiB written in pieces, patched, read back. */
    fd = open("A big file.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("open big");
    const unsigned size = 3u << 20;
    for (unsigned base = 0; base < size; base += sizeof block) {
        for (unsigned i = 0; i < sizeof block; ++i) block[i] = pattern(base + i);
        if (write(fd, block, sizeof block) != (ssize_t)sizeof block) return fail("write big");
    }
    if (lseek(fd, 1000001, SEEK_SET) != 1000001 || write(fd, "patched", 7) != 7) return fail("patch");
    int second = open("A BIG FILE.BIN", O_RDONLY);
    struct stat st;
    if (second < 0 || fstat(second, &st) || st.st_size != (off_t)size) return fail("second open sees size");
    if (lseek(second, 1000000, SEEK_SET) != 1000000 || read(second, text, 9) != 9 ||
        memcmp(text + 1, "patched", 7) || (unsigned char)text[8] != pattern(1000008))
        return fail("second open sees data");
    close(second);
    if (unlink("A big file.bin") == 0 || errno != EBUSY) return fail("unlink while open");
    if (ftruncate(fd, size - 100) || fstat(fd, &st) || st.st_size != (off_t)(size - 100)) return fail("ftruncate");
    if (lseek(fd, 0, SEEK_SET)) return fail("rewind");
    for (unsigned base = 0; base < size - 100; base += sizeof block) {
        unsigned want = size - 100 - base < sizeof block ? size - 100 - base : sizeof block;
        if (read(fd, block, sizeof block) != (ssize_t)want) return fail("read big");
        for (unsigned i = 0; i < want; ++i) {
            unsigned at = base + i;
            unsigned char expect = at >= 1000001 && at < 1000008 ? (unsigned char)"patched"[at - 1000001] : pattern(at);
            if (block[i] != expect) return fail("compare big");
        }
    }
    if (close(fd)) return fail("close big");

    FILE *out = fopen("note.txt", "w");
    if (!out || fprintf(out, "written on BaseOS\n") < 0 || fclose(out)) return fail("fopen note");
    out = fopen("note.txt", "a");
    if (!out || fprintf(out, "appended\n") < 0 || fclose(out)) return fail("append note");
    if (rename("note.txt", "../GAMES/Renamed Note.txt")) return fail("rename across folders");
    if (stat("/disk1/GAMES/Renamed Note.txt", &st) || st.st_size != 18 + 9) return fail("stat renamed");
    if (rename("/disk1/README.TXT", "/elsewhere.txt") == 0 || errno != EXDEV) return fail("cross-volume rename");
    if (mkdir("Empty", 0755) || rmdir("Empty")) return fail("rmdir");
    if (rmdir("/disk1/GAMES") == 0 || errno != ENOTEMPTY) return fail("rmdir non-empty");
    if (chdir("..") || !getcwd(cwd, sizeof cwd) || strcmp(cwd, "/disk1")) return fail("chdir ..");
    if (chdir("..") || !getcwd(cwd, sizeof cwd) || strcmp(cwd, "/")) return fail("chdir to native root");

    /* Save FSInfo and the clean mark before the host looks. */
    fd = open("/disk1/New/A big file.bin", O_RDONLY);
    if (fd < 0 || fsync(fd) || close(fd)) return fail("fsync");
    printf("fat disk ok: %u bytes\n", size - 100);
    return 0;
}
