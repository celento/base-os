#ifndef BASEOS_PROVISION_H
#define BASEOS_PROVISION_H
/* Seed missing default files after storage mounts. `scratch` is caller-owned
 * working memory of at least 64 KiB; nothing in it survives the call. */
#define PROVISION_SCRATCH_BYTES 65536u
void provision_system_files(char *scratch, unsigned capacity);
#endif
