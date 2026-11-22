#define PC_LD_INTERP 1
#include "ld.c"

#define AT_NULL_V 0
#define AT_PHDR_V 3
#define AT_PHENT_V 4
#define AT_PHNUM_V 5
#define AT_BASE_V 7
#define AT_ENTRY_V 9
#define PT_PHDR_V 6

static void register_interp(uint64_t interp_base)
{
  const Ehdr *eh = &__ehdr_start;
  const Phdr *ph = (const Phdr *)((const uint8_t *)eh + eh->phoff);
  struct obj *self = ld_alloc(sizeof(struct obj));
  if (!self)
    fatal("out of memory", 0);
  memset(self, 0, sizeof(*self));
  self->base = interp_base;
  self->path = "/lib/ld-purec.so.1";
  self->phdr = ph;
  self->phnum = eh->phnum;
  self->dyn = _DYNAMIC;
  self->pinned = 1;
  self->refs = 1;
  self->relocated = 1;
  self->inited = 1;
  self->lo = interp_base;
  self->hi = interp_base;
  for (uint16_t i = 0; i < eh->phnum; i++)
    if (ph[i].type == PT_LOAD && interp_base + ph[i].vaddr + ph[i].memsz > self->hi)
      self->hi = interp_base + ph[i].vaddr + ph[i].memsz;
  parse_dynamic(self);
  link_obj(self);
  global_add(self);
}

HIDDEN uint64_t pc_interp_start(uint64_t *frame)
{
  uint64_t self_base = (uint64_t)&__ehdr_start;
  const Dyn *sd = _DYNAMIC;
  const Rela *rela = 0;
  uint64_t relasz = 0;
  for (; sd->tag != DT_NULL; sd++)
  {
    if (sd->tag == DT_RELA)
      rela = (const Rela *)(self_base + sd->val);
    else if (sd->tag == DT_RELASZ)
      relasz = sd->val;
  }
  for (uint64_t i = 0; i < relasz / sizeof(Rela); i++)
    if ((uint32_t)(rela[i].info & 0xffffffffu) == R_RELATIVE)
      *(uint64_t *)(self_base + rela[i].offset) = self_base + (uint64_t)rela[i].addend;

  uint64_t argc = frame[1];
  const uint64_t *p = frame + 2 + argc + 1;
  while (*p)
    p++;
  p++;
  uint64_t at_phdr = 0, at_phnum = 0, at_base = self_base, at_entry = 0;
  for (; p[0] != AT_NULL_V; p += 2)
  {
    switch (p[0])
    {
    case AT_PHDR_V: at_phdr = p[1]; break;
    case AT_PHNUM_V: at_phnum = p[1]; break;
    case AT_BASE_V: at_base = p[1]; break;
    case AT_ENTRY_V: at_entry = p[1]; break;
    default: break;
    }
  }
  if (!at_phdr || !at_phnum || !at_entry)
    fatal("missing auxiliary vector entries", 0);
  const Phdr *ph = (const Phdr *)at_phdr;
  uint64_t base = 0;
  int have_base = 0;
  for (uint64_t i = 0; i < at_phnum; i++)
    if (ph[i].type == PT_PHDR_V)
    {
      base = at_phdr - ph[i].vaddr;
      have_base = 1;
      break;
    }
  if (!have_base)
    for (uint64_t i = 0; i < at_phnum; i++)
      if (ph[i].type == PT_LOAD && ph[i].offset == 0)
      {
        base = at_phdr - sizeof(Ehdr) - ph[i].vaddr;
        break;
      }
  const Dyn *dyn = 0;
  for (uint64_t i = 0; i < at_phnum; i++)
    if (ph[i].type == PT_DYNAMIC)
      dyn = (const Dyn *)(base + ph[i].vaddr);
  if (!dyn)
    fatal("program has no dynamic section", 0);
  return dl_main(frame, ph, (uint16_t)at_phnum, base, dyn, at_entry, at_base);
}
