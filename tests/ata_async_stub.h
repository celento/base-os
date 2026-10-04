/* Deterministic bounded adapter for FS-only host tests. The real controller
 * state machine has separate port-level tests in ata_host.c. */
#ifndef TEST_ATA_ASYNC_STUB_H
#define TEST_ATA_ASYNC_STUB_H
#ifndef TEST_ATA_ASYNC_BEGIN
#define TEST_ATA_ASYNC_BEGIN(operation, lba, sectors) 0
#endif
#ifndef TEST_ATA_ASYNC_POLL
#define TEST_ATA_ASYNC_POLL(operation, lba, sectors) 0
#endif
#ifndef TEST_ATA_ASYNC_AFTER
#define TEST_ATA_ASYNC_AFTER(operation, lba, buffer, sectors) ((void)0)
#endif
static struct {
    unsigned lba, remaining;
    unsigned char *buffer;
    int operation, active;
    enum AtaProgress result;
} test_ata_request;
static int test_ata_begin(int operation, unsigned lba, void *buffer, int sectors) {
    if (test_ata_request.active || sectors < 0 || lba > ata_sector_count() ||
        (unsigned)sectors > ata_sector_count() - lba || (sectors && !buffer)) return -1;
    if (TEST_ATA_ASYNC_BEGIN(operation, lba, sectors)) return -1;
    test_ata_request.lba = lba; test_ata_request.remaining = sectors;
    test_ata_request.buffer = buffer; test_ata_request.operation = operation;
    test_ata_request.active = 1; test_ata_request.result = ATA_PROGRESS_MORE;
    return 0;
}
int ata_request_read(unsigned lba, void *buffer, int sectors) {
    return test_ata_begin(0, lba, buffer, sectors);
}
int ata_request_write(unsigned lba, const void *buffer, int sectors) {
    return test_ata_begin(1, lba, (void *)buffer, sectors);
}
int ata_request_flush(void) { return test_ata_begin(2, 0, 0, 0); }
int ata_request_active(void) { return test_ata_request.active; }
int ata_ready(void) { return 1; }
enum AtaProgress ata_poll(unsigned budget) {
    if (!test_ata_request.active) return test_ata_request.result;
    unsigned count = test_ata_request.remaining;
    if (count > budget) count = budget;
    int result = TEST_ATA_ASYNC_POLL(test_ata_request.operation, test_ata_request.lba, count);
    if (result > 0) return ATA_PROGRESS_WAIT;
    if (result < 0) {
        test_ata_request.active = 0; test_ata_request.result = ATA_PROGRESS_ERROR;
        return ATA_PROGRESS_ERROR;
    }
    if (test_ata_request.operation == 2) result = ata_flush();
    else if (count) {
        result = test_ata_request.operation ?
            ata_write(test_ata_request.lba, test_ata_request.buffer, count) :
            ata_read(test_ata_request.lba, test_ata_request.buffer, count);
        if (!result) TEST_ATA_ASYNC_AFTER(test_ata_request.operation, test_ata_request.lba, test_ata_request.buffer, count);
        test_ata_request.lba += count; test_ata_request.buffer += count * SECTOR_SIZE;
        test_ata_request.remaining -= count;
    }
    if (result < 0 || !test_ata_request.remaining) {
        test_ata_request.active = 0;
        test_ata_request.result = result < 0 ? ATA_PROGRESS_ERROR : ATA_PROGRESS_DONE;
    }
    return test_ata_request.result;
}
#endif
