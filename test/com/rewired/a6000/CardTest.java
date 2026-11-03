package com.rewired.a6000;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;

/**
 * Card's naming and scanning, on a desktop folder laid out like the a6000's card. No JUnit on purpose: it runs with
 * the bare JDK, the way tools/test.ps1 calls it.
 */
public class CardTest {
    static int fails;

    static void eq(Object want, Object got, String what) {
        if (want == null ? got != null : !want.equals(got)) { System.out.println("FAIL " + what + ": want " + want + ", got " + got); fails++; }
    }

    static File touch(File f, long time) throws Exception {
        f.getParentFile().mkdirs();
        new FileOutputStream(f).close();
        f.setLastModified(time);
        return f;
    }

    public static void main(String[] a) throws Exception {
        eq("RW01542", Card.outBase("DSC01542.ARW"), "outBase");
        eq("RW00007", Card.outBase("DSC7.ARW"), "outBase short");
        eq("RW00000", Card.outBase("NONUM.ARW"), "outBase none");
        eq(7, Card.slot("BEND07.BND"), "slot");
        eq(7, Card.slot("bend07.bnd"), "slot lower");
        eq(-1, Card.slot("BEND7.BND"), "slot short");
        eq(-1, Card.slot("BENDXY.BND"), "slot letters");

        File root = Files.createTempDirectory("card").toFile();
        eq(null, Card.newestArw(root), "empty card");
        touch(new File(root, "DCIM/100MSDCF/DSC00001.ARW"), 1000000);
        File newest = touch(new File(root, "DCIM/100MSDCF/DSC00002.ARW"), 2000000);
        touch(new File(root, "DCIM/100MSDCF/DSC00003.JPG"), 3000000);
        eq(newest, Card.newestArw(root), "newest ARW ignores JPEG");

        File out = new File(root, Card.OUT_DIR);
        eq("RW00002", Card.freeBase(out, "RW00002"), "free");
        touch(new File(out, "RW00002.JPG"), 1000);
        eq("RW00002A", Card.freeBase(out, "RW00002"), "taken");

        eq(0, Card.presets(root).size(), "no presets");
        touch(new File(root, "CHDK/BENDS/BEND03.BND"), 1000);
        touch(new File(root, "REWIRED/BENDS/BEND00.BND"), 1000);
        touch(new File(root, "REWIRED/BENDS/NOTES.TXT"), 1000);
        eq(2, Card.presets(root).size(), "presets from both folders");
        eq("BEND01.BND", Card.nextPresetFile(root).getName(), "next free slot");

        System.out.println(fails == 0 ? "CardTest OK" : "CardTest FAILED (" + fails + ")");
        System.exit(fails == 0 ? 0 : 1);
    }
}
