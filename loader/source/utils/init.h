/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_INIT_H
#define SOLOADER_INIT_H

#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

extern so_module so_mod_fmodex;
extern so_module so_mod_fmodevent;

#define FMODEX_LOAD_ADDRESS    0x99000000
#define FMODEVENT_LOAD_ADDRESS 0x99200000

void resolve_imports(so_module *mod);

void so_patch();

void soloader_init_all();

#ifdef __cplusplus
};
#endif

#endif
