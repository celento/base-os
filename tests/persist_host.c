#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
static unsigned char bounce[DMA_CAPACITY];
#undef DMA_BASE
#define DMA_BASE ((uintptr_t)bounce)
#define PERSIST_HOST_TEST
#include "../src/persist.c"

static unsigned now;
static int reset_count, recal_count, seek_count, transfers;
static int cylinder, reset_senses, fail_transfer, fail_command_byte;
static int fail_result, bad_status, no_terminal_count, dma_masked = 1;
static unsigned char command[9], result[7];
static int command_n, command_len, result_n, result_pos, terminal_count;
uint32_t timer_ticks(void) { return now++; }
void platform_poll(void) {}
void timer_delay(unsigned delay) { now += delay; }
void kmemcpy(void *d, const void *s, int n) { memcpy(d, s, n); }
static void finish_command(void) {
    result_pos = result_n = 0;
    switch (command[0]) {
    case CMD_SPECIFY: break;
    case CMD_RECALIBRATE: cylinder = 0; recal_count++; break;
    case CMD_SEEK: cylinder = command[2]; seek_count++; break;
    case CMD_SENSE_INT:
        result_n = 2;
        result[0] = reset_senses ? 0xc0 : 0x20;
        result[1] = cylinder;
        if (reset_senses) reset_senses--;
        break;
    case CMD_READ: case CMD_WRITE:
        transfers++;
        assert(cylinder == command[2]);
        result_n = 7; memset(result, 0, sizeof(result)); result[6] = 2;
        if (fail_transfer) { result[0] = 0x40; fail_transfer--; }
        if (bad_status) result[2] = 0x20;
        terminal_count = !no_terminal_count;
        if (command[0] == CMD_READ) memset(bounce, 0xA7, sizeof(bounce));
        break;
    default: assert(0);
    }
    command_n = 0;
}
void fdc_test_out(unsigned short port, unsigned char value) {
    if (port == 0x0a) { dma_masked = (value & 4) != 0; return; }
    if (port == FDC_DOR && value == 0) {
        reset_count++; reset_senses = 4; cylinder = 0;
        command_n = result_n = result_pos = 0;
    }
    if (port != FDC_FIFO) return;
    if (!command_n) {
        command_len = value == CMD_SPECIFY ? 3 : value == CMD_RECALIBRATE ? 2 :
                      value == CMD_SENSE_INT ? 1 : value == CMD_SEEK ? 3 : 9;
    }
    command[command_n++] = value;
    if (command_n == command_len) finish_command();
}
unsigned char fdc_test_in(unsigned short port) {
    if (port == FDC_MSR) {
        if (fail_command_byte && command_n == fail_command_byte &&
            (command[0] == CMD_READ || command[0] == CMD_WRITE)) return 0;
        if (fail_result && result_n == 7) return 0;
        return result_pos < result_n ? MSR_RQM | MSR_DIO : MSR_RQM;
    }
    if (port == FDC_FIFO) { assert(result_pos < result_n); return result[result_pos++]; }
    if (port == 0x08) { int tc = terminal_count; terminal_count = 0; return tc ? 4 : 0; }
    return 0;
}
static void reset_test(void) {
    now = 0; reset_count = recal_count = seek_count = transfers = 0;
    fail_transfer = fail_command_byte = fail_result = bad_status = no_terminal_count = 0;
    command_n = result_n = result_pos = terminal_count = 0;
    fdc_ready = motor_running = 0; dma_masked = 1;
    disk_configure(36);
}
int main(void) {
    unsigned char data[512];
    reset_test(); fail_transfer = 1;
    assert(disk_read(1000, data, 1) == 0);
    assert(transfers == 2 && reset_count == 2 && recal_count == 2 && seek_count == 2);
    assert(data[0] == 0xa7 && data[511] == 0xa7 && dma_masked && !motor_running);
    for (int byte = 1; byte < 9; ++byte) {
        reset_test(); fail_command_byte = byte;
        assert(disk_write(1000, data, 1) < 0);
        assert(reset_count == 3 && dma_masked && !motor_running && !fdc_ready);
    }
    reset_test(); fail_result = 1;
    assert(disk_read(1000, data, 1) < 0 && dma_masked);
    reset_test(); bad_status = 1;
    assert(disk_read(1000, data, 1) < 0 && dma_masked);
    reset_test(); no_terminal_count = 1;
    assert(disk_read(1000, data, 1) < 0 && dma_masked);
    reset_test();
    assert(disk_read(~0u, data, 1) < 0);
    assert(disk_read(DISK_SECTORS - 1, data, 2) < 0);
    assert(disk_read(1, data, -1) < 0);
    assert(disk_read(1, 0, 1) < 0);
    assert(transfers == 0);
    puts("floppy: reset/reseek retry, every command-byte timeout, result timeout, status errors, DMA completion and bounds passed");
}
