package com.rewired.a6000;

/**
 * The ghost: the exposures of a multiple exposure taken so far, held over the live view while the next one is lined
 * up. CHDK_Rewired's (core/mexp_ghost.c) is five greys in a checkerboard, because a PowerShot's overlay is a fixed
 * palette with nothing half transparent in it; the a6000's is an Android view with real alpha, so this one is the
 * picture itself, in colour, blended the way the sequence will blend.
 *
 * Kept small - a 480x320 picture, three ints a pixel - and blended in linear light like the raws are, so ADD
 * brightens and AVERAGE darkens on screen as they will in the file. A preview of the composite, not the composite:
 * it is made from JPEGs, which have already been white balanced and toned, so it is close rather than exact.
 *
 * Plain Java on int arrays, with no Android in it, so test/ can run it on a desktop.
 */
final class Ghost {
    static final int W = 480, H = 320;

    // the order of RW_MEXP_* in native/mexp.h
    static final int ADD = 0, LIGHTEN = 1, DARKEN = 2, AVERAGE = 3, MODES = 4;
    static final String[] MODE_NAMES = { "ADD", "LIGHTEN", "DARKEN", "AVERAGE" };

    private static final int[] TO_LIN = new int[256];          // sRGB 0..255 -> linear 0..4095
    private static final int[] FROM_LIN = new int[4096];       // and back
    static {
        for (int i = 0; i < 256; i++) {
            double v = i / 255.0;
            v = v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
            TO_LIN[i] = (int) Math.round(v * 4095);
        }
        for (int i = 0; i < 4096; i++) {
            double v = i / 4095.0;
            v = v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
            FROM_LIN[i] = (int) Math.round(Math.max(0, Math.min(1, v)) * 255);
        }
    }

    final int mode;
    private final int[] acc = new int[W * H * 3];
    private int n;

    Ghost(int mode) { this.mode = mode >= 0 && mode < MODES ? mode : AVERAGE; }

    /** exposures in it so far */
    int count() { return n; }

    /** one more exposure, as W*H ARGB pixels */
    void add(int[] argb) {
        for (int i = 0, j = 0; i < W * H; i++) {
            int p = argb[i];
            for (int sh = 16; sh >= 0; sh -= 8, j++) {
                int v = TO_LIN[(p >> sh) & 255];
                if (n == 0) { acc[j] = v; continue; }
                switch (mode) {
                    case LIGHTEN: if (v > acc[j]) acc[j] = v; break;
                    case DARKEN: if (v < acc[j]) acc[j] = v; break;
                    default: acc[j] += v;                       // ADD and AVERAGE both sum; composite() tells them apart
                }
            }
        }
        n++;
    }

    /** the composite so far, as W*H opaque ARGB pixels */
    int[] composite() {
        int[] out = new int[W * H];
        int div = mode == AVERAGE && n > 0 ? n : 1;
        for (int i = 0, j = 0; i < W * H; i++) {
            int p = 0xff000000;
            for (int sh = 16; sh >= 0; sh -= 8, j++) {
                int v = acc[j] / div;
                p |= FROM_LIN[v > 4095 ? 4095 : v] << sh;
            }
            out[i] = p;
        }
        return out;
    }
}
