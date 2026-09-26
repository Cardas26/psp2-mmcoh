/* so_util.c — utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include "so_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitasdk.h>

#include "utils/filelog.h"

#ifdef HAVE_VITAGL
#include <vitaGL.h>
#endif

#ifdef USE_KUBRIDGE
#include <kubridge.h>
#else
#define VM_BLK_SIZE (16 * 1024 * 1024)
SceUID vm_blk = 0;
static void *vm_ptr = NULL;
static uintptr_t vm_avail_addr = 0;
#endif

#ifndef ZERO_INTERFERENCE
static void so_util_log_critical(const char *fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	sceClibPrintf("%s", buf);
	file_log_write(buf);
}
#endif

#ifndef ZERO_INTERFERENCE
#define SO_UTIL_LOG_CRITICAL so_util_log_critical
#else
#define SO_UTIL_LOG_CRITICAL(...) ((void)0)
#endif
#ifdef SO_UTIL_VERBOSE
#define SO_UTIL_LOG_VERBOSE sceClibPrintf
#else
#define SO_UTIL_LOG_VERBOSE
#endif

typedef struct b_enc {
	union {
		struct __attribute__((__packed__)) {
			int imm24: 24;
			unsigned int l: 1;
			unsigned int enc: 3;
			unsigned int cond: 4;
		} bits;
		uint32_t raw;
	};
} b_enc;

typedef struct ldst_enc {
	union {
		struct __attribute__((__packed__)) {
			int imm12: 12;
			unsigned int rt: 4;
			unsigned int rn: 4;
			unsigned int bit20_1: 1;
			unsigned int w: 1;
			unsigned int b: 1;
			unsigned int u: 1;
			unsigned int p: 1;
			unsigned int enc: 3;
			unsigned int cond: 4;
		} bits;
		uint32_t raw;
	};
} ldst_enc;

#define B_RANGE ((1 << 24) - 1)
#define B_OFFSET(x) (x + 8)
#define B(PC, DEST) ((b_enc){.bits = {.cond = 0b1110, .enc = 0b101, .l = 0, .imm24 = (((intptr_t)DEST-(intptr_t)PC) / 4) - 2}})
#define LDR_OFFS(RT, RN, IMM) ((ldst_enc){.bits = {.cond = 0b1110, .enc = 0b010, .p = 1, .u = (IMM >= 0), .b = 0, .w = 0, .bit20_1 = 1, .rn = RN, .rt = RT, .imm12 = (IMM >= 0) ? IMM : -IMM}})

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define PATCH_SZ 0x10000
static so_module *head = NULL, *tail = NULL;

static int _so_util_ret0(void) {
	return 0;
}

so_hook hook_thumb(uintptr_t addr, uintptr_t dst) {
	so_hook h = {0};
	SO_UTIL_LOG_VERBOSE("THUMB HOOK\n");
	if (addr == 0)
		return h;
	h.thumb_addr = addr;
	addr &= ~1;
	if (addr & 2) {
		h.pre_addr = addr;
		h.pre_instr = *(uint16_t *)addr;
		*(uint16_t *)addr = SO_HOOK_NOP;
		addr += 2;
		SO_UTIL_LOG_VERBOSE("THUMB UNALIGNED\n");
	}

	h.addr = addr;
	h.patch_instr[0] = 0xf000f8df;
	h.patch_instr[1] = dst;
	sceClibMemcpy(&h.orig_instr, (void *)addr, sizeof(h.orig_instr));
	sceClibMemcpy((void *)addr, h.patch_instr, sizeof(h.patch_instr));

	return h;
}

so_hook hook_arm(uintptr_t addr, uintptr_t dst) {
	SO_UTIL_LOG_VERBOSE("ARM HOOK\n");
	so_hook h = {0};
	if (addr == 0) {
		return h;
	}
	h.thumb_addr = 0;
	h.addr = addr;
	h.patch_instr[0] = 0xe51ff004;
	h.patch_instr[1] = dst;
	sceClibMemcpy(&h.orig_instr, (void *)addr, sizeof(h.orig_instr));
	sceClibMemcpy((void *)addr, h.patch_instr, sizeof(h.patch_instr));

	return h;
}

so_hook hook_addr(uintptr_t addr, uintptr_t dst) {
	if (addr & 1)
		return hook_thumb(addr, dst);
	else
		return hook_arm(addr, dst);
}

void so_flush_caches(const so_module *mod) {
	SO_UTIL_LOG_VERBOSE("Flushing cache on addr: %x size: %u\n", mod->text_base, mod->text_size);
#ifdef USE_KUBRIDGE
	kuKernelFlushCaches((void *)mod->text_base, mod->text_size);
#else
	sceKernelSyncVMDomain(vm_blk, (void *)mod->text_base, mod->text_size);
#endif
}

static int so_load_internal(so_module *mod, SceUID so_blockid, void *so_data, uintptr_t load_addr) {
	int res = 0;
	uintptr_t data_addr = load_addr;

	if (memcmp(so_data, ELFMAG, SELFMAG) != 0) {
		SO_UTIL_LOG_CRITICAL("so_load_internal: bad ELF magic (got %02x %02x %02x %02x, expected %02x %02x %02x %02x)\n",
			((unsigned char *)so_data)[0], ((unsigned char *)so_data)[1],
			((unsigned char *)so_data)[2], ((unsigned char *)so_data)[3],
			(unsigned char)ELFMAG[0], (unsigned char)ELFMAG[1],
			(unsigned char)ELFMAG[2], (unsigned char)ELFMAG[3]);
		res = -1;
		goto err_free_so;
	}

#ifndef USE_KUBRIDGE
	if (!vm_blk) {
		vm_blk = sceKernelAllocMemBlockForVM("so_blk", VM_BLK_SIZE);
		if (vm_blk < 0)
			goto err_free_so;
		sceKernelGetMemBlockBase(vm_blk, &vm_ptr);
		sceKernelOpenVMDomain();
		vm_avail_addr = (uintptr_t)vm_ptr;
	}
	load_addr = data_addr = (uintptr_t)vm_avail_addr + PATCH_SZ;
	SO_UTIL_LOG_CRITICAL("so block allocated (0x%08x) on address: 0x%08x (text: 0x%08x)\n", vm_blk, vm_ptr, load_addr);
#endif

	mod->ehdr = (Elf32_Ehdr *)so_data;
	mod->phdr = (Elf32_Phdr *)((uintptr_t)so_data + mod->ehdr->e_phoff);
	mod->shdr = (Elf32_Shdr *)((uintptr_t)so_data + mod->ehdr->e_shoff);
	mod->load_addr = load_addr;

	mod->shstr = (char *)((uintptr_t)so_data + mod->shdr[mod->ehdr->e_shstrndx].sh_offset);

	for (int i = 0; i < mod->ehdr->e_phnum; i++) {
		SO_UTIL_LOG_VERBOSE("segment %d: type: %x flags: %x\n", i, mod->phdr[i].p_type, mod->phdr[i].p_flags);

		if (mod->phdr[i].p_type == PT_LOAD) {
			void *prog_data;
			size_t prog_size;

			if ((mod->phdr[i].p_flags & PF_X) == PF_X) {
				mod->patch_size = ALIGN_MEM(PATCH_SZ, mod->phdr[i].p_align);
#ifndef USE_KUBRIDGE
				mod->patch_base = load_addr - mod->patch_size;
#else
				SceKernelAllocMemBlockKernelOpt opt;
				sceClibMemset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1;
				opt.field_C = (SceUInt32)load_addr - mod->patch_size;
				res = mod->patch_blockid = kuKernelAllocMemBlock("rwx_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mod->patch_size, &opt);
				if (res < 0)
					goto err_free_so;

				sceKernelGetMemBlockBase(mod->patch_blockid, (void **) &mod->patch_base);
				kuKernelMemProtect((void *) mod->patch_base, mod->patch_size, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
#endif
				mod->patch_head = mod->patch_base;

				prog_size = ALIGN_MEM(mod->phdr[i].p_memsz + mod->phdr[i].p_vaddr - (data_addr - load_addr), mod->phdr[i].p_align);

				mod->phdr[i].p_vaddr += (Elf32_Addr)load_addr;
				SO_UTIL_LOG_VERBOSE("Text segment: vaddr: %x (data addr: %x) size: %u\n", mod->phdr[i].p_vaddr, data_addr, mod->phdr[i].p_memsz);
#ifndef USE_KUBRIDGE
				mod->text_blockid = vm_blk;
				prog_data = (void *)data_addr;
#else
				sceClibMemset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1;
				opt.field_C = data_addr;
				res = mod->text_blockid = kuKernelAllocMemBlock("rwx_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, prog_size, &opt);
				if (res < 0)
					goto err_free_so;

				sceKernelGetMemBlockBase(mod->text_blockid, &prog_data);
				kuKernelMemProtect(prog_data, prog_size, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
#endif

				mod->text_base = mod->phdr[i].p_vaddr;
				mod->text_size = mod->phdr[i].p_memsz;

				mod->cave_size = ALIGN_MEM(prog_size - mod->phdr[i].p_memsz, 0x4);
				mod->cave_base = mod->cave_head = mod->phdr[i].p_vaddr + mod->phdr[i].p_memsz;
				mod->cave_base = ALIGN_MEM(mod->cave_base, 0x4);
				mod->cave_head = mod->cave_base;
				SO_UTIL_LOG_VERBOSE("code cave: %d bytes (@0x%08X).\n", mod->cave_size, mod->cave_base);

				data_addr = (uintptr_t)prog_data + prog_size;
			} else {
				if (mod->n_data >= MAX_DATA_SEG)
					goto err_free_data;

				prog_size = ALIGN_MEM(mod->phdr[i].p_memsz + mod->phdr[i].p_vaddr - (data_addr - load_addr), mod->phdr[i].p_align);

				mod->phdr[i].p_vaddr += load_addr;
				SO_UTIL_LOG_VERBOSE("Data segment: vaddr: %x (data addr: %x) size: %u\n", mod->phdr[i].p_vaddr, data_addr, mod->phdr[i].p_memsz);

#ifndef USE_KUBRIDGE
				prog_data = (void *)data_addr;
#else
				SceKernelAllocMemBlockKernelOpt opt = {0};
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1;
				opt.field_C = data_addr;
				res = mod->data_blockid[mod->n_data] = kuKernelAllocMemBlock("rw_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, prog_size, &opt);
				if (res < 0)
					goto err_free_text;

				sceKernelGetMemBlockBase(mod->data_blockid[mod->n_data], &prog_data);
#endif
				data_addr = (uintptr_t)prog_data + prog_size;

				mod->data_base[mod->n_data] = mod->phdr[i].p_vaddr;
				mod->data_size[mod->n_data] = mod->phdr[i].p_memsz;
				mod->n_data++;
			}

			sceClibMemset(prog_data + mod->phdr[i].p_filesz, 0, prog_size - mod->phdr[i].p_filesz);
			sceClibMemcpy((void *)mod->phdr[i].p_vaddr, (void *)((uintptr_t)so_data + mod->phdr[i].p_offset), mod->phdr[i].p_filesz);
		}
	}

	for (int i = 0; i < mod->ehdr->e_shnum; i++) {
		char *sh_name = mod->shstr + mod->shdr[i].sh_name;
		uintptr_t sh_addr = load_addr + mod->shdr[i].sh_addr;
		size_t sh_size = mod->shdr[i].sh_size;
		if (strcmp(sh_name, ".dynamic") == 0) {
			mod->dynamic = (Elf32_Dyn *)sh_addr;
			mod->num_dynamic = (uint32_t)(sh_size / sizeof(Elf32_Dyn));
		} else if (strcmp(sh_name, ".dynstr") == 0) {
			mod->dynstr = (char *)sh_addr;
		} else if (strcmp(sh_name, ".dynsym") == 0) {
			mod->dynsym = (Elf32_Sym *)sh_addr;
			mod->num_dynsym = (uint32_t)(sh_size / sizeof(Elf32_Sym));
		} else if (strcmp(sh_name, ".rel.dyn") == 0) {
			mod->reldyn = (Elf32_Rel *)sh_addr;
			mod->num_reldyn = (uint32_t)(sh_size / sizeof(Elf32_Rel));
		} else if (strcmp(sh_name, ".rel.plt") == 0) {
			mod->relplt = (Elf32_Rel *)sh_addr;
			mod->num_relplt = (uint32_t)(sh_size / sizeof(Elf32_Rel));
		} else if (strcmp(sh_name, ".init_array") == 0) {
			mod->init_array = (void (**)(void))sh_addr;
			mod->num_init_array = (uint32_t)(sh_size / sizeof(void *));
		} else if (strcmp(sh_name, ".hash") == 0) {
			mod->hash = (void *)sh_addr;
		} else if (strcmp(sh_name, ".plt") == 0) {
			mod->plt_base = sh_addr;
			mod->plt_size = sh_size;
		} else if (strcmp(sh_name, ".ARM.exidx") == 0) {
			mod->exidx_base = sh_addr;
			mod->exidx_size = sh_size;
		}
	}

	if (mod->dynamic == NULL || mod->dynstr == NULL || mod->dynsym == NULL ||
		mod->reldyn == NULL || mod->relplt == NULL) {
		res = -2;
		goto err_free_data;
	}

	for (uint32_t i = 0; i < mod->num_dynamic; i++) {
		switch (mod->dynamic[i].d_tag) {
		case DT_SONAME:
			mod->soname = mod->dynstr + mod->dynamic[i].d_un.d_ptr;
			break;
		default:
			break;
		}
	}

	sceKernelFreeMemBlock(so_blockid);

	if (!head && !tail) {
		head = mod;
		tail = mod;
	} else {
		tail->next = mod;
		tail = mod;
	}

#ifndef USE_KUBRIDGE
	vm_avail_addr = data_addr;
	SO_UTIL_LOG_CRITICAL("so loaded correctly. %u bytes left for more so files.\n", VM_BLK_SIZE - (vm_avail_addr - (uintptr_t)vm_ptr));
#endif

	return 0;

err_free_data:
#ifndef USE_KUBRIDGE
	for (int i = 0; i < mod->n_data; i++)
		sceKernelFreeMemBlock(mod->data_blockid[i]);
#endif
err_free_text:
	sceKernelFreeMemBlock(mod->text_blockid);
err_free_so:
	sceKernelFreeMemBlock(so_blockid);

	return res;
}

int so_mem_load(so_module *mod, const void *buffer, size_t so_size, uintptr_t load_addr) {
	memset(mod, 0, sizeof(so_module));

	SceUID so_blockid = sceKernelAllocMemBlock("so block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (so_size + 0xfff) & ~0xfff, NULL);
	if (so_blockid < 0)
		return so_blockid;

	void *so_data;
	sceKernelGetMemBlockBase(so_blockid, &so_data);
	sceClibMemcpy(so_data, buffer, so_size);

	return so_load_internal(mod, so_blockid, so_data, load_addr);
}

int so_file_load(so_module *mod, const char *filename, uintptr_t load_addr) {
	memset(mod, 0, sizeof(so_module));

	SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0);
	if (fd < 0) {
		SO_UTIL_LOG_CRITICAL("so_file_load: sceIoOpen(\"%s\") failed: 0x%08x\n", filename, fd);
		return fd;
	}

	size_t so_size = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	SO_UTIL_LOG_CRITICAL("so_file_load: \"%s\" is %u bytes\n", filename, (unsigned int)so_size);

	SceUID so_blockid = sceKernelAllocMemBlock("so block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (so_size + 0xfff) & ~0xfff, NULL);
	if (so_blockid < 0) {
		SO_UTIL_LOG_CRITICAL("so_file_load: sceKernelAllocMemBlock failed: 0x%08x\n", so_blockid);
		sceIoClose(fd);
		return so_blockid;
	}

	void *so_data;
	sceKernelGetMemBlockBase(so_blockid, &so_data);

	int nread = sceIoRead(fd, so_data, so_size);
	sceIoClose(fd);
	SO_UTIL_LOG_CRITICAL("so_file_load: sceIoRead returned %d (expected %u), first bytes: %02x %02x %02x %02x\n",
		nread, (unsigned int)so_size,
		((unsigned char *)so_data)[0], ((unsigned char *)so_data)[1],
		((unsigned char *)so_data)[2], ((unsigned char *)so_data)[3]);

	return so_load_internal(mod, so_blockid, so_data, load_addr);
}

void so_finalize(const so_module *mod) {
	for (int i = 0; i < mod->n_data; i++)
		sceKernelFreeMemBlock(mod->data_blockid[i]);
	sceKernelFreeMemBlock(mod->text_blockid);
}

int so_relocate(const so_module *mod) {
	for (uint32_t i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->load_addr + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
			if (sym->st_shndx != SHN_UNDEF) {
				*ptr += mod->load_addr + sym->st_value;
			}
			break;
		case R_ARM_RELATIVE:
			*ptr += mod->load_addr;
			break;
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx != SHN_UNDEF) {
				*ptr = mod->load_addr + sym->st_value;
			}
			break;
		}
		case R_ARM_NONE:
			break;
		default:
			SO_UTIL_LOG_CRITICAL("Error unknown relocation type %d\n", type);
			break;
		}
	}

	return 0;
}

uintptr_t so_resolve_link(const so_module *mod, const char *symbol) {
	for (uint32_t i = 0; i < mod->num_dynamic; i++) {
		if (mod->dynamic[i].d_tag == DT_NEEDED) {
			so_module *curr = head;
			while (curr) {
				if (curr != mod && strcmp(curr->soname, mod->dynstr + mod->dynamic[i].d_un.d_ptr) == 0) {
					uintptr_t link = so_symbol(curr, symbol);
					if (link)
						return link;
				}
				curr = curr->next;
			}
		}
	}

	return 0;
}

__attribute__((noreturn)) void reloc_err(uintptr_t got0)
{
	int found = 0;
	so_module *curr = head;
	while (curr && !found) {
		for (int i = 0; i < curr->n_data; i++)
			if ((got0 >= curr->data_base[i]) && (got0 <= (uintptr_t)(curr->data_base[i] + curr->data_size[i])))
				found = 1;

		if (!found)
			curr = curr->next;
	}

	if (curr) {
		for (uint32_t i = 0; i < curr->num_reldyn + curr->num_relplt; i++) {
			Elf32_Rel *rel = i < curr->num_reldyn ? &curr->reldyn[i] : &curr->relplt[i - curr->num_reldyn];
			Elf32_Sym *sym = &curr->dynsym[ELF32_R_SYM(rel->r_info)];
			uintptr_t *ptr = (uintptr_t *)(curr->load_addr + rel->r_offset);

			if (ELF32_R_TYPE(rel->r_info) == R_ARM_JUMP_SLOT && got0 == (uintptr_t)ptr) {
				SO_UTIL_LOG_CRITICAL("Unknown symbol \"%s\" (%p).\n", curr->dynstr + sym->st_name, (void*)got0);
				sceClibAbort();
				__builtin_unreachable();
			}
		}
	}

	SO_UTIL_LOG_CRITICAL("Unknown symbol \"???\" (%p).\n", (void*)got0);
	sceClibAbort();
	__builtin_unreachable();
}

__attribute__((naked)) void plt0_stub() {
	__asm__ volatile (
		"mov r0, r12\n"
		"b reloc_err\n"
	);
}

int so_nop_calls(const so_module *mod, uintptr_t *addresses, int addresses_num) {
	if (addresses_num == 0)
		return 0;

	for (int i = 0; i < addresses_num; i++) {
		addresses[i] &= ~1;
	}

	uintptr_t text_start = mod->text_base;
	uintptr_t text_end   = mod->text_base + mod->text_size;
	int count = 0;

	for (uintptr_t a = text_start; a + 4 <= text_end; a += 4) {
		uint32_t raw_instr = *(uint32_t *)a;

		int is_bl  = ((raw_instr & 0xFF000000) == 0xEB000000);
		int is_blx = ((raw_instr & 0xFE000000) == 0xFA000000);

		if (is_bl || is_blx) {
			int32_t imm24 = (int32_t)(raw_instr & 0x00FFFFFF);

			if (imm24 & (1 << 23)) {
				imm24 |= ~((1 << 24) - 1);
			}

			uintptr_t dest;
			if (is_bl) {
				dest = (uintptr_t)((intptr_t)(a + 8) + (imm24 * 4));
			} else {
				int H = (raw_instr >> 24) & 1;
				dest = (uintptr_t)((intptr_t)(a + 8) + (imm24 * 4) + (H * 2));
			}

			dest &= ~1;
			for (int i = 0; i < addresses_num; i++) {
				if (addresses[i] == dest) {
					*(uint32_t *)a = 0xE320F000;
					count++;
					break;
				}
			}
		}
	}

	return count;
}

int so_resolve(const so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only) {
	for (uint32_t i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->load_addr + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx == SHN_UNDEF) {
				int resolved = 0;
				if (!default_dynlib_only) {
					uintptr_t link = so_resolve_link(mod, mod->dynstr + sym->st_name);
					if (link) {
						SO_UTIL_LOG_VERBOSE("Resolved from dependencies: %s\n", mod->dynstr + sym->st_name);
						if (type == R_ARM_ABS32)
							*ptr += link;
						else
							*ptr = link;
						resolved = 1;
					}
				}

				if (!resolved) {
					for (int j = 0; j < size_default_dynlib / (int)sizeof(so_default_dynlib); j++) {
						if (strcmp(mod->dynstr + sym->st_name, default_dynlib[j].symbol) == 0) {
							*ptr = default_dynlib[j].func;
							resolved = 1;
							break;
						}
					}
				}

#ifdef HAVE_VITAGL
				if (!resolved) {
					*ptr = (uintptr_t)vglGetProcAddress(mod->dynstr + sym->st_name);
					if (*ptr)
						resolved = 1;
				}
#endif

				if (!resolved) {
					if (type == R_ARM_JUMP_SLOT) {
						SO_UTIL_LOG_CRITICAL("Unresolved import: %s\n", mod->dynstr + sym->st_name);
						*ptr = (uintptr_t)&plt0_stub;
					}
					else {
						SO_UTIL_LOG_CRITICAL("Unresolved import: %s\n", mod->dynstr + sym->st_name);
					}
				}
			}

			break;
		}
		default:
			break;
		}
	}

	return 0;
}

int so_resolve_with_dummy(const so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only) {
	for (uint32_t i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->load_addr + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx == SHN_UNDEF) {
				for (int j = 0; j < size_default_dynlib / sizeof(so_default_dynlib); j++) {
					if (strcmp(mod->dynstr + sym->st_name, default_dynlib[j].symbol) == 0) {
						*ptr = (uintptr_t) &_so_util_ret0;
						break;
					}
				}
			}

			break;
		}
		default:
			break;
		}
	}

	return 0;
}

void so_initialize(const so_module *mod) {
	for (uint32_t i = 0; i < mod->num_init_array; i++) {
		if (mod->init_array[i] && mod->init_array[i] != (void (*)(void))-1)
			mod->init_array[i]();
	}
}

uint32_t so_hash(const uint8_t *name) {
	uint64_t h = 0, g;
	while (*name) {
		h = (h << 4) + *name++;
		if ((g = (h & 0xf0000000)) != 0)
			h ^= g >> 24;
		h &= 0x0fffffff;
	}
	return h;
}

static int so_symbol_index(const so_module *mod, const char *symbol)
{
	if (mod->hash) {
		uint32_t hash = so_hash((const uint8_t *)symbol);
		uint32_t nbucket = mod->hash[0];
		uint32_t *bucket = &mod->hash[2];
		uint32_t *chain = &bucket[nbucket];
		for (uint32_t i = bucket[hash % nbucket]; i; i = chain[i]) {
			if (mod->dynsym[i].st_shndx == SHN_UNDEF)
				continue;
			if (mod->dynsym[i].st_info != SHN_UNDEF && strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
				return (int)i;
		}
	}

	for (uint32_t i = 0; i < mod->num_dynsym; i++) {
		if (mod->dynsym[i].st_shndx == SHN_UNDEF)
			continue;
		if (mod->dynsym[i].st_info != SHN_UNDEF && strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
			return (int)i;
	}

	return -1;
}

static uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz) {
	#define inrange(lsr, gtr, range) \
		(((uintptr_t)(range) == (uintptr_t)NULL) || ((uintptr_t)(range) >= ((uintptr_t)(gtr) - (uintptr_t)(lsr))))
	#define blkavail(type) (so->type##_size - (so->type##_head - so->type##_base))

	sz = ALIGN_MEM(sz, 4);

	if (sz <= (blkavail(patch)) && inrange(so->patch_base, dst, range)) {
		so->patch_head += sz;
		return (so->patch_head - sz);
	} else if (sz <= (blkavail(cave)) && inrange(dst, so->cave_base, range)) {
		so->cave_head += sz;
		return (so->cave_head - sz);
	}

	return (uintptr_t)NULL;
}

static void trampoline_ldm(so_module *mod, uint32_t *dst) {
	uint32_t trampoline[1];
	uint32_t funct[20] = {0xFAFAFAFA};
	uint32_t *ptr = funct;

	int cur = 0;
	int baseReg = (int)(((*dst) >> 16) & 0xF);
	int bitMask = (int)((*dst) & 0xFFFF);

	uint32_t stored = (uint32_t) NULL;
	for (int i = 0; i < 16; i++) {
		if (bitMask & (1 << i)) {
			if (baseReg == i)
				stored = LDR_OFFS(i, baseReg, cur).raw;
			else
				*ptr++ = LDR_OFFS(i, baseReg, cur).raw;
			cur += 4;
		}
	}

	if (stored) {
		*ptr++ = stored;
	}

	*ptr++ = (uint32_t) 0xe51ff004;
	*ptr++ = (uint32_t) dst+1;

	size_t trampoline_sz =	((uintptr_t)ptr - (uintptr_t)&funct[0]);
	uintptr_t patch_addr = so_alloc_arena(mod, B_RANGE, (uintptr_t) B_OFFSET(dst), trampoline_sz);

	if (!patch_addr) {
		SO_UTIL_LOG_CRITICAL("Failed to patch LDMIA at %p, unable to allocate space.\n", dst);
		return;
	}

	trampoline[0] = B(dst, patch_addr).raw;

	sceClibMemcpy((void*)patch_addr, funct, trampoline_sz);
	sceClibMemcpy(dst, trampoline, sizeof(trampoline));
}

uintptr_t so_symbol(const so_module *mod, const char *symbol) {
	int index = so_symbol_index(mod, symbol);
	if (index == -1)
		return (uint32_t) NULL;

	return mod->load_addr + mod->dynsym[index].st_value;
}

uintptr_t so_trampoline_symbol(const so_module *mod, const char *symbol) {
	uintptr_t got_slot = 0;
	for (uint32_t i = 0; i < mod->num_relplt; i++) {
		Elf32_Rel *rel = &mod->relplt[i];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];

		if ((ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) || (sym->st_name == 0))
			continue;
		if (strcmp(mod->dynstr + sym->st_name, symbol) == 0) {
			got_slot = mod->load_addr + rel->r_offset;
			break;
		}
	}

	if (!got_slot)
		return 0;

	uintptr_t plt_start = mod->plt_base;
	uintptr_t plt_end   = (uintptr_t)(mod->plt_base + mod->plt_size);

	for (uintptr_t a = plt_start; a + 12 <= plt_end; a += 4) {
		uint32_t w0 = *(uint32_t *)a;
		uint32_t w1 = *(uint32_t *)(a + 4);
		uint32_t w2 = *(uint32_t *)(a + 8);

		if ((w2 >> 12) != 0xe5bcf)
			continue;

		#define ARM_ROTIMM(w) ({ \
			uint32_t _imm = (w) & 0xff; \
			uint32_t _rot = ((w) >> 8) & 0xf; \
			(_imm >> (_rot * 2)) | (_imm << (32 - _rot * 2)); \
		})

		uintptr_t PC = a + 8;
		uintptr_t R12 = PC + ARM_ROTIMM(w0);
		R12 += ARM_ROTIMM(w1);
		uint32_t delta = w2 & 0xfff;
		uintptr_t target = R12 + delta;

		if (target == got_slot)
			return a;
	}

	return 0;
}

void so_symbol_fix_ldmia(so_module *mod, const char *symbol) {

	int idx = so_symbol_index(mod, symbol);
	if (idx == -1)
		return;

	uintptr_t st_addr = mod->load_addr + mod->dynsym[idx].st_value;
	for (uintptr_t addr = st_addr; addr < st_addr + mod->dynsym[idx].st_size; addr+=4) {
		uint32_t inst = *(uint32_t*)(addr);

		if (((inst & 0xFFF00000) == 0xE8900000) && (((inst >> 16) & 0xF) < 13) ) {
			SO_UTIL_LOG_CRITICAL("Found possibly misaligned LDMIA on 0x%08X, trying to fix it... (instr: 0x%08X, to 0x%08X)\n", addr, *(uint32_t*)addr, mod->patch_head);
			trampoline_ldm(mod, (uint32_t *) addr);
		}
	}
}
