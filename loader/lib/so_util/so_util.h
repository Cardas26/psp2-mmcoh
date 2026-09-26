#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <psp2common/types.h>
#include "elf.h"

#define MAX_DATA_SEG 4

#define USE_KUBRIDGE
#define HAVE_VITAGL

#ifndef USE_KUBRIDGE
extern SceUID vm_blk;
#endif

typedef struct so_module {
	struct so_module *next;

	SceUID patch_blockid, text_blockid, data_blockid[MAX_DATA_SEG];
	uintptr_t patch_base, patch_head, cave_base, cave_head, text_base, plt_base;
	uintptr_t load_addr, data_base[MAX_DATA_SEG], exidx_base;
	size_t patch_size, cave_size, text_size, data_size[MAX_DATA_SEG], plt_size, exidx_size;
	int n_data;

	Elf32_Ehdr *ehdr;
	Elf32_Phdr *phdr;
	Elf32_Shdr *shdr;

	Elf32_Dyn *dynamic;
	Elf32_Sym *dynsym;
	Elf32_Rel *reldyn;
	Elf32_Rel *relplt;

	void (** init_array)(void);
	uint32_t *hash;

	uint32_t num_dynamic;
	uint32_t num_dynsym;
	uint32_t num_reldyn;
	uint32_t num_relplt;
	uint32_t num_init_array;

	char *soname;
	char *shstr;
	char *dynstr;
} so_module;

typedef struct {
	uintptr_t addr;
	uintptr_t thumb_addr;
	uint32_t orig_instr[2];
	uint32_t patch_instr[2];
	uintptr_t pre_addr;
	uint16_t pre_instr;
} so_hook;

typedef struct {
	const char *symbol;
	uintptr_t func;
} so_default_dynlib;

so_hook hook_thumb(uintptr_t addr, uintptr_t dst);

so_hook hook_arm(uintptr_t addr, uintptr_t dst);

so_hook hook_addr(uintptr_t addr, uintptr_t dst);

void so_flush_caches(const so_module *mod);

int so_file_load(so_module *mod, const char *filename, uintptr_t load_addr);

int so_mem_load(so_module *mod, const void *buffer, size_t so_size, uintptr_t load_addr);

int so_relocate(const so_module *mod);

int so_nop_calls(const so_module *mod, uintptr_t *addresses, int addresses_num);

int so_resolve(const so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only);

int so_resolve_with_dummy(const so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only);

void so_symbol_fix_ldmia(so_module *mod, const char *symbol);

void so_initialize(const so_module *mod);

void so_finalize(const so_module *mod);

uintptr_t so_symbol(const so_module *mod, const char *symbol);

uintptr_t so_trampoline_symbol(const so_module *mod, const char *symbol);

#define SO_HOOK_NOP     0xbf00
#define SO_HOOK_SITE(h) ((void *)(h.pre_addr ? h.pre_addr : h.addr))
#define SO_HOOK_LEN(h)  (sizeof(h.orig_instr) + (h.pre_addr ? 2 : 0))

#ifdef USE_KUBRIDGE
#define SO_CONTINUE(type, h, ...) ({ \
	if (h.pre_addr) *(uint16_t *)h.pre_addr = h.pre_instr; \
	sceClibMemcpy((void *)h.addr, h.orig_instr, sizeof(h.orig_instr)); \
	kuKernelFlushCaches(SO_HOOK_SITE(h), SO_HOOK_LEN(h)); \
	type r = h.thumb_addr ? ((type(*)())h.thumb_addr)(__VA_ARGS__) : ((type(*)())h.addr)(__VA_ARGS__); \
	if (h.pre_addr) *(uint16_t *)h.pre_addr = SO_HOOK_NOP; \
	sceClibMemcpy((void *)h.addr, h.patch_instr, sizeof(h.patch_instr)); \
	kuKernelFlushCaches(SO_HOOK_SITE(h), SO_HOOK_LEN(h)); \
	r; \
})
#else
#define SO_CONTINUE(type, h, ...) ({ \
	if (h.pre_addr) *(uint16_t *)h.pre_addr = h.pre_instr; \
	sceClibMemcpy((void *)h.addr, h.orig_instr, sizeof(h.orig_instr)); \
	sceKernelSyncVMDomain(vm_blk, SO_HOOK_SITE(h), SO_HOOK_LEN(h)); \
	type r = h.thumb_addr ? ((type(*)())h.thumb_addr)(__VA_ARGS__) : ((type(*)())h.addr)(__VA_ARGS__); \
	if (h.pre_addr) *(uint16_t *)h.pre_addr = SO_HOOK_NOP; \
	sceClibMemcpy((void *)h.addr, h.patch_instr, sizeof(h.patch_instr)); \
	sceKernelSyncVMDomain(vm_blk, SO_HOOK_SITE(h), SO_HOOK_LEN(h)); \
	r; \
})
#endif

#endif
