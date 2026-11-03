package com.rewired.a6000;

/**
 * librewired.so: CHDK_Rewired's bend engine, the ARW codec and the pipeline (jni/rewired_jni.c).
 *
 * A recipe is passed around as its BSH1 record - the bytes a .BND sidecar holds - and every change to one is made on
 * the native side by {@link #edit}, so the engine's own rules (sanitising, the random generator, names) are the only
 * ones there are.
 */
final class Native {
    private Native() {}

    static { System.loadLibrary("rewired"); }

    // edit() operations; the numbers match the enum in rewired_jni.c
    static final int OP_ROUTE = 1, OP_ROUTE_STRAIGHT = 2, OP_RANDOM = 3, OP_MUTATE = 4, OP_RESET_MATRIX = 5,
            OP_TOGGLE_BEND = 6, OP_TOGGLE_BENDX = 7,
            OP_X_KIND = 8, OP_X_AMOUNT = 9, OP_X_REACH = 10, OP_X_TINT = 11, OP_X_LOCK = 12, OP_X_DROP = 13, OP_X_RANDOM = 14,
            OP_SEG_LAYOUT = 15, OP_SEG_ACTIVE = 16, OP_SEG_SIZE = 17, OP_TRASH = 18, OP_SIMPLIFY = 19, OP_RESET_ALL = 20;

    /** OP_RANDOM / OP_MUTATE: or into b to let the generator patch in clocks, noise and held samples */
    static final int ALLOW_BUS = 0x100;

    // describe() lines
    static final int D_BEND_ON = 0, D_BENDX_ON = 1, D_LAYOUT = 2, D_SEG_LABEL = 3, D_SEG_COUNT = 4, D_SEG_SIZE = 5,
            D_TRASH = 6, D_X_NAME = 7, D_X_KNOBS = 8, D_CHAIN = 9, D_ACTIVE = 10;

    // stage() values, from pipeline.h
    static final int STAGE_IDLE = 0, STAGE_READ = 1, STAGE_BEND = 2, STAGE_BENDX = 3, STAGE_WRITE = 4, STAGE_DONE = 5;

    static native byte[] defaultRecipe();
    static native byte[] edit(byte[] recipe, int op, int a, int b);

    /** the active segment's output bits, top bit first, each as its source's short name ("d9", "~d7", "NZ", "LO") */
    static native String[] routes(byte[] recipe);
    static native String[] describe(byte[] recipe);
    static native String effectDesc(byte[] recipe);
    /** the comment a picture is tagged with: the bend as text, then the record in hex */
    static native String tag(byte[] recipe);

    /** a BENDnn.BND preset, a .BND sidecar, or any tagged picture; null if none of those */
    static native byte[] loadPreset(String path);
    static native String presetName(String path);
    static native boolean savePreset(String path, byte[] recipe, String name);

    /** 0 on success; otherwise {@link #lastError()} says why. Blocks: run it off the UI thread. */
    static native int process(String inArw, String outArw, String outJpg, String outBnd, String romPath,
                              byte[] recipe, int seed, int frame);
    static native String lastError();
    /** REWIRED/LOG.TXT: every line opened, written and closed alone, so a crash loses nothing before it */
    static native void setLog(String path);
    static native void log(String line);
    static native void logMem(String when);
    /** 0..1000 */
    static native int progress();
    static native int stage();
}
