/* Deterministic transport fixtures: ordinary ATA status/command outcomes only.
 * No malformed pointers, memory-fault probes, fuzzing or guest observer. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define ATA_HOST_TEST
#include "../src/ata.c"

static unsigned ticks, word, remaining, position, command_count, flush_count;
static unsigned platform_calls, status_reads, byte_reads, byte_writes, data_words;
static unsigned identify_capacity = 512, disk_lba_base, sector_words = 256;
static unsigned release_after, released_status;
static unsigned char registers[8], status;
static int present = 1, bad_device, no_lba, no_flush, non_ata;
static int hold_issue, hold_sector, hold_completion, hold_flush;
static unsigned char bytes[512 * 512], output[512 * 257], other[512];
static struct { unsigned command, count, lba; } commands[32];

uint32_t timer_ticks(void) { return ticks; }
void platform_poll(void) {
    ++platform_calls; ++ticks;
    if (release_after && platform_calls == release_after) status = released_status;
}
unsigned char ata_test_in(unsigned short port) {
    ++byte_reads;
    if (port == ATA_CONTROL || port == ATA_STATUS) ++status_reads;
    if (!present) return 0xFF;
    if (port == ATA_CONTROL || port == ATA_STATUS) return status;
    assert(port >= ATA_DATA && port <= ATA_STATUS);
    return registers[port - ATA_DATA];
}
void ata_test_out(unsigned short port, unsigned char value) {
    ++byte_writes;
    if (port == ATA_CONTROL) { assert(value == 2); return; } /* nIEN only. */
    assert(port >= ATA_DATA && port <= ATA_STATUS);
    registers[port - ATA_DATA] = value;
    if (port != ATA_STATUS) return;
    unsigned lba = registers[3] | (unsigned)registers[4] << 8 |
                   (unsigned)registers[5] << 16 | (unsigned)(registers[6] & 15) << 24;
    assert(command_count < sizeof(commands) / sizeof(commands[0]));
    commands[command_count].command = value;
    commands[command_count].count = registers[2];
    commands[command_count].lba = lba;
    ++command_count; word = 0;
    if (value == ATA_IDENTIFY) {
        assert(registers[6] == 0xA0);
        remaining = 1; status = ATA_READY | ATA_DRQ;
        if (non_ata) registers[4] = 0x14;
    } else if (value == ATA_FLUSH) {
        ++flush_count; remaining = 0; status = hold_flush ? ATA_BUSY : ATA_READY;
    } else {
        assert(value == ATA_READ || value == ATA_WRITE);
        assert((registers[6] & 0xF0) == 0xE0); /* LBA28 primary master. */
        assert(registers[2] >= 1 && registers[2] <= 128);
        assert(lba >= disk_lba_base);
        assert(lba - disk_lba_base < sizeof(bytes) / 512);
        position = (lba - disk_lba_base) * 512;
        remaining = registers[2];
        status = hold_issue ? ATA_BUSY : ATA_READY | ATA_DRQ;
        assert(position + remaining * 512 <= sizeof(bytes));
    }
}
static void consumed(void) {
    ++data_words;
    if (++word == 256) {
        word = 0;
        if (!--remaining) status = hold_completion ? ATA_BUSY : ATA_READY;
        else if (hold_sector) status = ATA_BUSY;
    }
}
unsigned short ata_test_inw(unsigned short port) {
    assert(port == ATA_DATA && remaining && (status & ATA_DRQ) && !(status & ATA_BUSY));
    unsigned short value = 0;
    if (registers[7] == ATA_IDENTIFY) {
        if (word == 0) value = bad_device ? 0x8000 : 0;
        if (word == 49) value = no_lba ? 0 : 0x200;
        if (word == 60) value = identify_capacity;
        if (word == 61) value = identify_capacity >> 16;
        if (word == 83) value = no_flush ? 0 : 0x1000;
        if (word == 106) value = sector_words == 256 ? 0 : 0x5000;
        if (word == 117) value = sector_words;
        if (word == 118) value = sector_words >> 16;
    } else {
        assert(registers[7] == ATA_READ);
        value = bytes[position] | (unsigned)bytes[position + 1] << 8;
        position += 2;
    }
    consumed(); return value;
}
void ata_test_outw(unsigned short port, unsigned short value) {
    assert(port == ATA_DATA && remaining && registers[7] == ATA_WRITE);
    assert((status & ATA_DRQ) && !(status & ATA_BUSY));
    bytes[position++] = value; bytes[position++] = value >> 8; consumed();
}
static void clear_trace(void) {
    command_count = flush_count = platform_calls = status_reads = 0;
    byte_reads = byte_writes = data_words = 0;
    memset(commands, 0, sizeof(commands));
}
static void reset_hardware(void) {
    assert(!ata_request_active());
    ticks = word = remaining = position = disk_lba_base = 0;
    release_after = released_status = 0;
    present = 1; status = ATA_READY;
    bad_device = no_lba = no_flush = non_ata = 0;
    hold_issue = hold_sector = hold_completion = hold_flush = 0;
    identify_capacity = 512; sector_words = 256;
    memset(registers, 0, sizeof(registers));
    clear_trace();
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i * 19 + i / 1024);
    memset(output, 0xA5, sizeof(output));
}
static void mount_disk(void) {
    reset_hardware();
    assert(ata_probe() == 1 && ata_sector_count() == 512 && ata_ready());
    assert(ata_poll(1) == ATA_PROGRESS_IDLE);
    clear_trace();
}
static enum AtaProgress poll_bounded(unsigned budget) {
    unsigned before_words = data_words, before_poll = platform_calls, before_status = status_reads;
    enum AtaProgress result = ata_poll(budget);
    assert(platform_calls == before_poll);
    assert((data_words - before_words) % 256 == 0);
    assert((data_words - before_words) / 256 <= budget);
    /* Four fixed settling reads per data sector, plus a bounded number of
     * command transitions. A hardware wait cannot consume this allowance in a
     * busy loop. The exact single-read repeated-wait bound is checked below. */
    assert(status_reads - before_status <= 5 * budget + 12 * (budget / 128 + 2));
    return result;
}
static void expect_command(unsigned index, unsigned command, unsigned count, unsigned lba) {
    assert(index < command_count && commands[index].command == command);
    if (command == ATA_READ || command == ATA_WRITE) {
        assert(commands[index].count == count && commands[index].lba == lba);
    }
}
static void expect_inert_terminal(enum AtaProgress result) {
    unsigned reads = byte_reads, writes = byte_writes, words = data_words;
    assert(!ata_request_active());
    assert(poll_bounded(0) == result && poll_bounded(9) == result);
    assert(reads == byte_reads && writes == byte_writes && words == data_words);
}
static void expect_protected(void) {
    assert(!ata_ready() && !ata_request_active());
    unsigned writes = byte_writes, reads = byte_reads;
    assert(ata_request_read(0, output, 1) < 0 && ata_request_write(0, output, 1) < 0);
    assert(ata_request_flush() < 0 && ata_request_read(0, 0, 0) < 0);
    assert(ata_read(0, output, 1) < 0 && ata_write(0, output, 1) < 0 && ata_flush() < 0);
    expect_inert_terminal(ATA_PROGRESS_ERROR);
    assert(writes == byte_writes && reads == byte_reads);
}
static void test_sync_compatibility(void) {
    mount_disk();
    assert(ata_read(0, output, 257) == 0);
    assert(command_count == 3 && platform_calls == 257 && !memcmp(output, bytes, sizeof(output)));
    expect_command(0, ATA_READ, 128, 0);
    expect_command(1, ATA_READ, 128, 128);
    expect_command(2, ATA_READ, 1, 256);
    for (unsigned i = 0; i < sizeof(output); ++i) output[i] ^= 0x53;
    assert(ata_write(13, output, 257) == 0);
    assert(command_count == 6 && platform_calls == 514 && !memcmp(bytes + 13 * 512, output, sizeof(output)));
    expect_command(3, ATA_WRITE, 128, 13);
    expect_command(4, ATA_WRITE, 128, 141);
    expect_command(5, ATA_WRITE, 1, 269);
    assert(!flush_count && ata_flush() == 0 && flush_count == 1 && command_count == 7);
    expect_command(6, ATA_FLUSH, 0, 0);
    unsigned before = command_count;
    assert(ata_read(512, output, 1) < 0 && ata_write(500, output, 13) < 0);
    assert(ata_read(0, output, -1) < 0 && ata_read(0, 0, 1) < 0);
    assert(ata_read(UINT_MAX, output, 1) < 0 && ata_read(513, 0, 0) < 0);
    assert(ata_read(512, 0, 0) == 0 && command_count == before);
    status = ATA_BUSY; assert(ata_read(0, output, 1) < 0 && ticks >= 2 * TIMER_HZ);
    expect_protected();
    status = ATA_READY;
    assert(ata_probe() == 1 && ata_read(0, output, 1) == 0);
}
static void test_async_budgets_and_ownership(void) {
    mount_disk();
    assert(ata_request_read(0, output, 257) == 0);
    assert(ata_request_active() && ata_ready() && !byte_writes && !byte_reads);
    assert(poll_bounded(0) == ATA_PROGRESS_MORE && data_words == 0 && command_count == 1);
    unsigned writes = byte_writes, reads = byte_reads;
    memset(other, 0xB6, sizeof(other));
    assert(ata_request_read(0, other, 1) < 0 && ata_request_write(0, other, 1) < 0);
    assert(ata_request_read(0, 0, 0) < 0 && ata_request_flush() < 0);
    assert(ata_probe() < 0 && ata_ready());
    assert(ata_read(0, other, 1) < 0 && ata_write(0, other, 1) < 0 && ata_flush() < 0);
    assert(writes == byte_writes && reads == byte_reads);
    enum AtaProgress result;
    unsigned calls = 0;
    do {
        result = poll_bounded(7);
        assert(result == ATA_PROGRESS_MORE || result == ATA_PROGRESS_DONE);
        assert(++calls <= 37);
    } while (result != ATA_PROGRESS_DONE);
    assert(calls == 37 && command_count == 3 && !platform_calls);
    assert(!memcmp(output, bytes, sizeof(output)));
    for (unsigned i = 0; i < sizeof(other); ++i) assert(other[i] == 0xB6);
    expect_command(0, ATA_READ, 128, 0);
    expect_command(1, ATA_READ, 128, 128);
    expect_command(2, ATA_READ, 1, 256);
    expect_inert_terminal(ATA_PROGRESS_DONE);
    assert(ata_request_write(512, output, 1) < 0);
    expect_inert_terminal(ATA_PROGRESS_DONE);
    for (unsigned i = 0; i < sizeof(output); ++i) output[i] ^= 0x39;
    assert(ata_request_write(13, output, 257) == 0);
    assert(poll_bounded(128) == ATA_PROGRESS_MORE && data_words == 385 * 256);
    assert(poll_bounded(128) == ATA_PROGRESS_MORE && data_words == 513 * 256);
    assert(poll_bounded(128) == ATA_PROGRESS_DONE && data_words == 514 * 256);
    assert(command_count == 6 && !flush_count && !memcmp(bytes + 13 * 512, output, sizeof(output)));
    expect_command(3, ATA_WRITE, 128, 13);
    expect_command(4, ATA_WRITE, 128, 141);
    expect_command(5, ATA_WRITE, 1, 269);
    assert(ata_request_read(512, 0, 0) == 0 && !ata_request_active());
    expect_inert_terminal(ATA_PROGRESS_DONE);
    assert(ata_request_write(512, 0, 0) == 0);
    expect_inert_terminal(ATA_PROGRESS_DONE);
}
static void test_waits_and_flush_barriers(void) {
    mount_disk();
    hold_issue = hold_sector = hold_completion = 1;
    assert(ata_request_write(0, bytes + 1024, 2) == 0);
    assert(poll_bounded(8) == ATA_PROGRESS_WAIT && command_count == 1 && data_words == 0);
    unsigned reads = status_reads;
    assert(poll_bounded(8) == ATA_PROGRESS_WAIT && status_reads == reads + 1 && data_words == 0);
    status = ATA_READY | ATA_DRQ;
    assert(poll_bounded(8) == ATA_PROGRESS_WAIT && data_words == 256 && ata_request_active());
    assert(poll_bounded(0) == ATA_PROGRESS_WAIT && data_words == 256);
    status = ATA_READY | ATA_DRQ;
    assert(poll_bounded(8) == ATA_PROGRESS_WAIT && data_words == 512 && ata_request_active());
    assert(ata_request_flush() < 0 && !flush_count);
    status = ATA_READY;
    assert(poll_bounded(0) == ATA_PROGRESS_DONE);
    hold_flush = 1;
    assert(ata_request_flush() == 0 && command_count == 1);
    assert(poll_bounded(0) == ATA_PROGRESS_WAIT && command_count == 2 && flush_count == 1);
    assert(ata_request_read(0, output, 2) < 0);
    reads = status_reads;
    assert(poll_bounded(0) == ATA_PROGRESS_WAIT && status_reads == reads + 1);
    status = ATA_READY;
    assert(poll_bounded(0) == ATA_PROGRESS_DONE && ata_ready());
    hold_issue = hold_sector = hold_completion = 0;
    assert(ata_request_read(0, output, 2) == 0 && poll_bounded(2) == ATA_PROGRESS_DONE);
    assert(!memcmp(output, bytes + 1024, 1024));
    assert(command_count == 3 && flush_count == 1);
    expect_command(0, ATA_WRITE, 2, 0);
    expect_command(1, ATA_FLUSH, 0, 0);
    expect_command(2, ATA_READ, 2, 0);
}
static void test_phase_timeouts(void) {
    /* Idle BSY, idle unexpected DRQ, command BSY, missing DRQ, post-data BSY,
     * post-data DRQ, pre-flush BSY and outstanding flush BSY. */
    for (unsigned phase = 0; phase < 8; ++phase) {
        mount_disk(); ticks = 20;
        if (phase == 0 || phase == 6) status = ATA_BUSY;
        if (phase == 1) status = ATA_READY | ATA_DRQ;
        if (phase == 2 || phase == 3) hold_issue = 1;
        if (phase == 4 || phase == 5) hold_completion = 1;
        if (phase == 7) hold_flush = 1;
        assert((phase >= 6 ? ata_request_flush() : ata_request_read(0, output, 1)) == 0);
        assert(poll_bounded(1) == ATA_PROGRESS_WAIT);
        if (phase == 3) status = ATA_READY;
        if (phase == 5) status = ATA_READY | ATA_DRQ;
        unsigned words = data_words, commands_before = command_count;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            unsigned reads = status_reads;
            assert(poll_bounded(8) == ATA_PROGRESS_WAIT && status_reads == reads + 1);
        }
        assert(ticks == 20 && words == data_words && commands_before == command_count);
        ticks += 2 * TIMER_HZ - 1;
        assert(ata_request_read(0, other, 1) < 0 && ata_request_flush() < 0 && ata_probe() < 0);
        assert(poll_bounded(8) == ATA_PROGRESS_WAIT);
        ++ticks;
        assert(poll_bounded(8) == ATA_PROGRESS_ERROR);
        assert(words == data_words && commands_before == command_count);
        expect_protected();
    }
}
static void test_deadline_progress_and_wrap(void) {
    mount_disk();
    assert(ata_request_read(0, output, 3) == 0 && poll_bounded(1) == ATA_PROGRESS_MORE);
    ticks += 2 * TIMER_HZ - 1;
    assert(poll_bounded(1) == ATA_PROGRESS_MORE);
    ticks += 2 * TIMER_HZ - 1;
    assert(poll_bounded(1) == ATA_PROGRESS_DONE);
    assert(!memcmp(output, bytes, 3 * 512));
    /* A no-data budget must not refresh the phase deadline. */
    assert(ata_request_read(0, output, 1) == 0 && poll_bounded(0) == ATA_PROGRESS_MORE);
    ticks += 2 * TIMER_HZ - 1;
    assert(poll_bounded(0) == ATA_PROGRESS_MORE);
    ++ticks;
    assert(poll_bounded(0) == ATA_PROGRESS_MORE);
    status = ATA_BUSY;
    assert(poll_bounded(0) == ATA_PROGRESS_ERROR);
    expect_protected();
    mount_disk();
    ticks = UINT_MAX - TIMER_HZ; hold_issue = 1;
    assert(ata_request_read(0, output, 1) == 0 && poll_bounded(1) == ATA_PROGRESS_WAIT);
    ticks += 2 * TIMER_HZ - 1;
    assert(poll_bounded(1) == ATA_PROGRESS_WAIT);
    ++ticks;
    assert(poll_bounded(1) == ATA_PROGRESS_ERROR);
    expect_protected();
}
static void test_late_ready_polls(void) {
    mount_disk();
    assert(ata_request_read(0, output, 1) == 0);
    ticks += 3 * TIMER_HZ;
    /* An unissued request may still find the device idle after a long gap. */
    assert(poll_bounded(0) == ATA_PROGRESS_MORE && command_count == 1 && !data_words);
    ticks += 3 * TIMER_HZ;
    assert(poll_bounded(0) == ATA_PROGRESS_MORE && command_count == 1 && !data_words);
    ticks += 3 * TIMER_HZ;
    assert(poll_bounded(1) == ATA_PROGRESS_DONE && data_words == 256 && ata_ready());
    assert(!memcmp(output, bytes, 512));

    /* A previously waiting data phase can become ready while the caller is
     * away. The ready observation must win over the old phase deadline. */
    hold_issue = 1;
    assert(ata_request_read(1, output, 1) == 0 && poll_bounded(1) == ATA_PROGRESS_WAIT);
    ticks += 3 * TIMER_HZ; status = ATA_READY | ATA_DRQ;
    assert(poll_bounded(0) == ATA_PROGRESS_MORE);
    assert(poll_bounded(1) == ATA_PROGRESS_DONE && ata_ready());
    assert(!memcmp(output, bytes + 512, 512));

    hold_issue = 0; hold_completion = 1;
    assert(ata_request_write(2, output, 1) == 0 && poll_bounded(1) == ATA_PROGRESS_WAIT);
    unsigned words = data_words;
    ticks += 3 * TIMER_HZ; status = ATA_READY;
    assert(poll_bounded(0) == ATA_PROGRESS_DONE && words == data_words && ata_ready());
    assert(!memcmp(bytes + 512, bytes + 1024, 512));

    hold_flush = 1;
    assert(ata_request_flush() == 0 && poll_bounded(0) == ATA_PROGRESS_WAIT);
    ticks += 3 * TIMER_HZ; status = ATA_READY;
    assert(poll_bounded(0) == ATA_PROGRESS_DONE && flush_count == 1 && ata_ready());
    hold_flush = 0;
    assert(ata_request_flush() == 0);
    ticks += 3 * TIMER_HZ;
    assert(poll_bounded(0) == ATA_PROGRESS_DONE && flush_count == 2 && ata_ready());

    /* A true timeout remains terminal even if hardware later reports ready. */
    hold_issue = 1;
    assert(ata_request_read(0, output, 1) == 0 && poll_bounded(1) == ATA_PROGRESS_WAIT);
    ticks += 3 * TIMER_HZ;
    assert(poll_bounded(0) == ATA_PROGRESS_ERROR);
    status = ATA_READY | ATA_DRQ;
    expect_protected();
}
static void test_transport_errors(void) {
    const unsigned char failures[] = { 0, 0xFF, ATA_READY | ATA_ERROR, ATA_READY | ATA_FAULT };
    for (unsigned phase = 0; phase < 5; ++phase) {
        for (unsigned failure = 0; failure < sizeof(failures); ++failure) {
            mount_disk();
            if (phase == 1) hold_issue = 1;
            if (phase == 2) hold_completion = 1;
            if (phase == 4) hold_flush = 1;
            assert((phase >= 3 ? ata_request_flush() : ata_request_write(0, output, 1)) == 0);
            if (phase == 1 || phase == 2 || phase == 4)
                assert(poll_bounded(1) == ATA_PROGRESS_WAIT);
            status = failures[failure];
            unsigned words = data_words, commands_before = command_count;
            assert(poll_bounded(8) == ATA_PROGRESS_ERROR);
            assert(words == data_words && commands_before == command_count);
            expect_protected();
        }
    }
    /* ERR and DF are ignored while BSY is set, as the protocol requires. */
    mount_disk(); status = ATA_BUSY | ATA_ERROR | ATA_FAULT;
    assert(ata_request_read(0, output, 1) == 0 && poll_bounded(8) == ATA_PROGRESS_WAIT);
    assert(ata_ready()); status = ATA_READY;
    assert(poll_bounded(8) == ATA_PROGRESS_DONE);
}
static void test_probe_and_lba28(void) {
    reset_hardware(); present = 0;
    assert(ata_probe() == 0 && !ata_ready() && !ata_sector_count());
    assert(ata_request_read(0, output, 1) < 0 && ata_request_flush() < 0);
    assert(ata_poll(1) == ATA_PROGRESS_IDLE);
    reset_hardware(); status = 0;
    assert(ata_probe() == 0 && !ata_ready());
    for (unsigned unsupported = 0; unsupported < 7; ++unsupported) {
        reset_hardware();
        if (unsupported == 0) bad_device = 1;
        if (unsupported == 1) no_lba = 1;
        if (unsupported == 2) no_flush = 1;
        if (unsupported == 3) sector_words = 2048;
        if (unsupported == 4) non_ata = 1;
        if (unsupported == 5) identify_capacity = 0;
        if (unsupported == 6) identify_capacity = 0x10000001;
        assert(ata_probe() < 0 && !ata_ready() && !ata_sector_count());
        assert(ata_request_read(0, output, 1) < 0 && ata_request_flush() < 0);
    }
    reset_hardware(); identify_capacity = 0x10000000;
    assert(ata_probe() == 1 && ata_sector_count() == 0x10000000);
    clear_trace(); disk_lba_base = 0x0FFFFF00;
    assert(ata_request_read(0x0FFFFF7F, output, 129) == 0);
    assert(poll_bounded(128) == ATA_PROGRESS_MORE && poll_bounded(1) == ATA_PROGRESS_DONE);
    expect_command(0, ATA_READ, 128, 0x0FFFFF7F);
    expect_command(1, ATA_READ, 1, 0x0FFFFFFF);
    assert(!memcmp(output, bytes + 127 * 512, 129 * 512));
    assert(ata_request_read(0x0FFFFFFF, output, 2) < 0);
    assert(ata_request_read(0x10000000, 0, 0) == 0);
}
static void test_sync_wait_completion(void) {
    mount_disk(); hold_completion = 1;
    release_after = 3; released_status = ATA_READY;
    assert(ata_read(0, output, 1) == 0 && platform_calls == 3);
    assert(!memcmp(output, bytes, 512));
    hold_flush = 1; release_after = 6;
    assert(ata_flush() == 0 && platform_calls == 6 && flush_count == 1);
    /* A synchronous flush also protects an uncertain pre-command timeout. */
    status = ATA_BUSY;
    assert(ata_flush() < 0 && !ata_ready());
    expect_protected();
}
int main(void) {
    test_sync_compatibility();
    test_async_budgets_and_ownership();
    test_waits_and_flush_barriers();
    test_phase_timeouts();
    test_deadline_progress_and_wrap();
    test_late_ready_polls();
    test_transport_errors();
    test_probe_and_lba28();
    test_sync_wait_completion();
    puts("ATA: bounded resumable PIO, exclusive ownership, exact flush barriers, persistent deadlines, errors/remount, LBA28 bounds and synchronous compatibility passed");
}
