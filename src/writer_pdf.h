#ifndef BASEOS_WRITER_PDF_H
#define BASEOS_WRITER_PDF_H
#include "writer_codec.h"

#define WRITER_PDF_LETTER 0u
#define WRITER_PDF_A4 1u
#define WRITER_PDF_OK 0
#define WRITER_PDF_INVALID (-1)
#define WRITER_PDF_CAPACITY (-2)

/* Portable PDF 1.4 export, independent of Writer's screen and filesystem.
 * Letter (612x792 pt) or A4 (595.276x841.890 pt), 72 pt margins; Times standard
 * fonts, 12/18 pt body/heading, 16/24 pt leading. See docs/WRITER_PDF.md.
 * No heap, arena scratch, mutable globals, floating point or font embedding.
 *
 * Query exact bytes and page count with out=NULL, capacity=0. Otherwise writes
 * the complete PDF without a NUL terminator. written and pages are required.
 * Failure leaves doc, output, written and pages unchanged. All supplied ranges
 * must be valid, stable and nonoverlapping for the whole call. platform_poll()
 * services devices, and must not dispatch application actions or mutate input.
 *
 * INVALID: bad document/paper/arguments/overlap. CAPACITY: valid export does not
 * fit capacity. The caller must measure before creating a destination file and
 * also check its mounted filesystem limits; this module never touches files or
 * native binding/dirty/history state. CPU and memory bounds do not depend on
 * output capacity. Dense alternating styling may exceed Writer's 512 KiB buffer.
 */
int writer_pdf_export(const WriterDoc *doc, unsigned paper,
                      unsigned char *out, unsigned capacity,
                      unsigned *written, unsigned *pages);
#endif
