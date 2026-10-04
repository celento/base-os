#ifndef BASEOS_EXECUTABLE_H
#define BASEOS_EXECUTABLE_H
/* On-disk executable definitions only. These constants do not advertise kernel
 * support. BEX1 remains unchanged; BEX2 requires a separately enabled loader.
 * Words are little-endian and all application addresses are segment offsets. */
#define BOS_BEX1_MAGIC 0x31584542u
#define BOS_BEX2_MAGIC 0x32584542u
#define BOS_BEX2_HEADER_BYTES 64u
#define BOS_BEX2_FORMAT_VERSION 1u
#define BOS_BEX2_PAGE_BYTES 4096u
#define BOS_BEX2_VIRTUAL_BYTES 4194304u
#define BOS_BEX2_FILE_MAX 262144u
#define BOS_BEX2_STACK_MIN 16384u
#define BOS_BEX2_STACK_MAX 262144u
#define BOS_BEX2_STACK_DEFAULT 65536u
#define BOS_BEX2_TABLE_PAGES 2u
#define BOS_EXECUTABLE_BEX1 1u
#define BOS_EXECUTABLE_BEX2 2u

typedef struct {
    unsigned int magic, header_bytes, format_version, flags;
    unsigned int file_bytes, entry_offset, text_bytes, data_offset;
    unsigned int data_file_bytes, data_mem_bytes, workspace_bytes, stack_bytes;
    unsigned int required_abi_major, required_abi_minor, reserved0, reserved1;
} BosBex2Header;
_Static_assert(sizeof(unsigned int)==4 && sizeof(BosBex2Header)==64,
               "BEX2 header has sixteen 32-bit words");
#endif
