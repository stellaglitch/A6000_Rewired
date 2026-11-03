package com.rewired.a6000;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.Camera;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.io.File;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Rewired: CHDK_Rewired's circuit bending for the Sony a6000.
 *
 * The a6000 cannot be patched the way a PowerShot can - its imaging pipeline is not ours to hook - so the bend happens
 * one step later, on the raw the camera has just written: the shutter fires, the ARW lands on the card, and its sensor
 * data goes through the same bend matrix and experimental chain CHDK_Rewired runs inside a PowerShot's capture path.
 * The camera's own file is never touched; the bent raw, a developed JPEG and the recipe go into REWIRED/ (see {@link Card}).
 *
 * Multiple exposure (the MULTI row) is CHDK_Rewired's: two to nine shots blended into one, with the ones taken so far
 * held over the live view as a ghost to line the next one up against. See {@link Ghost} and native/mexp.h.
 *
 * Keys - on every body Recipe Lab lists them for:
 *   UP / DOWN       row: matrix, scope, chain, multi, preset
 *   LEFT / RIGHT    field in the row (on the matrix, which output bit)
 *   wheel / dial    change the field (on the matrix, patch a new source into the bit)
 *   centre          the row's action: matrix reroll, scope mutate, chain lock, preset load / save
 *   trash           the row's undo: straight wire, clear the region, drop an effect
 *   Fn              hide / show the panel
 *   shutter         take the picture and bend it
 *   MENU            leave (or close the bent picture)
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    // scan codes, com.sony.scalar.sysutil.ScalarInput (as Recipe Lab's Keys)
    static final int K_UP = 103, K_DOWN = 108, K_LEFT = 105, K_RIGHT = 106, K_ENTER = 232, K_MENU = 514, K_SK1 = 229,
            K_DELETE = 595, K_SK2 = 513, K_FN = 520, K_S1 = 516, K_S2 = 518, K_PLAY = 207,
            K_WHEEL_CW = 522, K_WHEEL_CCW = 523, K_DIAL_CW = 525, K_DIAL_CCW = 526,
            K_DIAL2_CW = 528, K_DIAL2_CCW = 529, K_DIAL3_CW = 635, K_DIAL3_CCW = 634;

    /** read as the ROM bus: some of the camera's own machine code, as CHDK_Rewired reads the PowerShot's ROM */
    static final String ROM_PATH = "/system/lib/libc.so";
    static final long WAIT_MS = 25000, POLL_MS = 400;
    /** the live quality parameter and the one value the app shoots with (Recipe Lab's runtime keys) */
    static final String STORAGE_FMT = "storage-fmt", RAW_JPEG = "rawjpeg";
    /** the quality the camera had before the app changed it, put back on leaving; null if untouched */
    private String origFmt;

    private final Handler handler = new Handler();
    private SharedPreferences prefs;
    private PatchView view;
    private SurfaceHolder holder;
    private Object cameraEx;
    private Camera camera;

    private byte[] recipe;
    private int depth = 3;
    private boolean allowBus;
    /** write the bent ARW too; off saves the 24 MB write, the slowest part on a slow card */
    private boolean saveArw = true;
    private int frame;
    private int presetIdx;
    private List<File> presetFiles;

    // multiple exposure settings, the MULTI row
    private boolean mexpOn, mexpBendEach = true;
    private int mexpMode = Ghost.AVERAGE, mexpShots = 2, ghostIdx = 2, screen = PatchView.SCREEN_LCD;
    static final int[] GHOST_ALPHA = { 0, 64, 128, 192 };
    static final String[] GHOST_NAMES = { "Off", "25%", "50%", "75%" };

    // A sequence part way through. The mode, count and bend setting are latched when it starts, as CHDK_Rewired's
    // mexp_begin() latches them: changing them half way would finish a composite under rules it was not begun with.
    private final List<File> seqParts = new ArrayList<File>();     // the earlier exposures, in the order taken
    private final List<File> seqTemp = new ArrayList<File>();      // bent ARWs kept only for the composite
    private int seqTarget, seqMode;
    private boolean seqBendEach;
    private Ghost ghost;

    // what a bend is for
    static final int SHOT_SINGLE = 0, SHOT_EXPOSURE = 1, SHOT_LAST = 2;

    // a shot in flight
    private boolean busy, waiting, swallowUp;
    private File before;
    private long waitStart, lastSize = -1;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getPreferences(MODE_PRIVATE);
        recipe = fromHex(prefs.getString("recipe", null));
        if (recipe == null) recipe = Native.defaultRecipe();
        recipe = Native.edit(recipe, 0, 0, 0);          // sanitised, whatever was stored
        depth = prefs.getInt("depth", 3);
        allowBus = prefs.getBoolean("allowBus", false);
        saveArw = prefs.getBoolean("saveArw", true);
        frame = prefs.getInt("frame", 0);
        mexpOn = prefs.getBoolean("mexpOn", false);
        mexpBendEach = prefs.getBoolean("mexpBendEach", true);
        mexpMode = clamp(prefs.getInt("mexpMode", Ghost.AVERAGE), 0, Ghost.MODES - 1);
        mexpShots = clamp(prefs.getInt("mexpShots", 2), 2, 9);
        ghostIdx = clamp(prefs.getInt("ghost", 2), 0, GHOST_ALPHA.length - 1);
        screen = clamp(prefs.getInt("screen", PatchView.SCREEN_LCD), 0, PatchView.SCREEN_NAMES.length - 1);

        FrameLayout root = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        view = new PatchView(this);
        root.addView(sv);
        root.addView(view);
        setContentView(root);
        holder = sv.getHolder();
        holder.setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);

        // A Java crash goes into the card log before the platform's handler takes the app down.
        final Thread.UncaughtExceptionHandler was = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            public void uncaughtException(Thread th, Throwable t) {
                try {
                    java.io.StringWriter sw = new java.io.StringWriter();
                    t.printStackTrace(new java.io.PrintWriter(sw));
                    Native.log("JAVA CRASH in " + th.getName() + ": " + sw);
                } catch (Throwable ignored) {}
                if (was != null) was.uncaughtException(th, t);
            }
        });
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Throwable t) { return "?"; }
    }

    /** the card log, REWIRED/LOG.TXT; a log that cannot be written never stops the app */
    private static void log(String s) { try { Native.log(s); } catch (Throwable t) {} }

    private static void openLog() {
        File root = card();
        if (root == null) { Native.setLog(null); return; }
        File dir = new File(root, Card.OUT_DIR);
        dir.mkdirs();
        File f = new File(dir, "LOG.TXT");
        if (f.length() > 256 * 1024) {                  // one older file kept, as Recipe Lab's logs do
            File old = new File(dir, "LOG1.TXT");
            old.delete();
            f.renameTo(old);
        }
        Native.setLog(f.getPath());
    }

    @Override
    protected void onResume() {
        super.onResume();
        openLog();
        log("OPEN Rewired " + versionName() + ", Android " + android.os.Build.VERSION.RELEASE
                + ", max Java heap " + (Runtime.getRuntime().maxMemory() >> 20) + " MB");
        try { Native.logMem("on open"); } catch (Throwable t) {}
        try {
            Class<?> cx = Class.forName("com.sony.scalar.hardware.CameraEx");
            Method open = cx.getMethod("open", int.class, Class.forName("com.sony.scalar.hardware.CameraEx$OpenOptions"));
            cameraEx = open.invoke(null, 0, null);
            camera = (Camera) cx.getMethod("getNormalCamera").invoke(cameraEx);
            holder.addCallback(this);
        } catch (Throwable t) {
            camera = null;
            status("Camera did not open: " + t);
            log("camera did not open: " + t);
        }
        presetFiles = card() == null ? null : Card.presets(card());
        refresh();
    }

    @Override
    protected void onPause() {
        super.onPause();
        log("PAUSE" + (busy ? " while bending - the bend thread runs on" : waiting ? " while waiting for the raw" : ""));
        handler.removeCallbacks(poll);
        waiting = false;
        save();
        holder.removeCallback(this);
        restoreQuality();
        try { if (camera != null) camera.stopPreview(); } catch (Throwable t) {}
        try { if (cameraEx != null) cameraEx.getClass().getMethod("release").invoke(cameraEx); } catch (Throwable t) {}
        cameraEx = null; camera = null;
    }

    public void surfaceCreated(SurfaceHolder h) {
        try { camera.setPreviewDisplay(h); camera.startPreview(); } catch (Throwable t) { status("Live view did not start: " + t); }
    }
    public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
    public void surfaceDestroyed(SurfaceHolder h) {}

    private void save() {
        prefs.edit().putString("recipe", toHex(recipe)).putInt("depth", depth).putBoolean("allowBus", allowBus).putBoolean("saveArw", saveArw)
                .putInt("frame", frame).putBoolean("mexpOn", mexpOn).putBoolean("mexpBendEach", mexpBendEach)
                .putInt("mexpMode", mexpMode).putInt("mexpShots", mexpShots).putInt("ghost", ghostIdx).putInt("screen", screen)
                .commit();
    }

    private static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

    private static File card() {
        try {
            return Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState()) ? Environment.getExternalStorageDirectory() : null;
        } catch (Throwable t) { return null; }
    }

    // ------------------------------------------------------------ the screen

    private void status(String s) { view.status = s; view.invalidate(); }

    /** everything PatchView draws, rebuilt from the recipe */
    private void refresh() {
        String[] d = Native.describe(recipe);
        view.routes = Native.routes(recipe);
        view.desc = d;

        boolean segs = !"1".equals(d[Native.D_SEG_COUNT]);
        view.fields[PatchView.ROW_SCOPE] = new String[] {
                "BEND", d[Native.D_BEND_ON],
                "LAYOUT", d[Native.D_LAYOUT],
                "REGION", segs ? d[Native.D_SEG_LABEL] : "-",
                "SIZE", d[Native.D_SEG_SIZE],
                "TRASH", d[Native.D_TRASH],
                "DEPTH", String.valueOf(depth),
                "BUSES", allowBus ? "yes" : "no",
                "SAVE", saveArw ? "JPG+ARW" : "JPG" };

        String[] knobs = d[Native.D_X_KNOBS].split("  ");
        String ar = knobs.length > 1 ? knobs[1] : "";
        String amount = "-", reach = "-";
        for (String k : ar.split(" ")) {
            if (k.startsWith("a")) amount = k.substring(1);
            if (k.startsWith("r")) reach = k.substring(1);
        }
        view.fields[PatchView.ROW_CHAIN] = new String[] {
                "EFFECT", d[Native.D_X_NAME],
                "AMOUNT", amount,
                "REACH", reach,
                "COLOUR", knobs.length > 2 ? knobs[2] : "-",
                "CHAIN", d[Native.D_BENDX_ON],
                "LOCKED", d[Native.D_CHAIN] };

        boolean seq = inSequence();
        view.fields[PatchView.ROW_MULTI] = new String[] {
                "MULTI", mexpOn ? "on" : "off",
                "BLEND", Ghost.MODE_NAMES[seq ? seqMode : mexpMode],
                "SHOTS", String.valueOf(seq ? seqTarget : mexpShots),
                "BEND", (seq ? seqBendEach : mexpBendEach) ? "each" : "composite",
                "GHOST", GHOST_NAMES[ghostIdx],
                "SCREEN", PatchView.SCREEN_NAMES[screen],
                "NOW", seq ? "shot " + (seqParts.size() + 1) + " of " + seqTarget : mexpOn ? "shot 1 of " + mexpShots : "-" };

        view.fields[PatchView.ROW_PRESET] = new String[] { "PRESET", presetLabel(), "CENTRE", presetAction() };

        // One style for every value: sentence case, as the engine's own names already are ("Quarters", "Lock loss").
        // Labels stay capitals, as captions; abbreviations (LCD, JPG, XOR) are left as they are.
        for (int r = PatchView.ROW_SCOPE; r < PatchView.ROWS; r++)
            for (int i = 1; i < view.fields[r].length; i += 2) view.fields[r][i] = cap(view.fields[r][i]);

        view.ghost = seq ? view.ghost : null;
        view.ghostAlpha = GHOST_ALPHA[ghostIdx];
        view.screen = screen;
        view.hint = hint();
        if (!busy && !waiting) {
            if (seq) view.status = "Multiple exposure: shot " + (seqParts.size() + 1) + " of " + seqTarget
                    + (view.ghost == null && ghostIdx > 0 ? " (no ghost - its picture would not load)" : " - line it up");
            else view.status = "1".equals(d[Native.D_ACTIVE]) ? "" : "Straight through - turn the wheel on a bit, or press centre to reroll";
        }
        view.invalidate();
    }

    private boolean inSequence() { return !seqParts.isEmpty(); }

    /** the first letter upper case, the rest as it is: "top left" -> "Top left", "(chain full)" -> "(Chain full)" */
    static String cap(String s) {
        for (int i = 0; i < s.length(); i++)
            if (Character.isLetter(s.charAt(i)))
                return s.substring(0, i) + Character.toUpperCase(s.charAt(i)) + s.substring(i + 1);
        return s;
    }

    private String hint() {
        switch (view.row) {
            case PatchView.ROW_MATRIX: return "Wheel: patch a source   Centre: reroll   Trash: straight wire   Shutter: shoot";
            case PatchView.ROW_SCOPE:  return "Wheel: change   Centre: mutate the matrix a little   Trash: reset this region";
            case PatchView.ROW_CHAIN:  return "Wheel: change   Centre: lock and start another   Trash: drop an effect";
            case PatchView.ROW_MULTI:  return inSequence() ? "Centre / Trash: cancel the sequence (shots so far stay on the card)"
                                                           : "Wheel: change   Bend: bend each shot, or only the blended result";
            default:                   return "Wheel: choose   Centre: " + presetAction();
        }
    }

    // presets: the card's BENDnn.BND files, then "Save as new"
    private int presetCount() { return presetFiles == null ? 0 : presetFiles.size(); }

    private String presetLabel() {
        if (presetIdx < presetCount()) {
            File f = presetFiles.get(presetIdx);
            String n = Native.presetName(f.getPath());
            return f.getName() + (n.length() > 0 ? " " + n : "") + (f.getPath().contains("CHDK") ? " (CHDK)" : "");
        }
        return "Save as new";
    }

    private String presetAction() { return presetIdx < presetCount() ? "load" : "save"; }

    // ------------------------------------------------------------ editing

    private int fieldCount(int row) {
        switch (row) {
            case PatchView.ROW_MATRIX: return view.routes.length;
            case PatchView.ROW_SCOPE: return 8;
            case PatchView.ROW_CHAIN: return 5;
            case PatchView.ROW_MULTI: return 6;         // NOW is shown, not set
            default: return 1;
        }
    }

    private void edit(int op, int a, int b) { recipe = Native.edit(recipe, op, a, b); refresh(); }

    /** the wheel, on whatever field is selected */
    private void turn(int dir) {
        int c = view.col;
        switch (view.row) {
            case PatchView.ROW_MATRIX:
                edit(Native.OP_ROUTE, view.routes.length - 1 - c, dir);
                return;
            case PatchView.ROW_SCOPE:
                switch (c) {
                    case 0: edit(Native.OP_TOGGLE_BEND, 0, 0); return;
                    case 1: edit(Native.OP_SEG_LAYOUT, dir, 0); return;
                    case 2: edit(Native.OP_SEG_ACTIVE, dir, 0); return;
                    case 3: edit(Native.OP_SEG_SIZE, dir, 0); return;
                    case 4: edit(Native.OP_TRASH, dir, 0); return;
                    case 5: depth = Math.max(1, Math.min(12, depth + dir)); refresh(); return;
                    case 6: allowBus = !allowBus; refresh(); return;
                    default: saveArw = !saveArw; refresh(); return;
                }
            case PatchView.ROW_CHAIN:
                switch (c) {
                    case 0: edit(Native.OP_X_KIND, dir, 0); return;
                    case 1: edit(Native.OP_X_AMOUNT, dir, 0); return;
                    case 2: edit(Native.OP_X_REACH, dir, 0); return;
                    case 3: edit(Native.OP_X_TINT, dir, 0); return;
                    default: edit(Native.OP_TOGGLE_BENDX, 0, 0); return;
                }
            case PatchView.ROW_MULTI:
                switch (c) {
                    case 0:
                        mexpOn = !mexpOn;
                        // Off part way through ends the sequence rather than freezing it, as CHDK_Rewired does.
                        if (!mexpOn && inSequence()) cancelSequence("multiple exposure switched off");
                        break;
                    case 1: mexpMode = (mexpMode + dir + Ghost.MODES) % Ghost.MODES; break;
                    case 2: mexpShots = clamp(mexpShots + dir, 2, 9); break;
                    case 3: mexpBendEach = !mexpBendEach; break;
                    case 4: ghostIdx = (ghostIdx + dir + GHOST_ALPHA.length) % GHOST_ALPHA.length; break;
                    default: screen = (screen + 1) % PatchView.SCREEN_NAMES.length; break;
                }
                refresh();
                if (inSequence() && c >= 1 && c <= 3) status("Changes to BLEND, SHOTS and BEND apply from the next sequence");
                return;
            default:
                int n = presetCount() + 1;
                presetIdx = ((presetIdx + dir) % n + n) % n;
                refresh();
        }
    }

    private int seed() { return (int) (System.currentTimeMillis() ^ (frame * 2654435761L)); }

    private void centre() {
        switch (view.row) {
            case PatchView.ROW_MATRIX: edit(Native.OP_RANDOM, seed(), depth | (allowBus ? Native.ALLOW_BUS : 0)); return;
            case PatchView.ROW_SCOPE: edit(Native.OP_MUTATE, seed(), allowBus ? Native.ALLOW_BUS : 0); return;
            case PatchView.ROW_CHAIN: edit(Native.OP_X_LOCK, 0, 0); return;
            case PatchView.ROW_MULTI: if (inSequence()) cancelSequence("cancelled"); return;
            default: presetGo();
        }
    }

    private void trash() {
        switch (view.row) {
            case PatchView.ROW_MATRIX: edit(Native.OP_ROUTE_STRAIGHT, view.routes.length - 1 - view.col, 0); return;
            case PatchView.ROW_SCOPE: edit(Native.OP_RESET_MATRIX, 0, 0); return;
            case PatchView.ROW_CHAIN: edit(Native.OP_X_DROP, 0, 0); return;
            case PatchView.ROW_MULTI: if (inSequence()) cancelSequence("cancelled"); return;
            default:
        }
    }

    private void presetGo() {
        File root = card();
        if (root == null) { status("No memory card"); return; }
        if (presetIdx < presetCount()) {
            byte[] r = Native.loadPreset(presetFiles.get(presetIdx).getPath());
            if (r == null) { status("That preset is not one this build can read"); return; }
            recipe = r;
            refresh();
            status("Loaded " + presetFiles.get(presetIdx).getName());
            return;
        }
        File f = Card.nextPresetFile(root);
        if (f == null) { status("All hundred preset slots are used"); return; }
        f.getParentFile().mkdirs();
        if (Native.savePreset(f.getPath(), recipe, "a6000 " + f.getName().substring(4, 6))) {
            presetFiles = Card.presets(root);
            presetIdx = Math.max(0, presetFiles.indexOf(f));
            refresh();
            status("Saved " + Card.OUT_DIR + "/" + Card.BENDS_DIR + "/" + f.getName());
        } else status("Could not write " + f.getName());
    }

    // ------------------------------------------------------------ the shot

    private void shoot() {
        if (busy || waiting || camera == null) return;
        File root = card();
        if (root == null) { status("No memory card"); return; }
        if (!rawPlusJpeg()) return;
        before = Card.newestArw(root);
        // Written before the call, not after: if the shutter itself takes the app down, this is the last line.
        log("SHUTTER (newest raw before: " + (before == null ? "none" : before.getName()) + ")");
        try { camera.takePicture(null, null, null); }
        catch (Throwable t) { status("The shutter refused: " + t); log("shutter refused: " + t); return; }
        log("shutter returned");
        waiting = true;
        waitStart = System.currentTimeMillis();
        lastSize = -1;
        status("Waiting for the raw...");
        handler.postDelayed(poll, POLL_MS);
    }

    /**
     * The quality the shutter fires with must be RAW+JPEG. On RAW alone, a shutter fired from an app restarts the
     * a6000 (found by Recipe Lab, its issue #100, and again here); on JPEG alone there is no raw to bend. So the live
     * parameter is set for the app's shots - the stored setting is not touched - and put back in onPause. Returns false,
     * and fires nothing, unless the camera confirms it.
     */
    private boolean rawPlusJpeg() {
        try {
            Camera.Parameters p = camera.getParameters();
            String fmt = p.get(STORAGE_FMT);
            if (RAW_JPEG.equals(fmt)) return true;
            log("quality is " + fmt + ": setting " + RAW_JPEG + " for the app's shots");
            if (fmt == null) { status("This camera does not report its Quality - not shooting"); return false; }
            if (origFmt == null) origFmt = fmt;
            p.set(STORAGE_FMT, RAW_JPEG);
            camera.setParameters(p);
            String now = camera.getParameters().get(STORAGE_FMT);
            if (RAW_JPEG.equals(now)) return true;
            log("camera kept quality " + now);
            status("Set Quality to RAW+JPEG in the camera menu - the camera would not take it from the app");
        } catch (Throwable t) {
            log("quality check failed: " + t);
            status("Set Quality to RAW+JPEG in the camera menu");
        }
        return false;
    }

    private void restoreQuality() {
        if (origFmt == null || camera == null) return;
        try {
            Camera.Parameters p = camera.getParameters();
            p.set(STORAGE_FMT, origFmt);
            camera.setParameters(p);
            log("quality put back to " + origFmt);
        } catch (Throwable t) { log("quality not put back: " + t); }
        origFmt = null;
    }

    private void cancelCapture() {
        try { if (cameraEx != null) cameraEx.getClass().getMethod("cancelTakePicture").invoke(cameraEx); } catch (Throwable t) {}
    }

    /** the camera saves on its own schedule: wait for a new ARW whose size has stopped changing */
    private final Runnable poll = new Runnable() {
        public void run() {
            if (!waiting) return;
            File root = card();
            File f = root == null ? null : Card.newestArw(root);
            boolean isNew = f != null && (before == null || !f.equals(before) || f.lastModified() > before.lastModified());
            if (isNew && f.length() > 1000000 && f.length() == lastSize) {
                waiting = false;
                log("raw arrived: " + f.getPath() + ", " + f.length() + " bytes, after "
                        + (System.currentTimeMillis() - waitStart) + " ms");
                try { if (camera != null) camera.startPreview(); } catch (Throwable t) { log("startPreview: " + t); }
                arrived(f);
                return;
            }
            lastSize = isNew ? f.length() : -1;
            if (System.currentTimeMillis() - waitStart > WAIT_MS) {
                waiting = false;
                log("no new raw within " + WAIT_MS + " ms; newest is " + (f == null ? "none" : f.getName()));
                status("No raw arrived. Check Quality is RAW+JPEG in the camera menu.");
                return;
            }
            handler.postDelayed(this, POLL_MS);
        }
    };

    /**
     * A raw has landed. Outside a multiple exposure it is bent on its own. Inside one, every exposure but the last is
     * either bent and kept for the composite (BEND each) or only noted (BEND composite); the last is bent and blended
     * with the earlier ones in a single pass, and that composite is the picture.
     */
    private void arrived(File f) {
        if (!mexpOn) { bend(f, SHOT_SINGLE); return; }
        if (!inSequence()) {
            seqTarget = mexpShots;
            seqMode = mexpMode;
            seqBendEach = mexpBendEach;
            ghost = new Ghost(seqMode);
            log("multiple exposure begins: " + seqTarget + " shots, " + Ghost.MODE_NAMES[seqMode]
                    + ", bend " + (seqBendEach ? "each" : "composite"));
        }
        int k = seqParts.size() + 1;
        log("multiple exposure shot " + k + " of " + seqTarget + ": " + f.getName());
        if (k >= seqTarget) { bend(f, SHOT_LAST); return; }
        if (seqBendEach) { bend(f, SHOT_EXPOSURE); return; }
        // Composite only: the camera's raw is the exposure, and its JPEG the ghost.
        seqParts.add(f);
        addGhost(sibling(f, ".JPG"), 8, 6);
        refresh();
    }

    private static File sibling(File f, String ext) {
        String n = f.getName();
        int dot = n.lastIndexOf('.');
        return new File(f.getParentFile(), (dot < 0 ? n : n.substring(0, dot)) + ext);
    }

    /**
     * Fold a picture into the ghost. The camera may still be writing its JPEG when the raw is done, so a picture that
     * is not there yet is tried again, a few times, half a second apart.
     */
    private void addGhost(final File jpg, final int sample, final int tries) {
        if (ghost == null || !inSequence()) return;
        Bitmap bm = null;
        try {
            if (jpg.length() > 0) {
                BitmapFactory.Options o = new BitmapFactory.Options();
                o.inSampleSize = sample;
                bm = BitmapFactory.decodeFile(jpg.getPath(), o);
            }
        } catch (Throwable t) { log("ghost decode: " + t); }
        if (bm == null) {
            if (tries > 1) handler.postDelayed(new Runnable() { public void run() { addGhost(jpg, sample, tries - 1); } }, 500);
            else { log("ghost: " + jpg.getName() + " would not load"); refresh(); }
            return;
        }
        try {
            Bitmap s = Bitmap.createScaledBitmap(bm, Ghost.W, Ghost.H, true);
            if (s != bm) bm.recycle();
            int[] px = new int[Ghost.W * Ghost.H];
            s.getPixels(px, 0, Ghost.W, 0, 0, Ghost.W, Ghost.H);
            s.recycle();
            ghost.add(px);
            Bitmap g = Bitmap.createBitmap(ghost.composite(), Ghost.W, Ghost.H, Bitmap.Config.ARGB_8888);
            if (view.ghost != null) view.ghost.recycle();
            view.ghost = g;
            log("ghost: " + ghost.count() + " exposure(s) from " + jpg.getName());
        } catch (Throwable t) { log("ghost: " + t); }
        refresh();
    }

    /** End a sequence without a composite. The exposures already taken stay on the card; only the bent ARWs kept
     * just to build the composite (SAVE JPG) go. */
    private void cancelSequence(String why) {
        log("multiple exposure " + why + " after " + seqParts.size() + " of " + seqTarget);
        endSequence();
        refresh();
        status("Multiple exposure " + why);
    }

    private void endSequence() {
        for (File f : seqTemp) f.delete();
        seqTemp.clear();
        seqParts.clear();
        ghost = null;
        if (view.ghost != null) { view.ghost.recycle(); view.ghost = null; }
    }

    private void bend(final File in, final int kind) {
        final File dir = new File(card(), Card.OUT_DIR);
        dir.mkdirs();
        final String base = Card.freeBase(dir, Card.outBase(in.getName()));
        final byte[] rec = recipe;
        final int seed = seed(), fr = ++frame;
        // An exposure bent for a composite needs its bent raw: that is what the composite is made from.
        final boolean arw = saveArw || kind == SHOT_EXPOSURE;
        final String[] earlier = new String[kind == SHOT_LAST ? seqParts.size() : 0];
        for (int i = 0; i < earlier.length; i++) earlier[i] = seqParts.get(i).getPath();
        final int mode = seqMode;
        final boolean after = seqBendEach;
        busy = true;
        save();
        view.progress = 0;
        status(kind == SHOT_LAST ? "Blending " + (earlier.length + 1) + " exposures" : "Bending " + in.getName());
        handler.post(progressTick);
        // An explicit 1 MB stack: Dalvik's default for a new thread is small, and this one runs a lot of native code.
        new Thread(null, new Runnable() {
            public void run() {
                String err;
                try {
                    int rc = Native.processMulti(in.getPath(), arw ? new File(dir, base + ".ARW").getPath() : null,
                            new File(dir, base + ".JPG").getPath(), new File(dir, base + ".BND").getPath(),
                            ROM_PATH, rec, seed, fr, earlier, mode, after);
                    err = rc == 0 ? null : Native.lastError();
                } catch (Throwable t) {
                    err = String.valueOf(t);
                    log("process threw: " + t);
                }
                final String e = err;
                handler.post(new Runnable() { public void run() { bent(dir, base, e, kind); } });
            }
        }, "bend", 1 << 20).start();
    }

    private final Runnable progressTick = new Runnable() {
        public void run() {
            if (!busy) return;
            int st = Native.stage();
            view.progress = Native.progress();
            view.status = (st == Native.STAGE_READ ? "Reading the raw" : st == Native.STAGE_BEND ? "Rewiring the bus"
                    : st == Native.STAGE_BENDX ? "Breaking the memory" : "Writing") + "  " + view.progress / 10 + "%";
            view.invalidate();
            handler.postDelayed(this, 200);
        }
    };

    private void bent(File dir, String base, String err, int kind) {
        busy = false;
        view.progress = -1;
        if (err != null) {
            if (kind != SHOT_SINGLE && inSequence()) {
                log("multiple exposure ended by a failed bend");
                endSequence();
                refresh();
                status("Bend failed, so the multiple exposure is cancelled: " + err);
            } else status("Bend failed: " + err);
            return;
        }
        if (kind == SHOT_EXPOSURE) {
            // Kept for the composite and folded into the ghost; no review, so the next shot can be lined up at once.
            File a = new File(dir, base + ".ARW");
            seqParts.add(a);
            if (!saveArw) seqTemp.add(a);
            addGhost(new File(dir, base + ".JPG"), 4, 1);
            return;
        }
        int shots = seqParts.size() + 1;
        if (kind == SHOT_LAST) endSequence();
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inSampleSize = 4;                         // 3012 wide down to the LCD's neighbourhood, in little memory
        Bitmap bm = null;
        try { bm = BitmapFactory.decodeFile(new File(dir, base + ".JPG").getPath(), o); } catch (Throwable t) { log("review decode: " + t); }
        log("review " + (bm == null ? "not shown (decode failed)" : "shown " + bm.getWidth() + "x" + bm.getHeight()));
        view.review = bm;
        view.reviewCaption = (kind == SHOT_LAST ? shots + " exposures, " + Ghost.MODE_NAMES[seqMode] + ": " : "")
                + Card.OUT_DIR + "/" + base + (saveArw ? ".JPG + .ARW" : ".JPG") + "   Centre / MENU / Shutter: back";
        status("");
        refresh();
    }

    private void closeReview() {
        if (view.review != null) { view.review.recycle(); view.review = null; }
        refresh();
    }

    // ------------------------------------------------------------ keys

    private static int turnDir(int sc) {
        switch (sc) {
            case K_WHEEL_CW: case K_DIAL_CW: case K_DIAL2_CW: case K_DIAL3_CW: return +1;
            case K_WHEEL_CCW: case K_DIAL_CCW: case K_DIAL2_CCW: case K_DIAL3_CCW: return -1;
        }
        return 0;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN) return onKeyDown(e.getKeyCode(), e);
        if (e.getAction() == KeyEvent.ACTION_UP) return onKeyUp(e.getKeyCode(), e);
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent e) {
        int sc = e.getScanCode();
        if (view.review != null) {
            // Only deliberate keys close the picture; anything else the camera sends is logged and ignored.
            if (sc == K_ENTER || sc == K_MENU || sc == K_SK1 || sc == K_S1) {
                log("review closed by key " + sc);
                closeReview();
                swallowUp = true;
            } else log("review: key " + sc + " ignored");
            return true;
        }
        if (busy) return true;                      // one shot at a time; the bar is the only thing on the keys
        int dir = turnDir(sc);
        if (dir != 0) { turn(dir); return true; }
        switch (sc) {
            case K_UP: view.row = (view.row + PatchView.ROWS - 1) % PatchView.ROWS; view.col = Math.min(view.col, fieldCount(view.row) - 1); refresh(); return true;
            case K_DOWN: view.row = (view.row + 1) % PatchView.ROWS; view.col = Math.min(view.col, fieldCount(view.row) - 1); refresh(); return true;
            case K_LEFT: view.col = (view.col + fieldCount(view.row) - 1) % fieldCount(view.row); refresh(); return true;
            case K_RIGHT: view.col = (view.col + 1) % fieldCount(view.row); refresh(); return true;
            case K_ENTER: if (e.getRepeatCount() == 0) centre(); return true;
            case K_DELETE: case K_SK2: if (e.getRepeatCount() == 0) trash(); return true;
            case K_FN: if (e.getRepeatCount() == 0) { view.panel = !view.panel; view.invalidate(); } return true;
            case K_S1: try { camera.autoFocus(null); } catch (Throwable t) {} return true;
            case K_S2: if (e.getRepeatCount() == 0) shoot(); return true;
            case K_MENU: case K_SK1: return true;
            case K_PLAY: return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) return true;
        return super.onKeyDown(keyCode, e);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent e) {
        int sc = e.getScanCode();
        if (swallowUp) { swallowUp = false; return true; }    // the release of the key that closed the picture
        switch (sc) {
            case K_S1: try { camera.cancelAutoFocus(); } catch (Throwable t) {} return true;
            case K_S2: cancelCapture(); return true;
            case K_MENU: case K_SK1:
                if (busy) return true;              // the bend finishes; leaving mid-write would leave half a file
                finish();
                return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BACK && !busy) { finish(); return true; }
        return true;
    }

    // ------------------------------------------------------------ prefs

    private static String toHex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(Character.forDigit((x >> 4) & 15, 16)).append(Character.forDigit(x & 15, 16));
        return s.toString();
    }

    private static byte[] fromHex(String s) {
        if (s == null || s.length() % 2 != 0) return null;
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            int hi = Character.digit(s.charAt(2 * i), 16), lo = Character.digit(s.charAt(2 * i + 1), 16);
            if (hi < 0 || lo < 0) return null;
            b[i] = (byte) ((hi << 4) | lo);
        }
        return b;
    }
}
