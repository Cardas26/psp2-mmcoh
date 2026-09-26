#ifndef SOLOADER_SPRITE_STATE_H
#define SOLOADER_SPRITE_STATE_H

#include <stdint.h>

#define F_POS_X      0x80
#define F_POS_Y      0x84
#define F_POS_Z      0x88
#define F_WIDTH      0x8C
#define F_HEIGHT     0x90
#define F_SCALE_X    0x94
#define F_SCALE_Y    0x98
#define F_NODE       0xAC
#define F_YSORT      0xB5
#define F_DIRTY      0xB6
#define F_VISIBLE    0xB7
#define F_PRIORITY   0xBC
#define F_ANCHOR     0xDC
#define F_CANCHOR_X  0xE8
#define F_CANCHOR_Y  0xEC
#define F_REF_Z      0xF0
#define F_OFFSET_X   0xF4
#define F_OFFSET_Y   0xF8
#define F_FLIP_V     0x128
#define F_FLIP_H     0x129
#define F_ALWAYS     0x11C

#define SPRITE_FLOAT_NEG_BITS 0x80000000u

#define SPRITE_RD32(s, off) (*(volatile uint32_t *)((volatile uint8_t *)(s) + (off)))
#define SPRITE_RD8(s, off)  (*((volatile uint8_t *)(s) + (off)))
#define SPRITE_WR8(s, off, v) (*((volatile uint8_t *)(s) + (off)) = (uint8_t)(v))

static const uint16_t sprite_state_words[] = {
	0x80, 0x84, 0x88,
	0x8C, 0x90,
	0x94, 0x98, 0x9C,
	0xE0, 0xE4,
	0xE8, 0xEC,
	0xF0,
	0xF4, 0xF8,
	0xBC,
	0xDC,
	0xFC,
	0x128,
};
#define SPRITE_STATE_WORDS \
	(sizeof(sprite_state_words) / sizeof(sprite_state_words[0]))

static inline void sprite_state_snap(const void *self, uint32_t *out) {
	for (uint32_t i = 0; i < SPRITE_STATE_WORDS; i++)
		out[i] = SPRITE_RD32(self, sprite_state_words[i]);
}

static inline uint32_t sprite_state_unchanged(const void *self, const uint32_t *before) {
	for (uint32_t i = 0; i < SPRITE_STATE_WORDS; i++)
		if (SPRITE_RD32(self, sprite_state_words[i]) != before[i]) return 0;
	return 1;
}

#endif
