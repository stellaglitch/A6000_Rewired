//-------------------------------------------------------------------
// rewire - the a6000 pipeline on a desktop.
//
// Two jobs. It is the test bench: the same C the camera runs, over a real
// ARW, with checks a camera cannot make. And it is a tool in its own right:
// bend ARWs already on a computer, with presets from either camera.
//
//   rewire IN.ARW [options]
//
//   -o OUT.ARW        write the bent raw
//   -j OUT.JPG        write the developed picture
//   -b OUT.BND        write the shot record (BSH1 sidecar)
//   -p FILE           load a recipe: a BENDnn.BND preset, a .BND sidecar, or
//                     any picture either camera tagged
//   -r SEED           random matrix (with -d DEPTH, default 3; -B allows buses)
//   -x NAME[:A[:R]]   add an experimental profile, amount A, reach R (repeat)
//   -s LAYOUT         segment layout by name ("quarters", "circle", ...)
//   -S SEED           pattern seed (layouts that move between shots)
//   -q QUALITY        JPEG quality, default 92
//   -m ROMFILE        file read as the ROM bus
//   -l LOGFILE        append the pipeline's log, as the camera writes REWIRED/LOG.TXT
//   -F                hold the whole frame even for a matrix-only bend (tests)
//   -w PRESET.BND     save the recipe as a preset and exit
//   -e EARLIER.ARW    multiple exposure: an earlier exposure to blend with IN
//                     (repeat, up to 8); IN is the last and gives the header
//   -M MODE           its blend: add, lighten, darken, average (default)
//   -A                blend after the bend (the earlier files are bent ARWs)
//   --mexp-selftest   check the four blends against their definitions
//   --list            list the effects and layouts
//   --info            print what is in the ARW
//   --selftest        check the codec against the file: decode, re-encode,
//                     compare, every row
//-------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../native/pipeline.h"
#include "../native/arw.h"
#include "../native/log.h"
#include "../native/mexp.h"

static int name_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int find_effect(const char *name)
{
    int k;
    for (k = 1; k < BX_COUNT; k++) if (name_eq(bendx_name(k), name)) return k;
    return -1;
}

static int find_layout(const char *name)
{
    int k;
    for (k = 0; k < BSEG_COUNT; k++) if (name_eq(bend_seg_name(k), name)) return k;
    return -1;
}

static void list(void)
{
    int k;
    printf("experimental profiles (-x):\n");
    for (k = 1; k < BX_COUNT; k++)
        printf("  %-14s amount 0..%-3d reach 0..%-3d %s\n", bendx_name(k),
               bendx_amount_max(k), bendx_reach_max(k), bendx_desc(k));
    printf("segment layouts (-s):\n");
    for (k = 0; k < BSEG_COUNT; k++) printf("  %s\n", bend_seg_name(k));
}

//-------------------------------------------------------------------

static int info(const char *path)
{
    arw_info_t *a = (arw_info_t *)calloc(1, sizeof(*a));
    char err[256];
    FILE *f = fopen(path, "rb");
    unsigned char *row;
    unsigned short *codes;
    unsigned x, y;
    double sum_l = 0, sum_r = 0;
    unsigned nl = 0, nr = 0, maxv = 0, minv = 4095;

    if (!f || !a) { printf("cannot open %s\n", path); return 1; }
    if (arw_open(f, a, err) != 0) { printf("%s: %s\n", path, err); return 1; }
    printf("model        %s\n", a->model);
    printf("raw          %u x %u, strip at %lu, %lu bytes\n", a->width, a->height, a->strip_off, a->strip_len);
    printf("curve        %u %u %u %u %u %u\n", a->curve_pts[0], a->curve_pts[1], a->curve_pts[2], a->curve_pts[3], a->curve_pts[4], a->curve_pts[5]);
    printf("SR2Private   %s\n", a->has_sr2 ? "decrypted" : "not read - defaults in use");
    printf("black/white  %u / %u (12-bit linear)\n", a->black, a->white);
    printf("as-shot WB   R %u  G %u  G %u  B %u\n", a->wb[0], a->wb[1], a->wb[2], a->wb[3]);
    printf("preview      %lu bytes at %lu\n", a->prev_len, a->prev_off);
    printf("thumbnail    %lu bytes at %lu\n", a->thumb_len, a->thumb_off);

    // Where the masked margin is: the mean of the outermost columns.
    row = (unsigned char *)malloc(a->width);
    codes = (unsigned short *)malloc(a->width * 2);
    fseek(f, (long)a->strip_off, SEEK_SET);
    for (y = 0; y < a->height; y++)
    {
        if (fread(row, 1, a->width, f) != a->width) break;
        if (y % 16) continue;
        arw_decode_row(row, a->width, codes);
        for (x = 0; x < 16; x++) { sum_l += a->lin[codes[x]]; nl++; }
        for (x = a->width - 16; x < a->width; x++) { sum_r += a->lin[codes[x]]; nr++; }
        for (x = 0; x < a->width; x++)
        {
            unsigned v = a->lin[codes[x]];
            if (v > maxv) maxv = v;
            if (v < minv) minv = v;
        }
    }
    printf("left 16 cols  mean %.1f\n", sum_l / (nl ? nl : 1));
    printf("right 16 cols mean %.1f\n", sum_r / (nr ? nr : 1));
    printf("range         %u .. %u\n", minv, maxv);
    fclose(f);
    return 0;
}

// Decode every row, re-encode it with every block forced through the
// encoder, decode that, and compare the codes. The encoder is lossy only
// where Sony's is - deltas rounded to the block's step - so this counts how
// many pixels move and by how much, and fails if any block does not survive
// the trip it should survive exactly: every block whose range fits in 7 bits.
static int selftest(const char *path)
{
    arw_info_t *a = (arw_info_t *)calloc(1, sizeof(*a));
    char err[256];
    FILE *f = fopen(path, "rb");
    unsigned char *row, *enc;
    unsigned short *c0, *c1, *dummy;
    unsigned long moved = 0, total = 0, exact_fail = 0, maxerr = 0, identical = 0;
    unsigned x, y, v;
    int fails = 0;

    if (!f || !a) { printf("cannot open %s\n", path); return 1; }
    if (arw_open(f, a, err) != 0) { printf("%s: %s\n", path, err); return 1; }

    // The curve: every code to linear and back lands on a code with the same
    // linear value.
    for (v = 0; v < 2048; v++)
        if (a->lin[a->code[a->lin[v]]] != a->lin[v]) { printf("FAIL curve: code %u\n", v); fails++; break; }

    row = (unsigned char *)malloc(a->width);
    enc = (unsigned char *)malloc(a->width);
    c0 = (unsigned short *)malloc(a->width * 2);
    c1 = (unsigned short *)malloc(a->width * 2);
    dummy = (unsigned short *)malloc(a->width * 2);
    fseek(f, (long)a->strip_off, SEEK_SET);
    for (y = 0; y < a->height; y++)
    {
        if (fread(row, 1, a->width, f) != a->width) { printf("FAIL short strip\n"); return 1; }
        arw_decode_row(row, a->width, c0);

        // Unchanged codes must reproduce the original bytes exactly.
        arw_encode_row(c0, a->width, c0, row, enc);
        if (memcmp(enc, row, a->width) != 0) { printf("FAIL passthrough row %u\n", y); fails++; break; }

        // Forced re-encode: a reference that can never match.
        for (x = 0; x < a->width; x++) dummy[x] = 0xffff;
        arw_encode_row(c0, a->width, dummy, row, enc);
        if (memcmp(enc, row, a->width) == 0) identical++;
        arw_decode_row(enc, a->width, c1);
        for (x = 0; x < a->width; x++)
        {
            unsigned long e = c0[x] > c1[x] ? c0[x] - c1[x] : c1[x] - c0[x];
            total++;
            if (e) moved++;
            if (e > maxerr) maxerr = e;
        }
        // Blocks with a range under 128 are stored exactly by Sony's format.
        for (x = 0; x + 31 < a->width; x += 32)
        {
            unsigned h, i;
            for (h = 0; h < 2; h++)
            {
                unsigned mx = 0, mn = 0x7ff;
                for (i = 0; i < 16; i++) { unsigned c = c0[x + h + 2 * i]; if (c > mx) mx = c; if (c < mn) mn = c; }
                if (mx - mn < 128)
                    for (i = 0; i < 16; i++)
                        if (c0[x + h + 2 * i] != c1[x + h + 2 * i]) { exact_fail++; break; }
            }
        }
    }
    printf("rows re-encoded byte-identical: %lu of %u\n", identical, a->height);
    printf("pixels moved by a forced re-encode: %lu of %lu (max %lu codes)\n", moved, total, maxerr);
    if (exact_fail) { printf("FAIL %lu exact-range blocks changed\n", exact_fail); fails++; }
    printf(fails ? "SELFTEST FAILED\n" : "SELFTEST OK\n");
    return fails ? 1 : 0;
}

// The four blends against what they are meant to be, on every pair and a few
// triples of a spread of values, black 128 and white 3840 as the a6000's.
static int mexp_selftest(void)
{
    static const unsigned short vals[] = { 0, 100, 128, 129, 500, 1000, 2047, 3000, 3839, 3840, 4095 };
    const unsigned nv = sizeof(vals) / sizeof(vals[0]), black = 128, white = 3840;
    unsigned i, j, k, fails = 0;
    int mode;

    for (mode = 0; mode < RW_MEXP_MODES; mode++)
        for (i = 0; i < nv; i++)
            for (j = 0; j < nv; j++)
                for (k = 0; k <= nv; k++)
                {
                    // k == nv: a pair; otherwise a triple
                    unsigned short r0[1] = { vals[i] }, r1[1] = { vals[j] }, r2[1] = { k < nv ? vals[k] : 0 }, out[1];
                    const unsigned short *rows[3] = { r0, r1, r2 };
                    int n = k < nv ? 3 : 2, t;
                    unsigned want, v[3] = { vals[i], vals[j], k < nv ? vals[k] : 0 };

                    switch (mode)
                    {
                    case RW_MEXP_LIGHTEN: want = v[0]; for (t = 1; t < n; t++) if (v[t] > want) want = v[t]; break;
                    case RW_MEXP_DARKEN:  want = v[0]; for (t = 1; t < n; t++) if (v[t] < want) want = v[t]; break;
                    case RW_MEXP_ADD:
                        want = black;
                        for (t = 0; t < n; t++) want += v[t] > black ? v[t] - black : 0;
                        if (want > white) want = white;
                        break;
                    default:
                        want = 0;
                        for (t = 0; t < n; t++) want += v[t];
                        want = (want + n / 2) / n;
                    }
                    // dst aliasing rows[0] is how the pipeline calls it
                    rw_mexp_blend(mode, r0, rows, n, 1, black, white);
                    out[0] = r0[0];
                    if (out[0] != want)
                    {
                        if (fails++ < 5) printf("FAIL %s %u %u %u -> %u, want %u\n", rw_mexp_name(mode),
                                                v[0], v[1], v[2], out[0], want);
                    }
                }
    printf(fails ? "MEXP SELFTEST FAILED (%u)\n" : "MEXP SELFTEST OK\n", fails);
    return fails ? 1 : 0;
}

//-------------------------------------------------------------------

int main(int argc, char **argv)
{
    rw_recipe_t r;
    rw_job_t job;
    const char *in = 0, *save_preset = 0;
    char err[256];
    int i, rc, xcount = 0, depth = 3, allow_bus = 0;
    long rseed = -1;

    rw_recipe_reset(&r);
    memset(&job, 0, sizeof(job));
    job.jpeg_quality = 92;
    job.seed = 1;
    job.frame = 1;
    job.mexp_mode = RW_MEXP_AVERAGE;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : 0;
        if (!strcmp(a, "--list")) { list(); return 0; }
        else if (!strcmp(a, "--info") && v) return info(v);
        else if (!strcmp(a, "--selftest") && v) return selftest(v);
        else if (!strcmp(a, "--mexp-selftest")) return mexp_selftest();
        else if (!strcmp(a, "-e") && v)
        {
            if (job.mexp_n >= RW_MEXP_MAX - 1) { printf("a sequence holds %d exposures at most\n", RW_MEXP_MAX); return 1; }
            job.mexp_in[job.mexp_n++] = v; i++;
        }
        else if (!strcmp(a, "-M") && v)
        {
            int m;
            for (m = 0; m < RW_MEXP_MODES && !name_eq(v, rw_mexp_name(m)); m++) {}
            if (m == RW_MEXP_MODES) { printf("no blend called \"%s\": add, lighten, darken, average\n", v); return 1; }
            job.mexp_mode = m; i++;
        }
        else if (!strcmp(a, "-A")) job.mexp_after_bend = 1;
        else if (!strcmp(a, "-o") && v) { job.out_arw = v; i++; }
        else if (!strcmp(a, "-j") && v) { job.out_jpg = v; i++; }
        else if (!strcmp(a, "-b") && v) { job.out_bnd = v; i++; }
        else if (!strcmp(a, "-m") && v) { job.rom_path = v; i++; }
        else if (!strcmp(a, "-q") && v) { job.jpeg_quality = atoi(v); i++; }
        else if (!strcmp(a, "-S") && v) { job.seed = (unsigned)strtoul(v, 0, 0); i++; }
        else if (!strcmp(a, "-d") && v) { depth = atoi(v); i++; }
        else if (!strcmp(a, "-B")) allow_bus = 1;
        else if (!strcmp(a, "-F")) job.force_frame = 1;
        else if (!strcmp(a, "-l") && v) { rw_log_open(v); i++; }
        else if (!strcmp(a, "-r") && v) { rseed = strtol(v, 0, 0); i++; }
        else if (!strcmp(a, "-w") && v) { save_preset = v; i++; }
        else if (!strcmp(a, "-p") && v)
        {
            char name[32], desc[96];
            if (rw_preset_load(v, &r, name, desc))
                printf("preset %s%s%s\n", v, name[0] ? ": " : "", name);
            else if (rw_recipe_from_file(v, &r))
                printf("recipe read from %s\n", v);
            else { printf("%s holds no recipe this build can read\n", v); return 1; }
            i++;
        }
        else if (!strcmp(a, "-s") && v)
        {
            int l = find_layout(v);
            if (l < 0) { printf("no layout called \"%s\" (--list)\n", v); return 1; }
            r.segs.layout = (unsigned char)l;
            bend_seg_sanitize(&r.segs, RW_NBITS);
            i++;
        }
        else if (!strcmp(a, "-x") && v)
        {
            char nm[64], *c1, *c2;
            bendx_t *x;
            int k;
            strncpy(nm, v, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
            c1 = strchr(nm, ':'); if (c1) *c1++ = 0;
            c2 = c1 ? strchr(c1, ':') : 0; if (c2) *c2++ = 0;
            k = find_effect(nm);
            if (k < 0) { printf("no effect called \"%s\" (--list)\n", nm); return 1; }
            if (xcount >= BENDX_CHAIN_MAX) { printf("the chain holds %d at most\n", BENDX_CHAIN_MAX); return 1; }
            r.bendx.n = (unsigned char)xcount;
            x = &r.bendx.item[xcount];
            bendx_set_kind(x, k);
            if (c1) x->amount = (unsigned char)atoi(c1);
            if (c2) x->reach = (unsigned char)atoi(c2);
            bendx_sanitize(x);
            bendx_chain_lock(&r.bendx);
            xcount++;
            r.bendx_on = 1;
            i++;
        }
        else if (a[0] != '-' && !in) in = a;
        else { printf("rewire: what is \"%s\"? See the comment at the top of host/rewire.c.\n", a); return 2; }
    }

    if (rseed >= 0)
    {
        bend_random(&r.bend, (unsigned)rseed, depth, allow_bus);
        r.bend_on = 1;
    }
    rw_recipe_sanitize(&r);

    if (save_preset)
    {
        if (!rw_preset_save(save_preset, &r, "rewire", "saved by the desktop tool")) { printf("cannot write %s\n", save_preset); return 1; }
        printf("saved %s\n", save_preset);
        return 0;
    }
    if (!in) { printf("usage: rewire IN.ARW [-o OUT.ARW] [-j OUT.JPG] [-p PRESET] [-r SEED] [-x EFFECT[:A[:R]]] ...\n"); return 2; }

    {
        char tag[4096];
        rw_recipe_tag(&r, tag, sizeof(tag));
        printf("%s", tag);
    }

    job.in_arw = in;
    rc = rw_process(&job, &r, err);
    if (rc) { printf("failed: %s\n", err); return 1; }
    printf("done\n");
    return 0;
}
