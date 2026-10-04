#ifndef WRITER_CODEC_H
#define WRITER_CODEC_H

/* Independent of the kernel's Writer arena size in layout.h. */
#define WRITER_TEXT_MAX 32768u
#define WRITER_PLAIN_MAX_INPUT (2u * WRITER_TEXT_MAX)
#define WRITER_STYLE_BOLD 1u
#define WRITER_STYLE_ITALIC 2u
#define WRITER_STYLE_UNDERLINE 4u
#define WRITER_STYLE_MASK 7u
#define WRITER_ALIGN_LEFT 0u
#define WRITER_ALIGN_CENTER 1u
#define WRITER_ALIGN_RIGHT 2u
#define WRITER_ALIGN_MASK 3u
#define WRITER_PARAGRAPH_HEADING 4u
#define WRITER_PARAGRAPH_MASK 7u
#define WRITER_NATIVE_HEADER_SIZE 16u
#define WRITER_NATIVE_MAX_SIZE (WRITER_NATIVE_HEADER_SIZE + 3u * WRITER_TEXT_MAX + 2u)

typedef struct {
    unsigned length;
    unsigned char text[WRITER_TEXT_MAX];
    unsigned char style[WRITER_TEXT_MAX + 1u];
    unsigned char paragraph[WRITER_TEXT_MAX + 1u];
} WriterDoc;

/* Text is printable ASCII, TAB or LF; no terminator is stored. style[length]
 * is the insertion style. Paragraphs start at zero and immediately after LF.
 * paragraph[] must be zero at non-start positions through length. Array bytes
 * beyond the logical document are ignored. Initialization clears all arrays. */
void writer_doc_init(WriterDoc *doc);
int writer_doc_validate(const WriterDoc *doc);

/* All int functions return zero on success, -1 on invalid input/capacity.
 * Import/decode validate the complete input before changing doc. Sources and
 * destinations must not overlap; overlapping ranges are rejected. A NULL input
 * is valid only for an empty plain import. CRLF pairs normalize to LF; lone CR
 * is rejected. Plain input may be up to WRITER_PLAIN_MAX_INPUT bytes provided
 * the complete normalized result fits WRITER_TEXT_MAX. */
int writer_plain_import(WriterDoc *doc, const unsigned char *data, unsigned length);
int writer_native_decode(WriterDoc *doc, const unsigned char *data, unsigned length);

/* Encode/export validate and measure before writing. NULL out with capacity=0
 * queries the exact required byte count. Output has no NUL terminator. Failure
 * leaves out and *written unchanged. written is required and must not alias doc
 * or out. Input/doc/output must remain stable throughout the call. */
int writer_native_encode(const WriterDoc *doc, unsigned char *out,
                         unsigned capacity, unsigned *written);
int writer_rtf_export(const WriterDoc *doc, unsigned char *out,
                      unsigned capacity, unsigned *written);

#endif
