#include "audio/music_rewrite.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

#include "audio/music_seg_cache.h"
#include "utils/init.h"
#include "utils/logger.h"

#define FMOD_CREATESTREAM           0x00000080
#define FMOD_CREATECOMPRESSEDSAMPLE 0x00000200
#define FMOD_OPENUSER               0x00000400
#define FMOD_OPENMEMORY             0x00000800

#define EXINFO_SIZE 136
#define EXINFO_W_INITIALSUBSOUND 7
#define EXINFO_W_INCLUSIONLIST   9
#define EXINFO_W_INCLUSIONNUM    10

#define MUSIC_BANK "MMCoH_Music_Bank.fsb"

#define SYM_CREATESTREAM   "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define SYM_CREATESOUND    "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define SYM_CREATESOUNDINT "_ZN4FMOD7SystemI19createSoundInternalEPKcjjjP22FMOD_CREATESOUNDEXINFOPPNS_4FileEbPPNS_6SoundIE"
#define SYM_PLAYSOUND      "_ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE"
#define SYM_GETSUBSOUND    "_ZN4FMOD5Sound11getSubSoundEiPPS0_"
#define SYM_SND_RELEASE    "_ZN4FMOD5Sound7releaseEv"

typedef int (*create_fn)(void *self, const char *name, uint32_t mode, void *exinfo, void **sound);
typedef int (*getsub_fn)(void *snd, int index, void **sub);

static so_hook h_createstream, h_createsoundint, h_playsound, h_snd_release;
static create_fn g_fn_createsound;
static getsub_fn g_fn_getsubsound;

#define RING 16
static int g_incl_ring[RING];
static volatile uint32_t g_incl_next;

static void *g_parent[RING];
static int   g_parent_seg[RING];

static const char *basename_of(const char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

static int is_music_bank_open(const char *name, uint32_t mode) {
	return name && !(mode & (FMOD_OPENUSER | FMOD_OPENMEMORY)) &&
	       strcmp(basename_of(name), MUSIC_BANK) == 0;
}

static void remember_parent(void *snd, int seg) {
	for (int i = 0; i < RING; i++)
		if (!g_parent[i]) { g_parent[i] = snd; g_parent_seg[i] = seg; return; }
	l_warn("[music] parent table full, %p (segment %d) not tracked - it will play silent", snd, seg);
}

static int parent_segment(void *snd) {
	for (int i = 0; i < RING; i++)
		if (g_parent[i] == snd) return g_parent_seg[i];
	return -1;
}

static void forget_parent(void *snd) {
	for (int i = 0; i < RING; i++)
		if (g_parent[i] == snd) g_parent[i] = NULL;
}

static int hook_createStream(void *self, const char *name, uint32_t mode,
                             void *exinfo, void **sound) {
	uint32_t *w = (uint32_t *)exinfo;
	if (!(mode & FMOD_CREATESTREAM) || !exinfo || w[0] != EXINFO_SIZE ||
	    !is_music_bank_open(name, mode))
		return SO_CONTINUE(int, h_createstream, self, name, mode, exinfo, sound);

	uint32_t slot = g_incl_next++ % RING;
	int seg = (int)w[EXINFO_W_INITIALSUBSOUND];
	g_incl_ring[slot] = seg;
	w[EXINFO_W_INCLUSIONLIST] = (uint32_t)(uintptr_t)&g_incl_ring[slot];
	w[EXINFO_W_INCLUSIONNUM]  = 1;
	uint32_t nm = (mode & ~(uint32_t)FMOD_CREATESTREAM) | FMOD_CREATECOMPRESSEDSAMPLE;

	music_seg_cache_hint(seg);

	int r = g_fn_createsound(self, name, nm, exinfo, sound);
	if (r == 0 && sound && *sound)
		remember_parent(*sound, seg);
	l_info("[music] open of segment %d: mode %#x -> %#x via createSound = %d, sound=%p",
	       seg, mode, nm, r, (r == 0 && sound) ? *sound : NULL);
	return r;
}

static int hook_createSoundInternal(void *self, const char *name, uint32_t mode,
                                    uint32_t a, uint32_t b, void *exinfo,
                                    void **file, int flag, void **sound) {
	if ((mode & FMOD_CREATESTREAM) && (mode & FMOD_CREATECOMPRESSEDSAMPLE) &&
	    is_music_bank_open(name, mode))
		mode &= ~(uint32_t)FMOD_CREATESTREAM;
	return SO_CONTINUE(int, h_createsoundint, self, name, mode, a, b, exinfo,
	                   file, flag, sound);
}

static int hook_playSound(void *self, int channelindex, void *sound,
                          int paused, void **channel) {
	int seg = parent_segment(sound);
	if (seg >= 0) {
		void *sub = NULL;
		int gr = g_fn_getsubsound(sound, seg, &sub);
		if (gr == 0 && sub)
			sound = sub;
		else
			l_warn("[music] playSound(parent %p): getSubSound(%d) = %d - playing the parent as asked",
			       sound, seg, gr);
	}
	return SO_CONTINUE(int, h_playsound, self, channelindex, sound, paused, channel);
}

static int hook_snd_release(void *self) {
	forget_parent(self);
	return SO_CONTINUE(int, h_snd_release, self);
}

void music_rewrite_install(void) {
	uintptr_t cs  = so_symbol(&so_mod_fmodex, SYM_CREATESTREAM);
	uintptr_t csi = so_symbol(&so_mod_fmodex, SYM_CREATESOUNDINT);
	uintptr_t ps  = so_symbol(&so_mod_fmodex, SYM_PLAYSOUND);
	uintptr_t rel = so_symbol(&so_mod_fmodex, SYM_SND_RELEASE);
	g_fn_createsound = (create_fn)so_symbol(&so_mod_fmodex, SYM_CREATESOUND);
	g_fn_getsubsound = (getsub_fn)so_symbol(&so_mod_fmodex, SYM_GETSUBSOUND);
	if (!cs || !csi || !ps || !rel || !g_fn_createsound || !g_fn_getsubsound) {
		l_error("[music] libfmodex symbol missing (createStream=%#x createSoundInternal=%#x "
		        "playSound=%#x release=%#x createSound=%p getSubSound=%p) - music opens NOT rewritten",
		        (unsigned)cs, (unsigned)csi, (unsigned)ps, (unsigned)rel,
		        (void *)g_fn_createsound, (void *)g_fn_getsubsound);
		return;
	}
	h_createstream   = hook_addr(cs,  (uintptr_t)&hook_createStream);
	h_createsoundint = hook_addr(csi, (uintptr_t)&hook_createSoundInternal);
	h_playsound      = hook_addr(ps,  (uintptr_t)&hook_playSound);
	h_snd_release    = hook_addr(rel, (uintptr_t)&hook_snd_release);
	l_info("[music] armed: music-bank opens go through the createSound export as "
	       "CREATECOMPRESSEDSAMPLE + an inclusion list of the one segment; "
	       "playSound(parent) plays that segment");
}
