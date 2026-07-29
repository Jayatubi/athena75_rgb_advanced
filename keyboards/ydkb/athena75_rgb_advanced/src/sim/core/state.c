// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "state.h"

#include "log.h"
#include "sim.h"

#include "../jit/jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   "ATHSNAP2"
#define TAG_LEN       15
#define MAX_BLOBS     32
#define FLASH_SECTOR  4096u

typedef struct {
    char               tag[TAG_LEN + 1];
    void              *blob;
    uint32_t           len;
    sim_state_fixup_fn fixup;
} blob_t;

static blob_t   g_blobs[MAX_BLOBS];
static unsigned g_blob_count;

void sim_state_register(sim_t *s, const char *tag, void *blob, size_t len, sim_state_fixup_fn fixup) {
    (void)s;
    if (g_blob_count == MAX_BLOBS) {
        LOG_E(LOG_D_SIM, "save-state blob table full, '%s' will not be saved", tag);
        return;
    }
    blob_t *b = &g_blobs[g_blob_count++];
    snprintf(b->tag, sizeof b->tag, "%s", tag);
    b->blob  = blob;
    b->len   = (uint32_t)len;
    b->fixup = fixup;
}

// ---- record I/O -------------------------------------------------------------

static bool put_record(FILE *f, const char *tag, const void *data, uint32_t len) {
    char hdr[TAG_LEN + 1] = {0};
    snprintf(hdr, sizeof hdr, "%s", tag);
    if (fwrite(hdr, sizeof hdr, 1, f) != 1) return false;
    if (fwrite(&len, sizeof len, 1, f) != 1) return false;
    return len == 0 || fwrite(data, 1, len, f) == len;
}

// ---- machine-level scalars --------------------------------------------------

typedef struct {
    uint64_t cycles;
    uint32_t cur_core, irq_lines;
    uint8_t  bootsel_requested;
    uint8_t  pad[3];
} machine_rec_t;

static void pack_machine(const sim_t *s, machine_rec_t *m) {
    m->cycles            = s->cycles;
    m->cur_core          = s->cur_core;
    m->irq_lines         = s->irq_lines;
    m->bootsel_requested = s->bootsel_requested;
    memset(m->pad, 0, sizeof m->pad);
}

static void unpack_machine(sim_t *s, const machine_rec_t *m) {
    s->cycles            = m->cycles;
    s->cur_core          = m->cur_core;
    s->irq_lines         = m->irq_lines;
    s->bootsel_requested = m->bootsel_requested != 0;
}

// ---- save -------------------------------------------------------------------

// 16 MiB of mostly-erased flash compresses to nothing by just skipping the
// blank sectors, which keeps a save under a few MiB without pulling in zlib.
static bool put_flash(FILE *f, const sim_t *s) {
    uint32_t nsectors = 0;
    for (uint32_t off = 0; off < SIM_FLASH_SIZE; off += FLASH_SECTOR) {
        const uint8_t *p = s->flash + off;
        for (uint32_t i = 0; i < FLASH_SECTOR; i++) {
            if (p[i] != 0xFFu) {
                nsectors++;
                break;
            }
        }
    }

    char hdr[TAG_LEN + 1] = {0};
    snprintf(hdr, sizeof hdr, "flash");
    uint32_t len = 4u + nsectors * (4u + FLASH_SECTOR);
    if (fwrite(hdr, sizeof hdr, 1, f) != 1) return false;
    if (fwrite(&len, sizeof len, 1, f) != 1) return false;
    if (fwrite(&nsectors, sizeof nsectors, 1, f) != 1) return false;

    for (uint32_t off = 0; off < SIM_FLASH_SIZE; off += FLASH_SECTOR) {
        const uint8_t *p     = s->flash + off;
        bool           blank = true;
        for (uint32_t i = 0; i < FLASH_SECTOR; i++) {
            if (p[i] != 0xFFu) {
                blank = false;
                break;
            }
        }
        if (blank) continue;
        if (fwrite(&off, sizeof off, 1, f) != 1) return false;
        if (fwrite(p, 1, FLASH_SECTOR, f) != FLASH_SECTOR) return false;
    }
    LOG_D(LOG_D_SIM, "save-state: %u of %u flash sectors are non-blank", nsectors,
          (unsigned)(SIM_FLASH_SIZE / FLASH_SECTOR));
    return true;
}

int sim_state_save(sim_t *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG_E(LOG_D_SIM, "cannot write save-state %s", path);
        return -1;
    }

    bool ok = fwrite(STATE_MAGIC, 8, 1, f) == 1;

    machine_rec_t m;
    pack_machine(s, &m);
    ok = ok && put_record(f, "machine", &m, sizeof m);
    ok = ok && put_record(f, "cpu0", &s->cpu[0], sizeof s->cpu[0]);
    ok = ok && put_record(f, "cpu1", &s->cpu[1], sizeof s->cpu[1]);
    ok = ok && put_record(f, "sram", s->sram, SIM_SRAM_SIZE);
    ok = ok && put_record(f, "rom", s->rom, SIM_ROM_SIZE);
    ok = ok && put_flash(f, s);
    for (unsigned i = 0; i < g_blob_count && ok; i++) {
        ok = put_record(f, g_blobs[i].tag, g_blobs[i].blob, g_blobs[i].len);
    }
    ok = ok && put_record(f, "end", NULL, 0);

    long size = ftell(f);
    fclose(f);
    if (!ok) {
        LOG_E(LOG_D_SIM, "save-state %s is truncated", path);
        return -1;
    }
    LOG_I(LOG_D_SIM, "saved machine state to %s (%.1f MiB) at %.3f ms", path, size / 1048576.0,
          sim_now_us(s) / 1000.0);
    return 0;
}

// ---- load -------------------------------------------------------------------

static bool take_flash(FILE *f, sim_t *s, uint32_t len) {
    uint32_t nsectors = 0;
    if (fread(&nsectors, sizeof nsectors, 1, f) != 1) return false;
    if (len != 4u + nsectors * (4u + FLASH_SECTOR)) {
        LOG_E(LOG_D_SIM, "save-state flash record is malformed");
        return false;
    }
    memset(s->flash, 0xFF, SIM_FLASH_SIZE);
    for (uint32_t i = 0; i < nsectors; i++) {
        uint32_t off;
        if (fread(&off, sizeof off, 1, f) != 1) return false;
        if (off > SIM_FLASH_SIZE - FLASH_SECTOR) return false;
        if (fread(s->flash + off, 1, FLASH_SECTOR, f) != FLASH_SECTOR) return false;
    }
    return true;
}

// Blobs are matched by tag, not by position, so a build that gained a
// peripheral can still read an older file: the new model just keeps its
// freshly reset state.
static bool take_blob(sim_t *s, const char *tag, const uint8_t *data, uint32_t len) {
    for (unsigned i = 0; i < g_blob_count; i++) {
        blob_t *b = &g_blobs[i];
        if (strcmp(b->tag, tag) != 0) continue;
        if (b->len != len) {
            LOG_E(LOG_D_SIM, "save-state '%s' is %u bytes, this build wants %u -- "
                             "the file is from a different build",
                  tag, len, b->len);
            return false;
        }
        void *old = malloc(len ? len : 1u);
        if (!old) return false;
        memcpy(old, b->blob, len);
        memcpy(b->blob, data, len);
        if (b->fixup) b->fixup(s, b->blob, old);
        free(old);
        return true;
    }
    LOG_W(LOG_D_SIM, "save-state has an unknown blob '%s' (%u bytes), ignoring", tag, len);
    return true;
}

int sim_state_load(sim_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E(LOG_D_SIM, "cannot read save-state %s", path);
        return -1;
    }
    char magic[8];
    if (fread(magic, sizeof magic, 1, f) != 1 || memcmp(magic, STATE_MAGIC, 8) != 0) {
        LOG_E(LOG_D_SIM, "%s is not an athena_sim save-state", path);
        fclose(f);
        return -1;
    }

    bool ok = true;
    for (;;) {
        char     tag[TAG_LEN + 1];
        uint32_t len;
        if (fread(tag, sizeof tag, 1, f) != 1 || fread(&len, sizeof len, 1, f) != 1) {
            LOG_E(LOG_D_SIM, "save-state %s ends without an 'end' record", path);
            ok = false;
            break;
        }
        tag[TAG_LEN] = '\0';
        if (!strcmp(tag, "end")) break;

        if (!strcmp(tag, "flash")) {
            ok = take_flash(f, s, len);
            if (!ok) break;
            continue;
        }

        uint8_t *buf = malloc(len ? len : 1u);
        if (!buf || (len && fread(buf, 1, len, f) != len)) {
            free(buf);
            ok = false;
            break;
        }

        if (!strcmp(tag, "machine") && len == sizeof(machine_rec_t)) {
            unpack_machine(s, (const machine_rec_t *)buf);
        } else if (!strcmp(tag, "cpu0") && len == sizeof s->cpu[0]) {
            sim_t   *back = s->cpu[0].sim;
            unsigned id   = s->cpu[0].id;
            memcpy(&s->cpu[0], buf, len);
            s->cpu[0].sim = back;
            s->cpu[0].id  = id;
        } else if (!strcmp(tag, "cpu1") && len == sizeof s->cpu[1]) {
            sim_t   *back = s->cpu[1].sim;
            unsigned id   = s->cpu[1].id;
            memcpy(&s->cpu[1], buf, len);
            s->cpu[1].sim = back;
            s->cpu[1].id  = id;
        } else if (!strcmp(tag, "sram") && len == SIM_SRAM_SIZE) {
            memcpy(s->sram, buf, len);
        } else if (!strcmp(tag, "rom") && len == SIM_ROM_SIZE) {
            memcpy(s->rom, buf, len);
        } else {
            ok = take_blob(s, tag, buf, len);
        }
        free(buf);
        if (!ok) break;
    }
    fclose(f);
    if (!ok) return -1;

    // Flash, SRAM and both cores were all just replaced wholesale. Nothing about a
    // block translated for the machine that was here a moment ago is still true.
    jit_flush_all(s);

    LOG_I(LOG_D_SIM, "restored machine state from %s at %.3f ms, core0 pc=%08x core1 %s", path,
          sim_now_us(s) / 1000.0, s->cpu[0].r[15], s->cpu[1].running ? "running" : "halted");
    return 0;
}
