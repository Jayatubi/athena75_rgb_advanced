// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reads the physical key positions straight out of keymaps/vial/vial.json so the
// virtual keyboard cannot drift from the one Vial draws. Only the KLE subset that
// file actually uses is understood: per-key x/y offsets, w/h, and the "d" decal
// flag. Rotation (r/rx/ry) is not used by this layout and is reported if it shows
// up rather than being silently mis-rendered.
//
// Each key's legend carries its matrix position as "row,col" on one of the legend
// lines, which is what VIA and Vial key off; we take the first line that looks
// like one.

#include "gui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} scan_t;

static void skip_ws(scan_t *s) {
    while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
}

static bool eat(scan_t *s, char c) {
    skip_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

static char peek(scan_t *s) {
    skip_ws(s);
    return s->p < s->end ? *s->p : '\0';
}

// Reads a JSON string into buf, translating the escapes KLE legends use. Newlines
// stay as '\n' because that is how the legend lines are separated.
static bool read_string(scan_t *s, char *buf, size_t cap) {
    if (!eat(s, '"')) return false;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->end) {
            char e = *s->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': continue;
                case 'u':
                    s->p += 4 <= (size_t)(s->end - s->p) ? 4 : (s->end - s->p);
                    c = '?';
                    break;
                default: c = e; break;
            }
        }
        if (n + 1 < cap) buf[n++] = c;
    }
    if (n < cap) buf[n] = '\0';
    return eat(s, '"');
}

static bool read_number(scan_t *s, float *out) {
    skip_ws(s);
    char *endp = NULL;
    float v    = strtof(s->p, &endp);
    if (endp == s->p) return false;
    s->p = endp;
    *out = v;
    return true;
}

// Skips one JSON value of any shape, so unknown properties cost nothing.
static void skip_value(scan_t *s) {
    char c = peek(s);
    if (c == '"') {
        char tmp[8];
        read_string(s, tmp, sizeof(tmp));
        return;
    }
    if (c == '[' || c == '{') {
        char open  = c;
        char close = c == '[' ? ']' : '}';
        int  depth = 0;
        s->p++; // consume the opener; strings inside must be skipped properly
        depth = 1;
        while (s->p < s->end && depth) {
            char d = *s->p;
            if (d == '"') {
                char tmp[8];
                read_string(s, tmp, sizeof(tmp));
                continue;
            }
            if (d == open) depth++;
            if (d == close) depth--;
            s->p++;
        }
        return;
    }
    while (s->p < s->end && *s->p != ',' && *s->p != ']' && *s->p != '}') s->p++;
}

// KLE property object: only the geometry keys matter here.
static void read_props(scan_t *s, float *dx, float *dy, float *w, float *h, bool *decal) {
    if (!eat(s, '{')) return;
    if (eat(s, '}')) return;
    do {
        char name[32];
        if (!read_string(s, name, sizeof(name))) break;
        if (!eat(s, ':')) break;
        if (!strcmp(name, "x")) {
            read_number(s, dx);
        } else if (!strcmp(name, "y")) {
            read_number(s, dy);
        } else if (!strcmp(name, "w")) {
            read_number(s, w);
        } else if (!strcmp(name, "h")) {
            read_number(s, h);
        } else if (!strcmp(name, "d")) {
            skip_ws(s);
            *decal = (s->p < s->end && *s->p == 't');
            skip_value(s);
        } else if (!strcmp(name, "r") || !strcmp(name, "rx") || !strcmp(name, "ry")) {
            LOG_W(LOG_D_GUI, "vial.json uses KLE rotation (%s); the virtual keyboard "
                             "draws it unrotated",
                  name);
            skip_value(s);
        } else {
            skip_value(s);
        }
    } while (eat(s, ','));
    eat(s, '}');
}

// "\n\n\n0,0" -> row 0, col 0. Returns false when no legend line is a position.
static bool matrix_from_legend(const char *legend, unsigned *row, unsigned *col) {
    const char *p = legend;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t len = (size_t)(p - line);
        if (*p == '\n') p++;
        unsigned r, c;
        int      used = 0;
        if (len && sscanf(line, "%u,%u%n", &r, &c, &used) == 2 && (size_t)used == len) {
            *row = r;
            *col = c;
            return true;
        }
    }
    return false;
}

int kle_load(kle_layout_t *out, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E(LOG_D_GUI, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1u);
    if (!text || fread(text, 1u, (size_t)size, f) != (size_t)size) {
        LOG_E(LOG_D_GUI, "cannot read %s", path);
        free(text);
        fclose(f);
        return -1;
    }
    text[size] = '\0';
    fclose(f);

    // The file has one "keymap" key, inside "layouts".
    const char *kw = strstr(text, "\"keymap\"");
    if (!kw) {
        LOG_E(LOG_D_GUI, "%s has no \"keymap\" array", path);
        free(text);
        return -1;
    }
    scan_t sc = {kw + strlen("\"keymap\""), text + size};
    if (!eat(&sc, ':') || !eat(&sc, '[')) {
        LOG_E(LOG_D_GUI, "%s: malformed keymap array", path);
        free(text);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    float y = 0.0f;
    if (peek(&sc) != ']') {
        do {
            if (!eat(&sc, '[')) break;
            float x = 0.0f, w = 1.0f, h = 1.0f;
            bool  decal = false;
            if (peek(&sc) != ']') {
                do {
                    if (peek(&sc) == '{') {
                        float dx = 0.0f, dy = 0.0f;
                        read_props(&sc, &dx, &dy, &w, &h, &decal);
                        x += dx;
                        y += dy;
                        continue;
                    }
                    char legend[128];
                    if (!read_string(&sc, legend, sizeof(legend))) break;
                    unsigned row = 0, col = 0;
                    if (!matrix_from_legend(legend, &row, &col)) {
                        LOG_W(LOG_D_GUI, "key at (%.2f,%.2f) has no row,col legend", x, y);
                    } else if (out->count < KLE_MAX_KEYS) {
                        kle_key_t *k = &out->keys[out->count++];
                        k->x = x;
                        k->y = y;
                        k->w = w;
                        k->h = h;
                        k->row = (uint8_t)row;
                        k->col = (uint8_t)col;
                        k->decal = decal;
                        if (x + w > out->width) out->width = x + w;
                        if (y + h > out->height) out->height = y + h;
                    } else {
                        LOG_W(LOG_D_GUI, "more than %u keys in the layout", KLE_MAX_KEYS);
                    }
                    // Per KLE, w/h/decal apply to a single key then reset.
                    x += w;
                    w = h = 1.0f;
                    decal = false;
                } while (eat(&sc, ','));
            }
            eat(&sc, ']');
            y += 1.0f;
        } while (eat(&sc, ','));
    }

    free(text);
    LOG_I(LOG_D_GUI, "layout: %u keys over %.2fx%.2f units from %s", out->count, out->width,
          out->height, path);
    return (int)out->count;
}
