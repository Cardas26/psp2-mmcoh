#include "utils/transform_same_gate.h"

#include <stdint.h>
#include <stdio.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define ON_CHANGED_SYM "_ZN5moFlo4Core10CTransform18OnTransformChangedEv"

#define POS_OFF   0x80
#define SCALE_OFF 0x8c

typedef int (*on_changed_fn)(void *self);
static on_changed_fn g_on_changed;

enum { S_POS3 = 0, S_POSV, S_SCALE1, S_SCALE3, S_SCALEV, S_COUNT };
static const char *const k_sym[S_COUNT] = {
	"_ZN5moFlo4Core10CTransform11SetPositionEfff",
	"_ZN5moFlo4Core10CTransform11SetPositionERKNS0_8CVector3E",
	"_ZN5moFlo4Core10CTransform7ScaleToEf",
	"_ZN5moFlo4Core10CTransform7ScaleToEfff",
	"_ZN5moFlo4Core10CTransform7ScaleToERKNS0_8CVector3E",
};
static const char *const k_name[S_COUNT] = { "pos3", "posv", "scale1", "scale3", "scalev" };

#define GATE_ON() (1)

#define GATE_COUNT(c)  ((void)0)

static inline int set3(void *self, unsigned off, unsigned slot,
                       uint32_t a, uint32_t b, uint32_t c) {
	uint32_t *w = (uint32_t *)((uint8_t *)self + off);
	if (GATE_ON() && w[0] == a && w[1] == b && w[2] == c) {
		return 0;
	}
	w[0] = a; w[1] = b; w[2] = c;
	return g_on_changed(self);
}

static int w_pos3(void *self, uint32_t x, uint32_t y, uint32_t z) {
	return set3(self, POS_OFF, S_POS3, x, y, z);
}
static int w_posv(void *self, const uint32_t *v) {
	return set3(self, POS_OFF, S_POSV, v[0], v[1], v[2]);
}
static int w_scale1(void *self, uint32_t s) {
	return set3(self, SCALE_OFF, S_SCALE1, s, s, s);
}
static int w_scale3(void *self, uint32_t x, uint32_t y, uint32_t z) {
	return set3(self, SCALE_OFF, S_SCALE3, x, y, z);
}
static int w_scalev(void *self, const uint32_t *v) {
	return set3(self, SCALE_OFF, S_SCALEV, v[0], v[1], v[2]);
}

void transform_same_gate_install(void) {
	uintptr_t on_changed = so_symbol(&so_mod, ON_CHANGED_SYM);
	uintptr_t addr[S_COUNT];
	for (int i = 0; i < S_COUNT; i++) addr[i] = so_symbol(&so_mod, k_sym[i]);
	int missing = !on_changed;
	for (int i = 0; i < S_COUNT; i++) if (!addr[i]) missing = 1;
	if (missing) {
		l_error("transform-gate: a CTransform symbol is missing - NOT installed, "
		        "the setters run stock");
		return;
	}
	g_on_changed = (on_changed_fn)on_changed;
	(void)hook_addr(addr[S_POS3],   (uintptr_t)&w_pos3);
	(void)hook_addr(addr[S_POSV],   (uintptr_t)&w_posv);
	(void)hook_addr(addr[S_SCALE1], (uintptr_t)&w_scale1);
	(void)hook_addr(addr[S_SCALE3], (uintptr_t)&w_scale3);
	(void)hook_addr(addr[S_SCALEV], (uintptr_t)&w_scalev);
}
