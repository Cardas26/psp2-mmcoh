#include "audio/save_names.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <so_util/so_util.h>
#include "utils/logger.h"

extern so_module so_mod;

#include "audio/music_script_names.h"

#define SYM_LOAD_CAMPAIGN   "_ZN8SaveGame12loadCampaignENS_14t_saveCampaignE"
#define SYM_SAVE_CAMPAIGN   "_ZN8SaveGame12saveCampaignENS_14t_saveCampaignE"
#define SYM_CAMPAIGN_INDEX  "_ZN8SaveGame16getCampaignIndexENS_14t_saveCampaignE"
#define SYM_CHECKSUM        "_ZN8SaveGame20calcCampaignChecksumEPNS_18t_campaignSettingsE"
#define SYM_SAVE_DATA       "_ZN8SaveGame10s_saveDataE"
#define SYM_SOFT_SAVE       "_ZN8SaveGame10s_softSaveE"

#define SOFT_SAVE_SLOT   0x40
#define SLOT_STRIDE      0x1778
#define RECORD_OFFSET    0xc
#define ENTRIES_OFFSET   0x80
#define ENTRY_STRIDE     0x104
#define ENTRY_N          15
#define FIELD_LEN        0x80

static so_hook h_load, h_save;
static int (*p_campaign_index)(int slot);
static uint32_t (*p_checksum)(void *record);
static uint8_t *g_save_data, *g_soft_save;

static uint8_t *record_for(int slot) {
	if (slot == SOFT_SAVE_SLOT)
		return g_soft_save;
	int idx = p_campaign_index(slot);
	if (idx < 0)
		return NULL;
	return g_save_data + idx * SLOT_STRIDE + RECORD_OFFSET;
}

static const char *repair_for(const char *field, const char *const *names, int n) {
	size_t len = strnlen(field, FIELD_LEN);
	if (len == 0)
		return NULL;
	for (int i = 0; i < n; i++)
		if (strncmp(field, names[i], FIELD_LEN) == 0)
			return NULL;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(names[i]);
		if (l < len && memcmp(field, names[i], l) == 0)
			return names[i];
	}
	return NULL;
}

static bool repair_field(char *field, const char *const *names, int n,
                         const char *what, int slot, int i, unsigned area) {
	const char *fix = repair_for(field, names, n);
	if (!fix)
		return false;
	l_perf("[save-names] slot %d entry %d area %u: %s \"%.*s\" -> \"%s\"",
	       slot, i, area, what, (int)strnlen(field, FIELD_LEN), field, fix);
	memset(field, 0, FIELD_LEN);
	strcpy(field, fix);
	return true;
}

static void repair_record(uint8_t *rec, int slot) {
	if (!rec)
		return;
	bool changed = false;
	for (int i = 0; i < ENTRY_N; i++) {
		uint8_t *e = rec + ENTRIES_OFFSET + i * ENTRY_STRIDE;
		unsigned area = *(uint16_t *)(e + 2 * FIELD_LEN);
		changed |= repair_field((char *)e, MUSIC_SCRIPT_BGM, MUSIC_SCRIPT_BGM_N,
		                        "BGM", slot, i, area);
		changed |= repair_field((char *)e + FIELD_LEN, MUSIC_SCRIPT_AMBIENT,
		                        MUSIC_SCRIPT_AMBIENT_N, "ambient", slot, i, area);
	}
	if (changed)
		*(uint32_t *)rec = p_checksum(rec);
}

static int hook_loadCampaign(int slot) {
	uint8_t *rec = record_for(slot);
	repair_record(rec, slot);
	return SO_CONTINUE(int, h_load, slot);
}

static int hook_saveCampaign(int slot) {
	int r = SO_CONTINUE(int, h_save, slot);
	uint8_t *rec = record_for(slot);
	repair_record(rec, slot);
	return r;
}

void save_names_install(void) {
	uintptr_t load = so_symbol(&so_mod, SYM_LOAD_CAMPAIGN);
	uintptr_t save = so_symbol(&so_mod, SYM_SAVE_CAMPAIGN);
	p_campaign_index = (int (*)(int))so_symbol(&so_mod, SYM_CAMPAIGN_INDEX);
	p_checksum       = (uint32_t (*)(void *))so_symbol(&so_mod, SYM_CHECKSUM);
	g_save_data      = (uint8_t *)so_symbol(&so_mod, SYM_SAVE_DATA);
	g_soft_save      = (uint8_t *)so_symbol(&so_mod, SYM_SOFT_SAVE);
	if (!load || !save || !p_campaign_index || !p_checksum || !g_save_data || !g_soft_save) {
		l_error("[save-names] SaveGame symbols missing - NOT hooked; an area "
		        "whose cue name changed between saves stays silent after a load");
		return;
	}
	h_load = hook_addr(load, (uintptr_t)&hook_loadCampaign);
	h_save = hook_addr(save, (uintptr_t)&hook_saveCampaign);
	l_perf("[save-names] loadCampaign/saveCampaign hooked; %d BGM and %d ambient "
	       "names known", MUSIC_SCRIPT_BGM_N, MUSIC_SCRIPT_AMBIENT_N);
}
