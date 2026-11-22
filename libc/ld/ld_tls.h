#ifndef LD_TLS_H
#define LD_TLS_H

#include <stddef.h>
#include <stdint.h>

#define LD_TLS_MAXMOD 256
#define LD_TLS_SURPLUS 2048
#define LD_TLS_DTV_INLINE 16

size_t ld_tls_register(const void *image, size_t filesz, size_t memsz, size_t align);
void ld_tls_release(size_t modid);
int ld_tls_finalize(const char **err);
void ld_tls_set_threaded_flag(volatile int *flag);
void ld_tls_load_images(void);
int ld_tls_reloc(uint32_t type, size_t modid, uint64_t symoff, int64_t addend,
                 void *where, const char **err);

#endif
