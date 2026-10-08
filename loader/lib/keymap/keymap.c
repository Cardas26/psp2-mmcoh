#include "keymap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const km_names[KM_N] = {
    "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
    "RStickUp", "RStickDown", "RStickLeft", "RStickRight", "LStickUp", "LStickDown", "LStickLeft", "LStickRight" };
static const char *const km_sticks[3] = { "None", "LStick", "RStick" };
static const uint32_t km_stick_dirs[3] = { 0, 0xFu << KM_LS_UP, 0xFu << KM_RS_UP };

const char *km_button_name(int b) { return b >= 0 && b < KM_N ? km_names[b] : "None"; }

static int km_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
static const char *km_base(const char *path) {
    const char *b = path;
    for (const char *s = path; *s; s++) if (*s == '/' || *s == '\\' || *s == ':') b = s + 1;
    return b;
}

char *km_ini_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static int km_ini_parse(char *line, char **k, char **v) {
    char *s = line;
    if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;
    s = km_ini_trim(s);
    if (*s == '[') { char *c = strchr(s, ']'); if (!c) return 0; *c = 0; *k = km_ini_trim(s + 1); return 1; }
    char *eq = strchr(s, '=');
    if (*s == ';' || *s == '#' || !eq || eq == s) return 0;
    *eq = 0; *k = km_ini_trim(s);
    char *val = eq + 1;
    for (char *c = val; *c; c++) if ((*c == ';' || *c == '#') && (c == val || c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
    *v = km_ini_trim(val);
    return 2;
}
int km_ini_read(const char *path, const char *sect, const char *key, char *out, size_t n) {
    char line[512], *k, *v;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int in = 1, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        int t = km_ini_parse(line, &k, &v);
        if (t == 1) in = km_ieq(k, sect);
        else if (t == 2 && in && km_ieq(k, key)) { snprintf(out, n, "%s", v); found = 1; }
    }
    fclose(f);
    return found;
}
static size_t km_put(char *out, size_t n, size_t cap, const char *s) {
    size_t l = strlen(s);
    if (n + l < cap) memcpy(out + n, s, l + 1);
    return n + l;
}
int km_ini_write(const char *path, const char *sect, const char *key, const char *value) {
    static char out[8192];
    char line[512], cut[512], kv[256], *k, *v;
    size_t n = 0; int in = 0, seen = 0, done = 0, nl = 1;
    out[0] = 0;
    snprintf(kv, sizeof kv, "%s=%s\n", key, value);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            memcpy(cut, line, sizeof cut);
            int t = km_ini_parse(cut, &k, &v);
            if (t == 1) { if (in && !done) { n = km_put(out, n, sizeof out, kv); done = 1; } in = km_ieq(k, sect); seen |= in; }
            else if (t == 2 && in && !done && km_ieq(k, key)) { n = km_put(out, n, sizeof out, kv); done = 1; continue; }
            n = km_put(out, n, sizeof out, line);
            nl = line[0] && line[strlen(line) - 1] == '\n';
        }
        fclose(f);
    }
    if (!done) {
        char h[80];
        snprintf(h, sizeof h, "[%s]\n", sect);
        if (!nl) n = km_put(out, n, sizeof out, "\n");
        if (!seen) n = km_put(out, n, sizeof out, h);
        n = km_put(out, n, sizeof out, kv);
    }
    if (n >= sizeof out || !(f = fopen(path, "w"))) return 0;
    fputs(out, f);
    fclose(f);
    return 1;
}

static void km_log(const struct km_map *m, const char *fmt, ...) {
    if (!m->log) return;
    char t[200]; va_list ap;
    va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    m->log(t);
}
static void km_add_note(struct km_map *m, const char *fmt, ...) {
    char t[80]; va_list ap;
    va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    km_log(m, "controls: %s", t);
    if (m->n_notes < KM_NOTES) snprintf(m->notes[m->n_notes++], sizeof m->notes[0], "%s", t);
}
static int km_meet(const struct km_map *m, int a, int b) {
    uint32_t x = m->acts[a].ctx, y = m->acts[b].ctx;
    return !x || !y || (x & y);
}
static int km_button(const struct km_map *m, const char *s) {
    for (int i = 0; i < KM_N; i++) if ((m->avail & 1u << i) && km_ieq(s, km_names[i])) return i;
    return -1;
}
static int km_parse(struct km_map *m, int a, const char *value, char *bad, size_t nb) {
    struct km_bind *b = &m->bind[a];
    char buf[160], *s = buf;
    snprintf(buf, sizeof buf, "%s", value);
    b->n = 0; b->stick = KM_NO_STICK;
    char *cmt = strpbrk(buf, ";#"); if (cmt) *cmt = 0;
    if (m->acts[a].kind == KM_STICK) {
        char *t = km_ini_trim(buf);
        if (!*t) return 1;
        for (int i = 0; i < 3; i++)
            if (km_ieq(t, km_sticks[i]) && (i == KM_NO_STICK || (m->avail & km_stick_dirs[i]))) { b->stick = i; return 1; }
        snprintf(bad, nb, "%s is no stick", t);
        return 0;
    }
    for (;;) {
        char *c = strchr(s, ','); if (c) *c = 0;
        char *tok = km_ini_trim(s), *plus = strchr(tok, '+');
        int md = KM_NONE, bt = -1;
        if (*tok && !km_ieq(tok, "None")) {
            if (plus) {
                *plus = 0;
                char *t1 = km_ini_trim(tok), *t2 = km_ini_trim(plus + 1);
                if (!*t1 || !*t2 || strchr(t2, '+')) { snprintf(bad, nb, "a chord is two buttons"); return 0; }
                if ((md = km_button(m, t1)) < 0 || (bt = km_button(m, t2)) < 0) { snprintf(bad, nb, "%s is no button", md < 0 ? t1 : t2); return 0; }
                if (md == bt) { snprintf(bad, nb, "%s+%s is one button twice", t1, t2); return 0; }
            } else if ((bt = km_button(m, tok)) < 0) { snprintf(bad, nb, "%s is no button", tok); return 0; }
            if (b->n == KM_ALTS) { snprintf(bad, nb, "more than %d buttons", KM_ALTS); return 0; }
            b->mod[b->n] = (uint8_t)md; b->btn[b->n++] = (uint8_t)bt;
        }
        if (!c) return 1;
        s = c + 1;
    }
}
static const char *km_alt(const struct km_map *m, int a, int i, char *out, size_t n) {
    int md = m->bind[a].mod[i];
    snprintf(out, n, "%s%s%s", md == KM_NONE ? "" : km_names[md], md == KM_NONE ? "" : "+", km_names[m->bind[a].btn[i]]);
    return out;
}
void km_value(const struct km_map *m, int a, char *out, size_t n) {
    char x[32];
    out[0] = 0;
    if (m->acts[a].kind == KM_STICK) { snprintf(out, n, "%s", km_sticks[m->bind[a].stick]); return; }
    for (int i = 0; i < m->bind[a].n; i++) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", i ? ", " : "", km_alt(m, a, i, x, sizeof x));
    }
    if (!out[0]) snprintf(out, n, "None");
}
static void km_line(const struct km_act *x, char *out, size_t n) {
    int w = 28 - (int)strlen(x->name) - 1 - (int)strlen(x->def);
    if (w < 1) w = 1;
    snprintf(out, n, "%s%*s; %s", x->def, w, "", x->desc);
}
static int km_write_whole(struct km_map *m) {
    FILE *f = fopen(m->path, "w");
    if (!f) { km_log(m, "controls: %s cannot be written", m->path); return 0; }
    fprintf(f, "; %s - the controls. Read when the game starts: edit, save, start the game again.\n; Buttons:", m->title);
    int col = 10;
    for (int i = 0; i < KM_N; i++) if (m->avail & 1u << i) {
        int l = (int)strlen(km_names[i]) + 1;
        if (col + l > 110) { fputs("\n;         ", f); col = 10; }
        fprintf(f, " %s", km_names[i]); col += l;
    }
    fputs(" None\n", f);
    int sticks = 0;
    for (int a = 0; a < m->n_acts; a++) sticks |= m->acts[a].kind == KM_STICK;
    if (sticks) {
        fputs("; A whole stick, for", f);
        for (int a = 0, k = 0; a < m->n_acts; a++) if (m->acts[a].kind == KM_STICK) fprintf(f, "%s %s", k++ ? "," : "", m->acts[a].name);
        fputs(":", f);
        for (int i = 1; i < 3; i++) if (m->avail & km_stick_dirs[i]) fprintf(f, " %s", km_sticks[i]);
        fputs(" None\n", f);
    }
    char chord[40] = "Circle+L";
    for (int a = 0; a < m->n_acts; a++) {
        const char *p = strchr(m->acts[a].def, '+');
        if (!p || m->acts[a].kind != KM_BUTTONS) continue;
        const char *s = p, *e = p;
        while (s > m->acts[a].def && s[-1] != ',' && s[-1] != ' ') s--;
        while (*e && *e != ',' && *e != ' ') e++;
        snprintf(chord, sizeof chord, "%.*s", (int)(e - s), s);
        break;
    }
    fprintf(f, "; Several buttons: Up, Cross    A chord, the first held and the second pressed: %s\n", chord);
    int moves = 0;
    for (int a = 0; a < m->n_acts; a++) moves += m->acts[a].need == 2;
    if (moves) {
        fputs("; A button held for a chord still does its own action: keep", f);
        for (int a = 0, k = 0; a < m->n_acts; a++) if (m->acts[a].need == 2)
            fprintf(f, "%s %s", !k++ ? "" : k == moves ? " and" : ",", m->acts[a].name);
        fputs(" off it.\n", f);
    }
    fputs("; A mistake falls back to that action's default and is named on the screen at start.\n"
          "; Delete a line for its default; delete this file for every default.\n", f);
    if (m->header) fputs(m->header, f);
    fputs("[controls]\n", f);
    char v[200];
    for (int a = 0; a < m->n_acts; a++) { km_line(&m->acts[a], v, sizeof v); fprintf(f, "%s=%s\n", m->acts[a].name, v); }
    fclose(f);
    km_log(m, "controls: %s written with every default", m->path);
    return 1;
}
int km_load(struct km_map *m) {
    static char val[KM_ACTS][160];
    int got[KM_ACTS], n_got = 0, missing = 0, last = 0;
    char bad[80], x[32], y[32];
    const char *file = km_base(m->path);
    m->n_notes = 0; m->was = m->taken = 0;
    if (m->n_acts > KM_ACTS) m->n_acts = KM_ACTS;
    FILE *f = fopen(m->path, "rb");
    if (f) fclose(f); else km_write_whole(m);
    for (int a = 0; a < m->n_acts; a++) n_got += got[a] = km_ini_read(m->path, "controls", m->acts[a].name, val[a], sizeof val[a]);
    if (!n_got) {
        long size = -1;
        if ((f = fopen(m->path, "rb"))) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        int keep = size > 0;
        if (keep) remove(m->aside);
        if (keep && rename(m->path, m->aside) != 0) km_add_note(m, "no action readable - defaults used");
        else if (!km_write_whole(m)) km_add_note(m, "%s cannot be written - defaults used", file);
        else if (keep) km_add_note(m, "no action readable - kept as %s", km_base(m->aside));
        else km_add_note(m, "%s was empty - every default written", file);
    } else for (int a = 0; a < m->n_acts; a++) if (!got[a]) {
        char line[200];
        km_line(&m->acts[a], line, sizeof line);
        km_ini_write(m->path, "controls", m->acts[a].name, line);
        missing++; last = a;
    }
    if (missing == 1) km_add_note(m, "%s was missing - default added", m->acts[last].name);
    else if (missing) km_add_note(m, "%d actions were missing - defaults added", missing);
    for (int a = 0; a < m->n_acts; a++) {
        const struct km_act *ac = &m->acts[a];
        const char *v = got[a] ? val[a] : ac->def;
        if (!km_parse(m, a, v, bad, sizeof bad)) { km_add_note(m, "%s: %s - default %s used", ac->name, bad, ac->def); km_parse(m, a, ac->def, bad, sizeof bad); }
        else if (ac->need && (ac->kind == KM_STICK ? !m->bind[a].stick : !m->bind[a].n)) {
            km_add_note(m, "%s has no %s - default %s used", ac->name, ac->kind == KM_STICK ? "stick" : "button", ac->def);
            km_parse(m, a, ac->def, bad, sizeof bad);
        }
    }
    uint32_t warned[KM_ACTS] = {0};
    for (int a = 0; a < m->n_acts; a++) for (int i = 0; i < m->bind[a].n; i++) {
        int md = m->bind[a].mod[i], bt = m->bind[a].btn[i];
        for (int a2 = a + 1; a2 < m->n_acts; a2++) for (int j = 0; j < m->bind[a2].n; j++)
            if (km_meet(m, a, a2) && m->bind[a2].mod[j] == md && m->bind[a2].btn[j] == bt)
                km_add_note(m, "%s does both %s and %s", km_alt(m, a, i, x, sizeof x), m->acts[a].name, m->acts[a2].name);
        if (md == KM_NONE) continue;
        for (int a2 = 0; a2 < m->n_acts; a2++) if (m->acts[a2].need == 2 && km_meet(m, a, a2) && !(warned[a2] & 1u << md))
            for (int j = 0; j < m->bind[a2].n; j++) if (m->bind[a2].mod[j] == KM_NONE && m->bind[a2].btn[j] == md) {
                km_add_note(m, "%s is held for %s and also does %s", km_names[md], m->acts[a].name, m->acts[a2].name);
                warned[a2] |= 1u << md;
                break;
            }
    }
    for (int a = 0; a < m->n_acts; a++) {
        int st = m->bind[a].stick;
        if (!st) continue;
        for (int a2 = a + 1; a2 < m->n_acts; a2++) if (m->bind[a2].stick == st && km_meet(m, a, a2))
            km_add_note(m, "%s does both %s and %s", km_sticks[st], m->acts[a].name, m->acts[a2].name);
        for (int a2 = 0; a2 < m->n_acts; a2++) for (int j = 0; j < m->bind[a2].n && km_meet(m, a, a2); j++) {
            int md = m->bind[a2].mod[j], bt = m->bind[a2].btn[j];
            if ((km_stick_dirs[st] >> bt & 1) || (md != KM_NONE && (km_stick_dirs[st] >> md & 1))) {
                km_add_note(m, "%s does %s and %s does %s", km_sticks[st], m->acts[a].name, km_alt(m, a2, j, y, sizeof y), m->acts[a2].name);
                break;
            }
        }
    }
    for (int a = 0; a < m->n_acts; a++) {
        char t[160];
        km_value(m, a, t, sizeof t);
        km_log(m, "controls: %s=%s", m->acts[a].name, t);
    }
    return m->n_notes;
}

void km_eval(struct km_map *m, uint32_t on, uint8_t *hit, uint8_t *fire) {
    uint32_t land = on & ~m->was;
    m->taken &= on;
    for (int a = 0; a < m->n_acts; a++) for (int i = 0; i < m->bind[a].n; i++) {
        int md = m->bind[a].mod[i], bt = m->bind[a].btn[i];
        if (md != KM_NONE && (on >> md & 1) && (land >> bt & 1)) m->taken |= 1u << bt;
    }
    for (int a = 0; a < m->n_acts; a++) {
        int h = 0, fr = 0;
        if (m->acts[a].kind == KM_BUTTONS)
            for (int i = 0; i < m->bind[a].n; i++) {
                int md = m->bind[a].mod[i];
                uint32_t b = 1u << m->bind[a].btn[i];
                if (md != KM_NONE ? (on >> md & 1) && (m->taken & b) : !(m->taken & b)) { h |= (on & b) != 0; fr |= (land & b) != 0; }
            }
        hit[a] = (uint8_t)h; fire[a] = (uint8_t)fr;
    }
    m->was = on;
}
void km_settle(struct km_map *m) { m->was = ~0u; }

int km_note(const struct km_map *m, int i, char *out, size_t n) {
    if (i < 0 || i >= m->n_notes) return 0;
    snprintf(out, n, "%s %d/%d: %s", km_base(m->path), i + 1, m->n_notes, m->notes[i]);
    return 1;
}
