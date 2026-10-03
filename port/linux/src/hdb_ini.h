/*
HDB_INI.H

A small hdbridge.ini reader, included by posix_hdbridge.c and
port/windows/src/win32_hdbridge.c (host-ABI units, whose stdio sees real
paths).
*/
#ifndef HDB_INI_H
#define HDB_INI_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdb_bridge.h"

static void hdb_ini_trim(char* s) {
    char* e;
    char* b = s;
    while (*b && isspace((unsigned char)*b)) ++b;
    if (b != s) memmove(s, b, strlen(b) + 1);
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

/* keys and sections ignore letter case: sUZDoom, suzdoom and SUZDOOM alike */
static int hdb_ini_same(const char* a, const char* b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return tolower((unsigned char)*a) == tolower((unsigned char)*b);
}

static void hdb_ini_copy(char* dst, size_t n, const char* src) {
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

static void hdb_config_defaults(hdb_config* c) {
    memset(c, 0, sizeof *c);
    c->start_doom = 1;
#ifdef _WIN32
    hdb_ini_copy(c->uzdoom_exe, sizeof c->uzdoom_exe, "HaloDoomBridge\\uzdoom\\uzdoom.exe");
#else
    hdb_ini_copy(c->uzdoom_exe, sizeof c->uzdoom_exe, "HaloDoomBridge/uzdoom/uzdoom");
#endif
    hdb_ini_copy(c->iwad, sizeof c->iwad, "HaloDoomBridge/freedoom2.wad");
    hdb_ini_copy(c->halodoom_pk3, sizeof c->halodoom_pk3, "HaloDoomBridge/HaloDoom.pk3");
    hdb_ini_copy(c->bridge_pk3, sizeof c->bridge_pk3, "HaloDoomBridge/HaloDoomBridge.pk3");
    hdb_ini_copy(c->doom_config, sizeof c->doom_config, "HaloDoomBridge/uzdoom-hdbridge.ini");
    c->doom_units_per_wu = 80.f;
    c->outgoing_damage_scale = 1.f;
    c->incoming_damage_scale = 1.f;
    c->proxy_radius_wu = 60.f;
    c->key_rgb = 0xFF00FF;
    c->key_tolerance = 24;
    /* Esc (start), E (X: action), ` (console), F1 (back), F11, F12 */
    {
        static const uint16_t keys[] = { 0x01, 0x12, 0x29, 0x3B, 0x57, 0x58 };
        int i;
        for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); ++i) c->halo_keys[i] = keys[i];
        c->halo_key_count = (int)(sizeof keys / sizeof keys[0]);
    }
}

/* Returns 1 if the file was read. Unknown keys are ignored. */
static int hdb_config_read(hdb_config* c, const char* path) {
    char line[1024], section[64] = "";
    FILE* f = fopen(path, "r");
    int first = 1;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq, *semi, *key, *val;
        if (first) {
            first = 0;
            /* Notepad's "UTF-8 with BOM": skip the mark */
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                memmove(line, line + 3, strlen(line + 3) + 1);
            /* UTF-16 ("Unicode" in Notepad) can't be read: say so */
            else if (((unsigned char)line[0] == 0xFF && (unsigned char)line[1] == 0xFE) ||
                     ((unsigned char)line[0] == 0xFE && (unsigned char)line[1] == 0xFF)) {
                fclose(f);
                return -1;
            }
        }
        semi = strchr(line, ';');
        if (semi) *semi = 0;
        hdb_ini_trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            char* close = strchr(line, ']');
            if (close) { *close = 0; hdb_ini_copy(section, sizeof section, line + 1); }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = line; val = eq + 1;
        hdb_ini_trim(key); hdb_ini_trim(val);

        if (hdb_ini_same(section, "Doom")) {
            if (hdb_ini_same(key, "bStartDoom")) c->start_doom = atoi(val);
            else if (hdb_ini_same(key, "bShowDoomWindow")) c->doom_visible = atoi(val);
            else if (hdb_ini_same(key, "sUZDoom")) hdb_ini_copy(c->uzdoom_exe, sizeof c->uzdoom_exe, val);
            else if (hdb_ini_same(key, "sIWAD")) hdb_ini_copy(c->iwad, sizeof c->iwad, val);
            else if (hdb_ini_same(key, "sHaloDoom")) hdb_ini_copy(c->halodoom_pk3, sizeof c->halodoom_pk3, val);
            else if (hdb_ini_same(key, "sBridgePk3")) hdb_ini_copy(c->bridge_pk3, sizeof c->bridge_pk3, val);
            else if (hdb_ini_same(key, "sDoomConfig")) hdb_ini_copy(c->doom_config, sizeof c->doom_config, val);
            else if (hdb_ini_same(key, "sExtraArgs")) hdb_ini_copy(c->extra_args, sizeof c->extra_args, val);
        } else if (hdb_ini_same(section, "Scale")) {
            if (hdb_ini_same(key, "fDoomUnitsPerWU")) c->doom_units_per_wu = (float)atof(val);
            else if (hdb_ini_same(key, "fOutgoingDamage")) c->outgoing_damage_scale = (float)atof(val);
            else if (hdb_ini_same(key, "fIncomingDamage")) c->incoming_damage_scale = (float)atof(val);
            else if (hdb_ini_same(key, "fProxyRadiusWU")) c->proxy_radius_wu = (float)atof(val);
        } else if (hdb_ini_same(section, "Overlay")) {
            if (hdb_ini_same(key, "sKeyRGB")) c->key_rgb = (uint32_t)strtoul(val, NULL, 16);
            else if (hdb_ini_same(key, "iKeyTolerance")) c->key_tolerance = (uint32_t)atoi(val);
        } else if (hdb_ini_same(section, "Input")) {
            if (hdb_ini_same(key, "sHaloKeys")) {
                char* p = val;
                c->halo_key_count = 0;
                while (*p && c->halo_key_count < HDB_MAX_RESERVED_KEYS) {
                    char* end;
                    unsigned long v = strtoul(p, &end, 16);
                    if (end == p) { ++p; continue; }
                    c->halo_keys[c->halo_key_count++] = (uint16_t)v;
                    p = end;
                }
            }
        } else if (hdb_ini_same(section, "DamageTypes")) {
            if (hdb_ini_same(key, "default"))
                hdb_ini_copy(c->default_damage_effect_path, sizeof c->default_damage_effect_path, val);
            else if (c->damage_type_count < HDB_MAX_DAMAGE_TYPES) {
                int i = c->damage_type_count++;
                hdb_ini_copy(c->damage_type_name[i], sizeof c->damage_type_name[i], key);
                hdb_ini_copy(c->damage_effect_path[i], sizeof c->damage_effect_path[i], val);
            }
        }
    }
    fclose(f);
    return 1;
}

#endif
