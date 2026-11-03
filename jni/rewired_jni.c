//-------------------------------------------------------------------
// The Java side's way in. A recipe crosses the boundary as its BSH1 record -
// the same bytes a .BND sidecar holds - so Java never needs to know the
// structs, and every edit goes through the engine's own sanitize, random and
// naming functions rather than a Java copy of them.
//-------------------------------------------------------------------

#include <jni.h>
#include <string.h>
#include <stdio.h>
#include "../native/recipe.h"
#include "../native/pipeline.h"
#include "../native/log.h"
#include "../native/mexp.h"

#define CLS(name) Java_com_rewired_a6000_Native_##name

static char last_err[256];

//-------------------------------------------------------------------

static int get_recipe(JNIEnv *env, jbyteArray a, rw_recipe_t *r)
{
    jsize n;
    unsigned char buf[RW_REC_MAX];
    if (!a) { rw_recipe_reset(r); return 0; }
    n = (*env)->GetArrayLength(env, a);
    if (n > (jsize)sizeof(buf)) n = sizeof(buf);
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)buf);
    if (!rw_recipe_unpack(buf, n, r)) { rw_recipe_reset(r); return 0; }
    return 1;
}

static jbyteArray put_recipe(JNIEnv *env, const rw_recipe_t *r)
{
    unsigned char buf[RW_REC_MAX];
    int n = rw_recipe_pack(r, buf, sizeof(buf));
    jbyteArray a = (*env)->NewByteArray(env, n);
    if (a) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)buf);
    return a;
}

static jobjectArray strings(JNIEnv *env, char lines[][48], int n)
{
    jclass sc = (*env)->FindClass(env, "java/lang/String");
    jobjectArray out = (*env)->NewObjectArray(env, n, sc, 0);
    int i;
    for (i = 0; i < n && out; i++)
    {
        jstring s = (*env)->NewStringUTF(env, lines[i]);
        (*env)->SetObjectArrayElement(env, out, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}

//-------------------------------------------------------------------
// Sources in the order the wheel walks them: each data pin from the top,
// the same pins inverted, the two rails, then the buses and their inverses.

#define PALETTE_MAX (2 * BEND_MAX_BITS + 2 + 2 * BUS_COUNT)

static int palette(int nbits, unsigned char *p)
{
    int n = 0, i;
    for (i = nbits - 1; i >= 0; i--) p[n++] = (unsigned char)BSRC_DATA(i);
    for (i = nbits - 1; i >= 0; i--) p[n++] = (unsigned char)BSRC_NDATA(i);
    p[n++] = BSRC_LOW;
    p[n++] = BSRC_HIGH;
    for (i = 0; i < BUS_COUNT; i++) p[n++] = (unsigned char)BSRC_BUS(i);
    for (i = 0; i < BUS_COUNT; i++) p[n++] = (unsigned char)BSRC_NBUS(i);
    return n;
}

static int step(int v, int dir, int max)        // wrapping, 0..max
{
    v += dir;
    if (v < 0) v = max;
    if (v > max) v = 0;
    return v;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Matches Native.java.
enum
{
    OP_ROUTE = 1, OP_ROUTE_STRAIGHT, OP_RANDOM, OP_MUTATE, OP_RESET_MATRIX,
    OP_TOGGLE_BEND, OP_TOGGLE_BENDX,
    OP_X_KIND, OP_X_AMOUNT, OP_X_REACH, OP_X_TINT, OP_X_LOCK, OP_X_DROP, OP_X_RANDOM,
    OP_SEG_LAYOUT, OP_SEG_ACTIVE, OP_SEG_SIZE, OP_TRASH, OP_SIMPLIFY, OP_RESET_ALL
};

JNIEXPORT jbyteArray JNICALL CLS(edit)(JNIEnv *env, jclass c, jbyteArray rec, jint op, jint a, jint b)
{
    rw_recipe_t r;
    bend_t *m;
    bendx_t *x;
    unsigned char pal[PALETTE_MAX];
    int n, i, k;

    get_recipe(env, rec, &r);
    m = rw_recipe_seg(&r, r.segs.active);
    x = bendx_chain_live(&r.bendx);

    switch (op)
    {
    case OP_ROUTE:                  // a = output bit, b = direction
        if (a < 0 || a >= m->nbits) break;
        n = palette(m->nbits, pal);
        for (i = 0, k = 0; i < n; i++) if (pal[i] == m->route[a]) { k = i; break; }
        m->route[a] = pal[step(k, b, n - 1)];
        r.bend_on = 1;
        break;
    case OP_ROUTE_STRAIGHT:
        if (a >= 0 && a < m->nbits) m->route[a] = (unsigned char)BSRC_DATA(a);
        break;
    case OP_RANDOM:                 // a = seed, b = depth | 0x100 to allow buses
        bend_random(m, (unsigned)a, b & 0xff, (b & 0x100) ? 1 : 0);
        r.bend_on = 1;
        break;
    case OP_MUTATE:
        bend_mutate(m, (unsigned)a, (b & 0x100) ? 1 : 0);
        r.bend_on = 1;
        break;
    case OP_RESET_MATRIX:
        bend_reset(m, RW_NBITS);
        break;
    case OP_TOGGLE_BEND:  r.bend_on  = !r.bend_on;  break;
    case OP_TOGGLE_BENDX: r.bendx_on = !r.bendx_on; break;
    case OP_X_KIND:
        if (!x) break;
        bendx_set_kind(x, step(x->kind, a, BX_COUNT - 1));
        r.bendx_on = 1;
        break;
    case OP_X_AMOUNT:
        if (x) x->amount = (unsigned char)clampi(x->amount + a, 0, bendx_amount_max(x->kind));
        break;
    case OP_X_REACH:
        if (x) x->reach = (unsigned char)clampi(x->reach + a, 0, bendx_reach_max(x->kind));
        break;
    case OP_X_TINT:
        if (x && bendx_tint_max(x->kind) > 0) x->tint = (unsigned char)step(x->tint, a, bendx_tint_max(x->kind));
        break;
    case OP_X_LOCK:
        bendx_chain_lock(&r.bendx);
        break;
    case OP_X_DROP:                 // the live one if it is on, else the last locked
        if (x && !bendx_is_off(x)) bendx_reset(x);
        else if (r.bendx.n > 0) bendx_chain_remove(&r.bendx, r.bendx.n - 1);
        break;
    case OP_X_RANDOM:
        if (x && !bendx_is_off(x)) bendx_random(x, (unsigned)a);
        break;
    case OP_SEG_LAYOUT:
        r.segs.layout = (unsigned char)step(r.segs.layout, a, BSEG_COUNT - 1);
        r.segs.active = 0;
        break;
    case OP_SEG_ACTIVE:
        r.segs.active = (unsigned char)step(r.segs.active, a, bend_seg_count(r.segs.layout) - 1);
        break;
    case OP_SEG_SIZE:
        r.segs.size = (unsigned char)clampi(r.segs.size + a * 5, 5, 100);
        break;
    case OP_TRASH:
        m->trash_type = (unsigned char)step(m->trash_type, a, TRASH_COUNT - 1);
        break;
    case OP_SIMPLIFY:
        bend_simplify(m);
        break;
    case OP_RESET_ALL:
        rw_recipe_reset(&r);
        break;
    }
    rw_recipe_sanitize(&r);
    return put_recipe(env, &r);
}

JNIEXPORT jbyteArray JNICALL CLS(defaultRecipe)(JNIEnv *env, jclass c)
{
    rw_recipe_t r;
    rw_recipe_reset(&r);
    return put_recipe(env, &r);
}

//-------------------------------------------------------------------
// What the screen draws.

// The active segment's twelve output bits, top first.
JNIEXPORT jobjectArray JNICALL CLS(routes)(JNIEnv *env, jclass c, jbyteArray rec)
{
    rw_recipe_t r;
    char lines[BEND_MAX_BITS][48];
    bend_t *m;
    int i, n;
    get_recipe(env, rec, &r);
    m = rw_recipe_seg(&r, r.segs.active);
    n = m->nbits;
    for (i = 0; i < n; i++) bend_src_name(m->route[n - 1 - i], lines[i]);
    return strings(env, lines, n);
}

// One line per fact, in a fixed order the Java side indexes:
//   0 bend on/off   1 chain on/off   2 layout   3 active segment label
//   4 segment count 5 size (or "-")  6 trash    7 live effect name
//   8 live amount/reach/colour        9 chain summary   10 "1" if anything bends
JNIEXPORT jobjectArray JNICALL CLS(describe)(JNIEnv *env, jclass c, jbyteArray rec)
{
    rw_recipe_t r;
    char l[11][48], tmp[16];
    bendx_t *x;
    bend_t *m;
    int i, n;

    get_recipe(env, rec, &r);
    m = rw_recipe_seg(&r, r.segs.active);
    x = bendx_chain_live(&r.bendx);
    memset(l, 0, sizeof(l));

    strcpy(l[0], r.bend_on ? "on" : "off");
    strcpy(l[1], r.bendx_on ? "on" : "off");
    snprintf(l[2], 48, "%s", bend_seg_name(r.segs.layout));
    snprintf(l[3], 48, "%s", bend_seg_label(r.segs.layout, r.segs.active));
    snprintf(l[4], 48, "%d", bend_seg_count(r.segs.layout));
    if (bend_seg_has_size(r.segs.layout)) snprintf(l[5], 48, "%d%%", r.segs.size);
    else strcpy(l[5], "-");
    snprintf(l[6], 48, "%s", bend_trash_names[m->trash_type % TRASH_COUNT]);

    if (!x) { strcpy(l[7], "(chain full)"); strcpy(l[8], ""); }
    else if (bendx_is_off(x)) { strcpy(l[7], "Off"); strcpy(l[8], ""); }
    else
    {
        snprintf(l[7], 48, "%s", bendx_name(x->kind));
        bendx_label(x, tmp);
        snprintf(l[8], 48, "%s  a%d r%d  %s", tmp, x->amount, x->reach, bendx_tint_name(x));
    }

    n = bendx_chain_count(&r.bendx);
    l[9][0] = 0;
    for (i = 0; i < (int)r.bendx.n && i < BENDX_CHAIN_MAX; i++)
    {
        if (i) strncat(l[9], " > ", 47 - strlen(l[9]));
        strncat(l[9], bendx_name(r.bendx.item[i].kind), 47 - strlen(l[9]));
    }
    if (!l[9][0]) strcpy(l[9], n ? "" : "-");
    strcpy(l[10], rw_recipe_active(&r) ? "1" : "0");
    return strings(env, l, 11);
}

JNIEXPORT jstring JNICALL CLS(effectDesc)(JNIEnv *env, jclass c, jbyteArray rec)
{
    rw_recipe_t r;
    bendx_t *x;
    get_recipe(env, rec, &r);
    x = bendx_chain_live(&r.bendx);
    return (*env)->NewStringUTF(env, (x && !bendx_is_off(x)) ? bendx_desc(x->kind) : "");
}

JNIEXPORT jstring JNICALL CLS(tag)(JNIEnv *env, jclass c, jbyteArray rec)
{
    static char buf[4096];
    rw_recipe_t r;
    get_recipe(env, rec, &r);
    rw_recipe_tag(&r, buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

//-------------------------------------------------------------------
// Files.

JNIEXPORT jbyteArray JNICALL CLS(loadPreset)(JNIEnv *env, jclass c, jstring path)
{
    rw_recipe_t r;
    const char *p = (*env)->GetStringUTFChars(env, path, 0);
    int ok = rw_preset_load(p, &r, 0, 0) || rw_recipe_from_file(p, &r);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return ok ? put_recipe(env, &r) : 0;
}

JNIEXPORT jstring JNICALL CLS(presetName)(JNIEnv *env, jclass c, jstring path)
{
    rw_recipe_t r;
    char name[RW_NAME_MAX + 1], desc[RW_DESC_MAX + 1];
    const char *p = (*env)->GetStringUTFChars(env, path, 0);
    int ok = rw_preset_load(p, &r, name, desc);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return (*env)->NewStringUTF(env, ok ? name : "");
}

JNIEXPORT jboolean JNICALL CLS(savePreset)(JNIEnv *env, jclass c, jstring path, jbyteArray rec, jstring name)
{
    rw_recipe_t r;
    const char *p = (*env)->GetStringUTFChars(env, path, 0);
    const char *nm = name ? (*env)->GetStringUTFChars(env, name, 0) : 0;
    int ok;
    get_recipe(env, rec, &r);
    ok = rw_preset_save(p, &r, nm, "saved on the a6000");
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (nm) (*env)->ReleaseStringUTFChars(env, name, nm);
    return ok ? JNI_TRUE : JNI_FALSE;
}

//-------------------------------------------------------------------
// The work. Runs on the caller's thread; the UI polls progress().

static const char *opt(JNIEnv *env, jstring s) { return s ? (*env)->GetStringUTFChars(env, s, 0) : 0; }
static void rel(JNIEnv *env, jstring s, const char *p) { if (s && p) (*env)->ReleaseStringUTFChars(env, s, p); }

JNIEXPORT jint JNICALL CLS(processMulti)(JNIEnv *env, jclass c, jstring in, jstring outArw, jstring outJpg,
                                         jstring outBnd, jstring rom, jbyteArray rec, jint seed, jint frame,
                                         jobjectArray earlier, jint mode, jboolean afterBend)
{
    rw_recipe_t r;
    rw_job_t job;
    jstring js[RW_MEXP_MAX - 1];
    int rc, i, n = earlier ? (*env)->GetArrayLength(env, earlier) : 0;

    if (n > RW_MEXP_MAX - 1) n = RW_MEXP_MAX - 1;
    get_recipe(env, rec, &r);
    memset(&job, 0, sizeof(job));
    job.in_arw = opt(env, in);
    job.out_arw = opt(env, outArw);
    job.out_jpg = opt(env, outJpg);
    job.out_bnd = opt(env, outBnd);
    job.rom_path = opt(env, rom);
    job.jpeg_quality = 92;
    job.seed = (unsigned)seed;
    job.frame = (unsigned)frame;
    for (i = 0; i < n; i++)
    {
        js[i] = (jstring)(*env)->GetObjectArrayElement(env, earlier, i);
        job.mexp_in[i] = opt(env, js[i]);
    }
    job.mexp_n = n;
    job.mexp_mode = mode;
    job.mexp_after_bend = afterBend ? 1 : 0;

    last_err[0] = 0;
    rc = rw_process(&job, &r, last_err);

    rel(env, in, job.in_arw);
    rel(env, outArw, job.out_arw);
    rel(env, outJpg, job.out_jpg);
    rel(env, outBnd, job.out_bnd);
    rel(env, rom, job.rom_path);
    for (i = 0; i < n; i++) rel(env, js[i], job.mexp_in[i]);
    return rc;
}

JNIEXPORT jint JNICALL CLS(process)(JNIEnv *env, jclass c, jstring in, jstring outArw, jstring outJpg,
                                    jstring outBnd, jstring rom, jbyteArray rec, jint seed, jint frame)
{
    return CLS(processMulti)(env, c, in, outArw, outJpg, outBnd, rom, rec, seed, frame, 0, 0, JNI_FALSE);
}

// The card log: REWIRED/LOG.TXT. Java writes its own lines through log().
JNIEXPORT void JNICALL CLS(setLog)(JNIEnv *env, jclass c, jstring path)
{
    const char *p = opt(env, path);
    rw_log_open(p);
    rel(env, path, p);
}

JNIEXPORT void JNICALL CLS(log)(JNIEnv *env, jclass c, jstring line)
{
    const char *p = opt(env, line);
    if (p) rw_log("%s", p);
    rel(env, line, p);
}

JNIEXPORT void JNICALL CLS(logMem)(JNIEnv *env, jclass c, jstring when)
{
    const char *p = opt(env, when);
    rw_log_mem(p ? p : "");
    rel(env, when, p);
}

JNIEXPORT jstring JNICALL CLS(lastError)(JNIEnv *env, jclass c) { return (*env)->NewStringUTF(env, last_err); }
JNIEXPORT jint JNICALL CLS(progress)(JNIEnv *env, jclass c) { return rw_progress; }
JNIEXPORT jint JNICALL CLS(stage)(JNIEnv *env, jclass c) { return rw_stage; }
