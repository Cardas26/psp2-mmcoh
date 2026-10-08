#ifndef KEYMAP_H
#define KEYMAP_H
#include <stddef.h>
#include <stdint.h>

enum { KM_UP, KM_DOWN, KM_LEFT, KM_RIGHT, KM_CROSS, KM_CIRCLE, KM_SQUARE, KM_TRI, KM_L, KM_R, KM_START, KM_SELECT,
       KM_RS_UP, KM_RS_DOWN, KM_RS_LEFT, KM_RS_RIGHT, KM_LS_UP, KM_LS_DOWN, KM_LS_LEFT, KM_LS_RIGHT, KM_N, KM_NONE = 0xFF };
enum { KM_NO_STICK, KM_LSTICK, KM_RSTICK };
enum { KM_BUTTONS, KM_STICK };
enum { KM_ACTS = 32, KM_ALTS = 4, KM_NOTES = 8 };

struct km_act { const char *name, *def, *desc; int kind, need; uint32_t ctx; };
struct km_bind { int n; uint8_t mod[KM_ALTS], btn[KM_ALTS]; int stick; };

struct km_map {
    const char *path, *aside;
    const char *title;
    const char *header;
    const struct km_act *acts; int n_acts;
    uint32_t avail;
    void (*log)(const char *line);
    struct km_bind bind[KM_ACTS];
    char notes[KM_NOTES][80]; int n_notes;
    uint32_t was, taken;
};

const char *km_button_name(int b);
int km_load(struct km_map *m);
void km_eval(struct km_map *m, uint32_t on, uint8_t *hit, uint8_t *fire);
void km_settle(struct km_map *m);
int km_note(const struct km_map *m, int i, char *out, size_t n);
void km_value(const struct km_map *m, int a, char *out, size_t n);

char *km_ini_trim(char *s);
int km_ini_read(const char *path, const char *sect, const char *key, char *out, size_t n);
int km_ini_write(const char *path, const char *sect, const char *key, const char *value);
#endif
