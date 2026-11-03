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
import java.util.List;

/**
 * Rewired: CHDK_Rewired's circuit bending for the Sony a6000.
 *
 * The a6000 cannot be patched the way a PowerShot can - its imaging pipeline is not ours to hook - so the bend happens
 * one step later, on the raw the camera has just written: the shutter fires, the ARW lands on the card, and its sensor
 * data goes through the same bend matrix and experimental chain CHDK_Rewired runs inside a PowerShot's capture path.
 * The camera's own file is never touched; the bent raw, a developed JPEG and the recipe go into REWIRED/ (see {@link Card}).
 *
 * Keys - on every body Recipe Lab lists them for:
 *   UP / DOWN       row: matrix, scope, chain, preset
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
                .putInt("frame", frame).commit();
    }

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

        view.fields[PatchView.ROW_PRESET] = new String[] { "PRESET", presetLabel(), "", "centre: " + presetAction() };

        view.hint = hint();
        if (!busy && !waiting) {
            view.status = "1".equals(d[Native.D_ACTIVE]) ? "" : "Straight through - turn the wheel on a bit, or press centre to reroll";
        }
        view.invalidate();
    }

    private String hint() {
        switch (view.row) {
            case PatchView.ROW_MATRIX: return "wheel: patch a source   centre: reroll   trash: straight wire   shutter: shoot";
            case PatchView.ROW_SCOPE:  return "wheel: change   centre: mutate the matrix a little   trash: reset this region";
            case PatchView.ROW_CHAIN:  return "wheel: change   centre: lock and start another   trash: drop an effect";
            default:                   return "wheel: choose   centre: " + presetAction();
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
            default: presetGo();
        }
    }

    private void trash() {
        switch (view.row) {
            case PatchView.ROW_MATRIX: edit(Native.OP_ROUTE_STRAIGHT, view.routes.length - 1 - view.col, 0); return;
            case PatchView.ROW_SCOPE: edit(Native.OP_RESET_MATRIX, 0, 0); return;
            case PatchView.ROW_CHAIN: edit(Native.OP_X_DROP, 0, 0); return;
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
                bend(f);
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

    private void bend(final File in) {
        final File dir = new File(card(), Card.OUT_DIR);
        dir.mkdirs();
        final String base = Card.freeBase(dir, Card.outBase(in.getName()));
        final byte[] rec = recipe;
        final int seed = seed(), fr = ++frame;
        final boolean arw = saveArw;
        busy = true;
        save();
        view.progress = 0;
        status("Bending " + in.getName());
        handler.post(progressTick);
        // An explicit 1 MB stack: Dalvik's default for a new thread is small, and this one runs a lot of native code.
        new Thread(null, new Runnable() {
            public void run() {
                String err;
                try {
                    int rc = Native.process(in.getPath(), arw ? new File(dir, base + ".ARW").getPath() : null,
                            new File(dir, base + ".JPG").getPath(), new File(dir, base + ".BND").getPath(),
                            ROM_PATH, rec, seed, fr);
                    err = rc == 0 ? null : Native.lastError();
                } catch (Throwable t) {
                    err = String.valueOf(t);
                    log("process threw: " + t);
                }
                final String e = err;
                handler.post(new Runnable() { public void run() { bent(dir, base, e); } });
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

    private void bent(File dir, String base, String err) {
        busy = false;
        view.progress = -1;
        if (err != null) { status("Bend failed: " + err); return; }
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inSampleSize = 4;                         // 3012 wide down to the LCD's neighbourhood, in little memory
        Bitmap bm = null;
        try { bm = BitmapFactory.decodeFile(new File(dir, base + ".JPG").getPath(), o); } catch (Throwable t) { log("review decode: " + t); }
        log("review " + (bm == null ? "not shown (decode failed)" : "shown " + bm.getWidth() + "x" + bm.getHeight()));
        view.review = bm;
        view.reviewCaption = Card.OUT_DIR + "/" + base + (saveArw ? ".JPG + .ARW" : ".JPG") + "   centre / MENU / shutter: back";
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
