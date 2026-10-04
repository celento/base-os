#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ATA_HOST_TEST
#include "../src/ata.c"
static unsigned ticks, word, remaining, position, command_count, flush_count;
static unsigned char registers[8], status;
static int present = 1, timeout, bad_device;
static unsigned char bytes[512 * 512], output[512 * 257];
uint32_t timer_ticks(void) { return ticks; }
void platform_poll(void) { ++ticks; }
unsigned char ata_test_in(unsigned short port) {
    if (!present) return 0xFF;
    if (port == ATA_CONTROL || port == ATA_STATUS) return timeout ? ATA_BUSY : status;
    return registers[port - ATA_DATA];
}
void ata_test_out(unsigned short port, unsigned char value) {
    if (port == ATA_CONTROL) return;
    registers[port - ATA_DATA] = value;
    if (port != ATA_STATUS) return;
    ++command_count; word = 0;
    if (value == ATA_IDENTIFY) { remaining = 1; status = ATA_READY | ATA_DRQ; }
    else if (value == ATA_FLUSH) { ++flush_count; remaining = 0; status = ATA_READY; }
    else {
        assert(value == ATA_READ || value == ATA_WRITE);
        position = (registers[3] | (unsigned)registers[4] << 8 | (unsigned)registers[5] << 16) * 512;
        remaining = registers[2]; status = ATA_READY | ATA_DRQ;
        assert(position + remaining * 512 <= sizeof(bytes));
    }
}
static void consumed(void) {
    if (++word == 256) {
        word = 0;
        if (!--remaining) status = ATA_READY;
    }
}
unsigned short ata_test_inw(unsigned short port) {
    assert(port == ATA_DATA && remaining && (status & ATA_DRQ));
    unsigned short value = 0;
    if (registers[7] == ATA_IDENTIFY) {
        if (word == 0) value = bad_device ? 0x8000 : 0;
        if (word == 49) value = 0x200;
        if (word == 60) value = 512;
        if (word == 83) value = 0x1000;
    } else {
        assert(registers[7] == ATA_READ);
        value = bytes[position] | (unsigned)bytes[position + 1] << 8;
        position += 2;
    }
    consumed(); return value;
}
void ata_test_outw(unsigned short port, unsigned short value) {
    assert(port == ATA_DATA && remaining && registers[7] == ATA_WRITE);
    bytes[position++] = value; bytes[position++] = value >> 8; consumed();
}
int main(void) {
    present = 0; assert(ata_probe() == 0);
    present = 1; status = ATA_READY;
    assert(ata_probe() == 1 && ata_sector_count() == 512);
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i * 19 + i / 1024);
    unsigned before = command_count;
    assert(ata_read(0, output, 257) == 0);
    assert(command_count == before + 3 && !memcmp(output, bytes, sizeof(output)));
    for (unsigned i = 0; i < sizeof(output); ++i) output[i] ^= 0x53;
    assert(ata_write(13, output, 257) == 0);
    assert(!memcmp(bytes + 13 * 512, output, sizeof(output)));
    assert(ata_flush() == 0 && flush_count == 1);
    before = command_count;
    assert(ata_read(512, output, 1) < 0 && ata_write(500, output, 13) < 0);
    assert(ata_read(0, output, -1) < 0 && ata_read(0, 0, 1) < 0);
    assert(ata_read(512, 0, 0) == 0 && command_count == before);
    timeout = 1; assert(ata_read(0, output, 1) < 0 && ticks >= 2 * TIMER_HZ);
    timeout = 0; assert(ata_read(0, output, 1) < 0);
    assert(ata_probe() == 1 && ata_read(0, output, 1) == 0);
    bad_device = 1; assert(ata_probe() < 0 && ata_write(0, output, 1) < 0);
    puts("ATA: absent/unsupported devices, chunked PIO read/write, flush, bounds, timeout and explicit remount passed");
}
