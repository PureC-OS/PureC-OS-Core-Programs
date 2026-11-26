#include "../include/purec/sysasm.h"
#include "../include/purec/abi/syscall_nr.h"
#include <stddef.h>
#include <stdint.h>
#include "ld_tls.h"
#include "../pthread_impl.h"

#define EXPORT __attribute__((visibility("default")))
#define TLS_PAGE 4096ULL
#define TLS_R_DTPMOD64 16
#define TLS_R_DTPOFF64 17
#define TLS_R_TPOFF64 18
#define TLS_R_TPOFF32 23

struct tls_index {
  uint64_t module;
  uint64_t offset;
};

struct tls_mod {
  const void *image;
  size_t filesz;
  size_t memsz;
  size_t align;
  size_t offset;
  uint8_t used;
  uint8_t is_static;
  uint8_t pending;
};

static volatile int *threaded_flag;

void ld_tls_set_threaded_flag(volatile int *flag)
{
  threaded_flag = flag;
}

static struct tls_mod mods[LD_TLS_MAXMOD + 1];
static size_t mod_count;
static size_t static_used;
static size_t static_limit;
static size_t static_align = 64;
static int finalized;
static size_t dtv_cap = LD_TLS_DTV_INLINE;

static int64_t tls_sys3(uint64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    return __pc_syscall6(number, a, b, c, 0, 0, 0);
}

static size_t round_up(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

static void tls_copy(void *dst, const void *src, size_t n)
{
  uint8_t *d = dst;
  const uint8_t *s = src;
  while (n--)
    *d++ = *s++;
}

static void *map_pages(size_t size)
{
  uint64_t req[4] = {0, round_up(size, TLS_PAGE), 3, 0};
  int64_t base = tls_sys3(SYS_MMAP, (uint64_t)req, 0, 0);
  return base < 0 ? 0 : (void *)base;
}

static void unmap_pages(void *base, size_t size)
{
  tls_sys3(SYS_MUNMAP, (uint64_t)base, round_up(size, TLS_PAGE), 0);
}

static uint8_t *current_tp(void)
{
  uint8_t *tp;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
  return tp;
}

static size_t tcb_size(void)
{
  return round_up(sizeof(struct pthread), 64);
}

static size_t block_size(void)
{
  return static_limit + tcb_size() + (dtv_cap + 1) * sizeof(size_t);
}

static size_t *inline_dtv(uint8_t *tp)
{
  return (size_t *)(tp + tcb_size());
}

size_t ld_tls_register(const void *image, size_t filesz, size_t memsz, size_t align)
{
  if (!align)
    align = 1;
  if (align > TLS_PAGE || mod_count >= LD_TLS_MAXMOD)
    return 0;
  size_t id = ++mod_count;
  mods[id].image = image;
  mods[id].filesz = filesz;
  mods[id].memsz = memsz;
  mods[id].align = align;
  mods[id].offset = 0;
  mods[id].used = 1;
  mods[id].is_static = 0;
  mods[id].pending = 0;
  if (!finalized && align > static_align)
    static_align = align;
  return id;
}

void ld_tls_release(size_t modid)
{
  if (modid && modid <= mod_count)
    mods[modid].used = 0;
}

static void install_images(uint8_t *tp, int only_pending)
{
  size_t *dtv = ((size_t **)tp)[1];
  for (size_t id = 1; id <= mod_count; id++)
  {
    struct tls_mod *m = &mods[id];
    if (!m->used || !m->is_static)
      continue;
    if (only_pending && !m->pending)
      continue;
    uint8_t *block = tp - m->offset;
    if (m->image && m->filesz)
      tls_copy(block, m->image, m->filesz);
    if (id <= dtv[0])
      dtv[id] = (size_t)block;
  }
}

static uint8_t *setup_block(uint8_t *base, int copy)
{
  uint8_t *tp = base + static_limit;
  ((void **)tp)[0] = tp;
  ((struct pthread *)tp)->stack_guard = 0x5C0FFEE5A17ULL;
  size_t *dtv = inline_dtv(tp);
  dtv[0] = dtv_cap;
  ((size_t **)tp)[1] = dtv;
  if (copy)
    install_images(tp, 0);
  else
    for (size_t id = 1; id <= mod_count; id++)
      if (mods[id].used && mods[id].is_static)
        dtv[id] = (size_t)(tp - mods[id].offset);
  return tp;
}

int ld_tls_finalize(const char **err)
{
  size_t off = 0;
  for (size_t id = 1; id <= mod_count; id++)
  {
    struct tls_mod *m = &mods[id];
    if (!m->used)
      continue;
    off = round_up(off + m->memsz, m->align);
    m->offset = off;
    m->is_static = 1;
    m->pending = 1;
  }
  static_used = off;
  if (mod_count + 8 > dtv_cap)
    dtv_cap = mod_count + 8;
  static_limit = round_up(off + LD_TLS_SURPLUS, static_align);
  finalized = 1;
  uint8_t *base = map_pages(block_size());
  if (!base)
  {
    *err = "cannot allocate thread-local storage";
    return -1;
  }
  uint8_t *tp = setup_block(base, 0);
  if (tls_sys3(SYS_SET_FS_BASE, (uint64_t)tp, 0, 0) < 0)
  {
    *err = "cannot set thread pointer";
    return -1;
  }
  return 0;
}

void ld_tls_load_images(void)
{
  if (!finalized)
    return;
  install_images(current_tp(), 1);
  for (size_t id = 1; id <= mod_count; id++)
    mods[id].pending = 0;
}

static int static_offset(size_t id, size_t *out, const char **err)
{
  if (!id || id > mod_count || !mods[id].used)
  {
    *err = "thread-local relocation against a module without TLS";
    return -1;
  }
  struct tls_mod *m = &mods[id];
  if (m->is_static)
  {
    *out = m->offset;
    return 0;
  }
  if (m->align > static_align)
  {
    *err = "static TLS alignment exceeds reserved block";
    return -1;
  }
  size_t off = round_up(static_used + m->memsz, m->align);
  if (off > static_limit)
  {
    *err = "cannot allocate memory in static TLS block";
    return -1;
  }
  if (m->filesz && threaded_flag && *threaded_flag)
  {
    *err = "static TLS with initialized data after threads were created";
    return -1;
  }
  m->offset = off;
  m->is_static = 1;
  m->pending = 1;
  static_used = off;
  *out = off;
  return 0;
}

int ld_tls_reloc(uint32_t type, size_t modid, uint64_t symoff, int64_t addend,
                 void *where, const char **err)
{
  uint64_t value = symoff + (uint64_t)addend;
  if (type == TLS_R_DTPMOD64)
  {
    if (!modid || modid > mod_count)
    {
      *err = "thread-local relocation against a module without TLS";
      return -1;
    }
    *(uint64_t *)where = modid;
    return 0;
  }
  if (type == TLS_R_DTPOFF64)
  {
    *(uint64_t *)where = value;
    return 0;
  }
  size_t off;
  if (static_offset(modid, &off, err) < 0)
    return -1;
  if (type == TLS_R_TPOFF32)
    *(int32_t *)where = (int32_t)(value - off);
  else
    *(uint64_t *)where = value - off;
  return 0;
}

static size_t *grow_dtv(uint8_t *tp)
{
  size_t *old = ((size_t **)tp)[1];
  size_t *grown = map_pages((LD_TLS_MAXMOD + 9) * sizeof(size_t));
  if (!grown)
    return 0;
  for (size_t i = 0; i <= old[0]; i++)
    grown[i] = old[i];
  grown[0] = LD_TLS_MAXMOD + 8;
  ((size_t **)tp)[1] = grown;
  if (old != inline_dtv(tp))
    unmap_pages(old, (LD_TLS_MAXMOD + 9) * sizeof(size_t));
  return grown;
}

EXPORT void *__tls_get_addr(struct tls_index *ti)
{
  uint8_t *tp = current_tp();
  size_t *dtv = ((size_t **)tp)[1];
  size_t id = ti->module;
  if (!id || id > mod_count)
    return 0;
  if (id > dtv[0])
  {
    dtv = grow_dtv(tp);
    if (!dtv)
      return 0;
  }
  uint8_t *block = (uint8_t *)dtv[id];
  if (!block)
  {
    struct tls_mod *m = &mods[id];
    block = map_pages(m->memsz ? m->memsz : 1);
    if (!block)
      return 0;
    if (m->image && m->filesz)
      tls_copy(block, m->image, m->filesz);
    dtv[id] = (size_t)block;
  }
  return block + ti->offset;
}

EXPORT size_t __ld_tls_block_size(void)
{
  return finalized ? block_size() : 0;
}

EXPORT void *__ld_tls_setup(void *base)
{
  return setup_block(base, 1);
}

EXPORT void *__ld_tls_main_tp(void)
{
  return finalized ? current_tp() : 0;
}

EXPORT void __ld_tls_teardown(void *pointer)
{
  uint8_t *tp = pointer;
  size_t *dtv = ((size_t **)tp)[1];
  if (!dtv)
    return;
  uint8_t *lo = tp - static_limit;
  for (size_t id = 1; id <= dtv[0] && id <= mod_count; id++)
  {
    uint8_t *block = (uint8_t *)dtv[id];
    if (block && (block < lo || block >= tp))
      unmap_pages(block, mods[id].memsz ? mods[id].memsz : 1);
    dtv[id] = 0;
  }
  if (dtv != inline_dtv(tp))
    unmap_pages(dtv, (LD_TLS_MAXMOD + 9) * sizeof(size_t));
  ((size_t **)tp)[1] = inline_dtv(tp);
  inline_dtv(tp)[0] = 0;
}
