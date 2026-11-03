//-------------------------------------------------------------------
// Recipes, presets and picture tags - see recipe.h.
//
// The pack/unpack and the preset layout are CHDK_Rewired's (core/bend_shot.c
// and core/bend_store.c) with the CHDK file calls swapped for stdio. They must
// stay byte-compatible with it: that is the whole point of having them.
//-------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recipe.h"

#define BSH_HDR     16
#define BSH_HDR_V1  12
#define BSH_VER     3
#define BSH_F_BEND  1
#define BSH_F_BENDX 2

#define BND_HDR     8
#define BND_VER     3

#define TAG_MARK    "@rewired_optics"

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static unsigned get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

//-------------------------------------------------------------------

void rw_recipe_reset(rw_recipe_t *r)
{
    memset(r, 0, sizeof(*r));
    bend_reset(&r->bend, RW_NBITS);
    bendx_chain_reset(&r->bendx);
    bend_seg_sanitize(&r->segs, RW_NBITS);
    r->bend_on = 1;
    r->bendx_on = 1;
}

void rw_recipe_sanitize(rw_recipe_t *r)
{
    bend_sanitize(&r->bend, RW_NBITS);
    bend_seg_sanitize(&r->segs, RW_NBITS);
    bendx_chain_sanitize(&r->bendx);
    r->bend_on = r->bend_on ? 1 : 0;
    r->bendx_on = r->bendx_on ? 1 : 0;
}

bend_t *rw_recipe_seg(rw_recipe_t *r, int seg)
{
    if (seg <= 0 || seg >= bend_seg_count(r->segs.layout)) return &r->bend;
    return &r->segs.b[seg - 1];
}

int rw_recipe_active(const rw_recipe_t *r)
{
    int i, n = bend_seg_count(r->segs.layout);
    if (r->bendx_on && bendx_chain_count(&r->bendx) > 0) return 1;
    if (!r->bend_on) return 0;
    for (i = 0; i < n; i++)
        if (!bend_is_identity(i ? &r->segs.b[i - 1] : &r->bend)) return 1;
    return 0;
}

//-------------------------------------------------------------------
// BSH1

int rw_recipe_pack(const rw_recipe_t *r, unsigned char *out, int outlen)
{
    int n;
    if (outlen < (int)RW_REC_MAX) return 0;
    memcpy(out, "BSH1", 4);
    put16(out + 4,  BSH_VER);
    put16(out + 6,  sizeof(bend_t));
    put16(out + 8,  sizeof(bendx_chain_t));
    put16(out + 10, (r->bend_on ? BSH_F_BEND : 0) | (r->bendx_on ? BSH_F_BENDX : 0));
    put16(out + 12, sizeof(bend_segs_t));
    put16(out + 14, 0);
    n = BSH_HDR;
    memcpy(out + n, &r->bend,  sizeof(bend_t));         n += sizeof(bend_t);
    memcpy(out + n, &r->bendx, sizeof(bendx_chain_t));  n += sizeof(bendx_chain_t);
    memcpy(out + n, &r->segs,  sizeof(bend_segs_t));    n += sizeof(bend_segs_t);
    return n;
}

// Records from before version 3 used the original 24-entry effect list.
static void upgrade_bendx(bendx_chain_t *c, int ver)
{
    int i;
    if (ver >= 3) return;
    for (i = 0; i < BENDX_CHAIN_MAX; i++)
    {
        unsigned k = c->item[i].kind;
        if (k == 12 || k == 13 || k == 17 || k == 19 || k == 20) c->item[i].kind = BX_OFF;
        else if (k >= 21 && k <= 23) c->item[i].kind = (unsigned char)(k - 5);
        else if (k >= 18) c->item[i].kind = (unsigned char)(k - 3);
        else if (k >= 14) c->item[i].kind = (unsigned char)(k - 2);
    }
}

int rw_recipe_unpack(const unsigned char *rec, int len, rw_recipe_t *r)
{
    rw_recipe_t t;
    int ver, at;
    unsigned flags;

    if (!rec || len < BSH_HDR_V1 || memcmp(rec, "BSH1", 4) != 0) return 0;
    if (get16(rec + 6) != sizeof(bend_t) || get16(rec + 8) != sizeof(bendx_chain_t)) return 0;

    memset(&t, 0, sizeof(t));
    ver = (int)get16(rec + 4);
    if (ver == 1)
    {
        at = BSH_HDR_V1;
        if (len < at + (int)(sizeof(bend_t) + sizeof(bendx_chain_t))) return 0;
    }
    else if (ver == 2 || ver == BSH_VER)
    {
        at = BSH_HDR;
        if (get16(rec + 12) != sizeof(bend_segs_t)) return 0;
        if (len < at + (int)(sizeof(bend_t) + sizeof(bendx_chain_t) + sizeof(bend_segs_t))) return 0;
    }
    else return 0;

    memcpy(&t.bend, rec + at, sizeof(bend_t));         at += sizeof(bend_t);
    memcpy(&t.bendx, rec + at, sizeof(bendx_chain_t)); at += sizeof(bendx_chain_t);
    if (ver >= 2) memcpy(&t.segs, rec + at, sizeof(bend_segs_t));

    flags = get16(rec + 10);
    t.bend_on  = (flags & BSH_F_BEND)  ? 1 : 0;
    t.bendx_on = (flags & BSH_F_BENDX) ? 1 : 0;
    upgrade_bendx(&t.bendx, ver);
    rw_recipe_sanitize(&t);
    *r = t;
    return 1;
}

//-------------------------------------------------------------------
// BND1 presets: header, bend_t, then (version 2) a 25 + 81 byte name and
// description, then (version 3) the chain, the segments and the two switches.

int rw_preset_load(const char *path, rw_recipe_t *r, char *name, char *desc)
{
    unsigned char hdr[BND_HDR], info[RW_NAME_MAX + 1 + RW_DESC_MAX + 1], on[4];
    rw_recipe_t t;
    FILE *f = fopen(path, "rb");
    int ok = 0, ver;

    if (name) name[0] = 0;
    if (desc) desc[0] = 0;
    if (!f) return 0;

    rw_recipe_reset(&t);
    if (fread(hdr, 1, BND_HDR, f) == BND_HDR && memcmp(hdr, "BND1", 4) == 0 &&
        get16(hdr + 6) == sizeof(bend_t))
    {
        ver = (int)get16(hdr + 4);
        if (ver >= 1 && ver <= BND_VER && fread(&t.bend, 1, sizeof(bend_t), f) == sizeof(bend_t))
        {
            ok = 1;
            t.bend_on = 1;
            t.bendx_on = 0;
            if (ver >= 2)
            {
                ok = fread(info, 1, sizeof(info), f) == sizeof(info);
                if (ok && name) { memcpy(name, info, RW_NAME_MAX); name[RW_NAME_MAX] = 0; }
                if (ok && desc) { memcpy(desc, info + RW_NAME_MAX + 1, RW_DESC_MAX); desc[RW_DESC_MAX] = 0; }
            }
            if (ok && ver >= 3)
            {
                ok = fread(&t.bendx, 1, sizeof(bendx_chain_t), f) == sizeof(bendx_chain_t)
                  && fread(&t.segs, 1, sizeof(bend_segs_t), f) == sizeof(bend_segs_t)
                  && fread(on, 1, 4, f) == 4;
                if (ok) { t.bend_on = on[0] ? 1 : 0; t.bendx_on = on[1] ? 1 : 0; }
            }
        }
    }
    fclose(f);
    if (!ok) return 0;
    rw_recipe_sanitize(&t);
    *r = t;
    return 1;
}

int rw_preset_save(const char *path, const rw_recipe_t *r, const char *name, const char *desc)
{
    unsigned char hdr[BND_HDR], info[RW_NAME_MAX + 1 + RW_DESC_MAX + 1], on[4];
    FILE *f;
    int ok;

    memcpy(hdr, "BND1", 4);
    put16(hdr + 4, BND_VER);
    put16(hdr + 6, sizeof(bend_t));
    memset(info, 0, sizeof(info));
    if (name) strncpy((char *)info, name, RW_NAME_MAX);
    if (desc) strncpy((char *)info + RW_NAME_MAX + 1, desc, RW_DESC_MAX);
    on[0] = (unsigned char)(r->bend_on ? 1 : 0);
    on[1] = (unsigned char)(r->bendx_on ? 1 : 0);
    on[2] = on[3] = 0;

    f = fopen(path, "wb");
    if (!f) return 0;
    ok = fwrite(hdr, 1, BND_HDR, f) == BND_HDR
      && fwrite(&r->bend, 1, sizeof(bend_t), f) == sizeof(bend_t)
      && fwrite(info, 1, sizeof(info), f) == sizeof(info)
      && fwrite(&r->bendx, 1, sizeof(bendx_chain_t), f) == sizeof(bendx_chain_t)
      && fwrite(&r->segs, 1, sizeof(bend_segs_t), f) == sizeof(bend_segs_t)
      && fwrite(on, 1, 4, f) == 4;
    if (fclose(f) != 0) ok = 0;
    if (!ok) remove(path);
    return ok;
}

//-------------------------------------------------------------------
// The tag.

static int cat(char *out, int at, int outlen, const char *s)
{
    while (*s && at < outlen - 1) out[at++] = *s++;
    out[at] = 0;
    return at;
}

static int cat_matrix(char *out, int at, int outlen, const bend_t *b)
{
    char tmp[16];
    int i;
    for (i = b->nbits - 1; i >= 0; i--)
    {
        sprintf(tmp, "%d:", i);
        at = cat(out, at, outlen, tmp);
        bend_src_name(b->route[i], tmp);
        at = cat(out, at, outlen, tmp);
        at = cat(out, at, outlen, i ? " " : "");
    }
    return at;
}

int rw_recipe_tag(const rw_recipe_t *r, char *out, int outlen)
{
    unsigned char rec[RW_REC_MAX];
    char tmp[64];
    int at = 0, i, n, len;

    out[0] = 0;
    at = cat(out, at, outlen, TAG_MARK "\n");
    if (r->bend_on)
    {
        n = bend_seg_count(r->segs.layout);
        for (i = 0; i < n; i++)
        {
            at = cat(out, at, outlen, "bend ");
            if (n > 1)
            {
                at = cat(out, at, outlen, bend_seg_label(r->segs.layout, i));
                at = cat(out, at, outlen, " ");
            }
            at = cat_matrix(out, at, outlen, i ? &r->segs.b[i - 1] : &r->bend);
            at = cat(out, at, outlen, "\n");
        }
        if (n > 1)
        {
            at = cat(out, at, outlen, "seg ");
            at = cat(out, at, outlen, bend_seg_name(r->segs.layout));
            at = cat(out, at, outlen, "\n");
        }
    }
    if (r->bendx_on && bendx_chain_count(&r->bendx) > 0)
    {
        at = cat(out, at, outlen, "x ");
        n = bendx_chain_count(&r->bendx);
        for (i = 0; i < n; i++)
        {
            at = cat(out, at, outlen, i ? " > " : "");
            at = cat(out, at, outlen, bendx_name(r->bendx.item[i].kind));
            bendx_label(&r->bendx.item[i], tmp);
            at = cat(out, at, outlen, " ");
            at = cat(out, at, outlen, tmp);
        }
        at = cat(out, at, outlen, "\n");
    }

    len = rw_recipe_pack(r, rec, sizeof(rec));
    at = cat(out, at, outlen, "BSH1:");
    for (i = 0; i < len && at < outlen - 3; i++)
    {
        sprintf(tmp, "%02x", rec[i]);
        at = cat(out, at, outlen, tmp);
    }
    at = cat(out, at, outlen, "\n");
    return at;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int rw_recipe_from_file(const char *path, rw_recipe_t *r)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf, rec[RW_REC_MAX];
    long size, win, i;
    int ok = 0;

    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);

    // A sidecar is the record itself.
    if (size >= BSH_HDR_V1 && size <= (long)RW_REC_MAX + 64)
    {
        buf = (unsigned char *)malloc((size_t)size);
        fseek(f, 0, SEEK_SET);
        if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size)
            ok = rw_recipe_unpack(buf, (int)size, r);
        free(buf);
        if (ok) { fclose(f); return 1; }
    }

    // Otherwise a tag: the comment CHDK_Rewired appends after the EOI, or the
    // one this app writes after the SOI. Look in both ends of the file.
    win = size < 65536 ? size : 65536;
    buf = (unsigned char *)malloc((size_t)win + 1);
    if (!buf) { fclose(f); return 0; }
    for (i = 0; i < 2 && !ok; i++)
    {
        long from = i ? size - win : 0, k;
        fseek(f, from, SEEK_SET);
        if (fread(buf, 1, (size_t)win, f) != (size_t)win) continue;
        buf[win] = 0;
        for (k = 0; k + 5 < win && !ok; k++)
            if (memcmp(buf + k, "BSH1:", 5) == 0)
            {
                long p = k + 5;
                int n = 0, hi, lo;
                while (p + 1 < win && n < (int)sizeof(rec) &&
                       (hi = hexval(buf[p])) >= 0 && (lo = hexval(buf[p + 1])) >= 0)
                {
                    rec[n++] = (unsigned char)((hi << 4) | lo);
                    p += 2;
                }
                ok = rw_recipe_unpack(rec, n, r);
            }
    }
    free(buf);
    fclose(f);
    return ok;
}
