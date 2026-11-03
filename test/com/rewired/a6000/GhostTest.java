package com.rewired.a6000;

/** Ghost's four blends on flat pictures, with the bare JDK like CardTest. */
public class GhostTest {
    static int fails;

    static int[] flat(int grey) {
        int[] p = new int[Ghost.W * Ghost.H];
        java.util.Arrays.fill(p, 0xff000000 | grey * 0x010101);
        return p;
    }

    static int grey(Ghost g) { return g.composite()[Ghost.W * Ghost.H / 2] & 255; }

    static void check(String what, int got, int lo, int hi) {
        if (got < lo || got > hi) { System.out.println("FAIL " + what + ": " + got + " not in " + lo + ".." + hi); fails++; }
    }

    static Ghost two(int mode, int a, int b) {
        Ghost g = new Ghost(mode);
        g.add(flat(a));
        g.add(flat(b));
        return g;
    }

    public static void main(String[] args) {
        Ghost one = new Ghost(Ghost.AVERAGE);
        one.add(flat(100));
        check("one exposure is itself", grey(one), 99, 101);
        check("lighten", grey(two(Ghost.LIGHTEN, 40, 200)), 199, 201);
        check("darken", grey(two(Ghost.DARKEN, 40, 200)), 39, 41);
        // In linear light: the mean of black and white is 50% light, which is sRGB 188, not 128.
        check("average", grey(two(Ghost.AVERAGE, 0, 255)), 186, 190);
        check("average of equals", grey(two(Ghost.AVERAGE, 150, 150)), 149, 151);
        check("add of two mid greys brightens", grey(two(Ghost.ADD, 128, 128)), 170, 180);
        check("add clips", grey(two(Ghost.ADD, 255, 200)), 255, 255);
        check("count", two(Ghost.ADD, 1, 2).count(), 2, 2);
        System.out.println(fails == 0 ? "GHOST TEST OK" : "GHOST TEST FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
