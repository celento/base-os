/* Files through the C library on the BaseOS ELF runtime:
 *   build/toolchain/bin/i686-baseos-gcc -O2 files.c -o files
 * Writes, reads and lists files with stdio, POSIX directories and pipes, and
 * a file larger than the volume ever holds in memory. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *what) {
    printf("files: %s failed (errno %d)\n", what, errno);
    return 1;
}

/* A 5 MiB file, past the volume's per-file memory limit: written, patched
 * and read back in pieces through file descriptors. */
#define BIG (5u << 20)
static unsigned char block[65536];
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 2654435761u >> 24); }
static int big_file(void) {
    int fd = open("big.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("open big.bin");
    for (unsigned base = 0; base < BIG; base += sizeof block) {
        for (unsigned i = 0; i < sizeof block; ++i) block[i] = pattern(base + i);
        if (write(fd, block, sizeof block) != (ssize_t)sizeof block) return fail("write big.bin");
    }
    if (lseek(fd, 3000000, SEEK_SET) != 3000000 || write(fd, "patched", 7) != 7) return fail("patch big.bin");
    struct stat st;
    if (fstat(fd, &st) || st.st_size != BIG) return fail("fstat big.bin");
    if (lseek(fd, 0, SEEK_SET)) return fail("rewind big.bin");
    for (unsigned base = 0; base < BIG; base += sizeof block) {
        if (read(fd, block, sizeof block) != (ssize_t)sizeof block) return fail("read big.bin");
        for (unsigned i = 0; i < sizeof block; ++i) {
            unsigned at = base + i;
            unsigned char want = at >= 3000000 && at < 3000007 ? (unsigned char)"patched"[at - 3000000] : pattern(at);
            if (block[i] != want) return fail("compare big.bin");
        }
    }
    if (ftruncate(fd, 1000) || fstat(fd, &st) || st.st_size != 1000) return fail("ftruncate big.bin");
    if (close(fd) || remove("big.bin")) return fail("remove big.bin");
    printf("big file ok: %u bytes\n", BIG);
    return 0;
}

int main(void) {
    if (mkdir("/c-files", 0755) && errno != EEXIST) return fail("mkdir");
    if (chdir("/c-files")) return fail("chdir");
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/c-files")) return fail("getcwd");

    FILE *out = fopen("notes.txt", "w");
    if (!out) return fail("fopen for writing");
    for (int i = 1; i <= 100; ++i) fprintf(out, "line %d\n", i);
    if (fclose(out)) return fail("fclose");

    FILE *in = fopen("notes.txt", "r");
    if (!in) return fail("fopen for reading");
    char line[32];
    int lines = 0, last = 0;
    while (fgets(line, sizeof line, in)) {
        ++lines;
        sscanf(line, "line %d", &last);
    }
    fclose(in);
    printf("read %d lines, last %d\n", lines, last);

    struct stat st;
    if (stat("notes.txt", &st) || !S_ISREG(st.st_mode)) return fail("stat");
    printf("notes.txt is %ld bytes\n", (long)st.st_size);
    if (rename("notes.txt", "kept.txt")) return fail("rename");

    DIR *dir = opendir(".");
    if (!dir) return fail("opendir");
    int entries = 0;
    for (struct dirent *entry; (entry = readdir(dir));)
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            printf("entry %s\n", entry->d_name);
            ++entries;
        }
    closedir(dir);

    int ends[2];
    char word[8] = {0};
    if (pipe(ends) || write(ends[1], "pipe", 4) != 4 || read(ends[0], word, sizeof word) != 4)
        return fail("pipe");
    close(ends[0]);
    close(ends[1]);

    if (big_file()) return 3;
    if (remove("kept.txt") || chdir("/") || rmdir("/c-files")) return fail("cleanup");
    printf("files ok: %d entry, %s\n", entries, word);
    return lines == 100 && last == 100 && st.st_size == 792 && entries == 1 ? 0 : 2;
}
