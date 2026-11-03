package com.rewired.a6000;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The memory card: where the camera puts its raws, where this app puts its output and presets.
 *
 * Sony's card layer takes DOS 8.3 names only (Recipe Lab found this out: a nine-letter folder could not be created
 * on an A6000), so every name here is eight characters or fewer.
 *
 * <pre>
 *   DCIM/100MSDCF/DSC01542.ARW     the camera's own shot, never written to
 *   REWIRED/RW01542.ARW            the bent raw: develop it on a computer
 *   REWIRED/RW01542.JPG            the bent picture, developed in the camera, tagged with its recipe
 *   REWIRED/RW01542.BND            the recipe on its own, the same record CHDK_Rewired writes
 *   REWIRED/BENDS/BEND00.BND ..    saved presets, the same files as CHDK/BENDS on a PowerShot
 * </pre>
 *
 * No Android in here, so it can be checked on a desktop.
 */
final class Card {
    private Card() {}

    static final String OUT_DIR = "REWIRED";
    static final String BENDS_DIR = "BENDS";
    /** where a PowerShot running CHDK_Rewired keeps its presets: read too, so a card moved across works */
    static final String CHDK_BENDS = "CHDK/BENDS";

    /** the newest ARW under DCIM, by modification time then name; null if there is none */
    static File newestArw(File root) {
        File best = null;
        File[] dirs = new File(root, "DCIM").listFiles();
        if (dirs == null) return null;
        for (File d : dirs) {
            File[] fs = d.isDirectory() ? d.listFiles() : null;
            if (fs == null) continue;
            for (File f : fs) {
                if (!f.getName().toUpperCase().endsWith(".ARW")) continue;
                if (best == null || f.lastModified() > best.lastModified()
                        || (f.lastModified() == best.lastModified() && f.getName().compareTo(best.getName()) > 0)) best = f;
            }
        }
        return best;
    }

    /** "DSC01542.ARW" -> "RW01542": the camera's number kept, so the two are easy to pair */
    static String outBase(String camName) {
        String n = camName;
        int dot = n.lastIndexOf('.');
        if (dot >= 0) n = n.substring(0, dot);
        StringBuilder digits = new StringBuilder();
        for (int i = n.length() - 1; i >= 0 && digits.length() < 5 && Character.isDigit(n.charAt(i)); i--) digits.insert(0, n.charAt(i));
        while (digits.length() < 5) digits.insert(0, '0');
        return "RW" + digits;
    }

    /** a base name not already taken in dir: RW01542, then RW01542A, RW01542B ... for a re-bend of the same shot */
    static String freeBase(File dir, String base) {
        if (!new File(dir, base + ".ARW").exists() && !new File(dir, base + ".JPG").exists()) return base;
        for (char c = 'A'; c <= 'Z'; c++) {
            String b = base + c;
            if (!new File(dir, b + ".ARW").exists() && !new File(dir, b + ".JPG").exists()) return b;
        }
        return base + "Z";
    }

    /** every BENDnn.BND in the app's folder and in CHDK's, sorted by path */
    static List<File> presets(File root) {
        List<File> out = new ArrayList<File>();
        for (String dir : new String[] { OUT_DIR + "/" + BENDS_DIR, CHDK_BENDS }) {
            File[] fs = new File(root, dir).listFiles();
            if (fs == null) continue;
            for (File f : fs) if (slot(f.getName()) >= 0) out.add(f);
        }
        Collections.sort(out);
        return out;
    }

    /** NN of BENDNN.BND, or -1 */
    static int slot(String name) {
        String u = name.toUpperCase();
        if (u.length() != 10 || !u.startsWith("BEND") || !u.endsWith(".BND")) return -1;
        char a = u.charAt(4), b = u.charAt(5);
        if (!Character.isDigit(a) || !Character.isDigit(b)) return -1;
        return (a - '0') * 10 + (b - '0');
    }

    /** the first free BENDnn.BND in the app's own preset folder; null when all hundred are used */
    static File nextPresetFile(File root) {
        File dir = new File(root, OUT_DIR + "/" + BENDS_DIR);
        for (int i = 0; i < 100; i++) {
            File f = new File(dir, "BEND" + (i < 10 ? "0" : "") + i + ".BND");
            if (!f.exists()) return f;
        }
        return null;
    }
}
