package com.rewired.a6000;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.View;

/**
 * Everything drawn over the live view: the patch strip, the rows under it, the status line, the progress bar while a
 * shot is being bent, and the bent picture when it is done.
 *
 * Drawn on a Canvas, as Recipe Lab draws its screens: the camera's Android has no touch and only a handful of keys,
 * and a few rectangles and lines of text are easier to make legible on its LCD than a widget layout.
 */
public class PatchView extends View {
    static final int ROW_MATRIX = 0, ROW_SCOPE = 1, ROW_CHAIN = 2, ROW_MULTI = 3, ROW_PRESET = 4, ROWS = 5;
    static final String[] ROW_NAMES = { "MATRIX", "SCOPE", "CHAIN", "MULTI", "PRESET" };

    /**
     * The panel a picture is drawn for. The view is always 640x480-shaped, but the a6000's LCD is a 16:9 panel and its
     * finder a 4:3 one, so the same 3:2 picture covers a different part of the view on each - the live view does
     * likewise, and the ghost has to land where the live view is to be any use for lining a shot up.
     */
    static final int SCREEN_LCD = 0, SCREEN_EVF = 1;
    static final String[] SCREEN_NAMES = { "LCD", "EVF" };
    private static final float[] SCREEN_ASPECT = { 16f / 9f, 4f / 3f };

    // the state MainActivity hands over before each invalidate()
    String[] routes = new String[0];
    String[] desc = new String[11];
    String[][] fields = new String[ROWS][];     // label/value pairs per row, except the matrix
    int row, col;
    boolean panel = true;
    String status = "";
    String hint = "";
    int progress = -1;                          // 0..1000 while working, -1 otherwise
    Bitmap review;
    String reviewCaption = "";
    Bitmap ghost;                               // the multiple exposure so far, over the live view
    int ghostAlpha;                             // 0..255; 0 hides it
    int screen = SCREEN_LCD;

    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gp = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final RectF rf = new RectF();
    private final Rect src = new Rect(), dst = new Rect();

    private static final int BG = 0xd0101014, CELL = 0xff26262e, CELL_SEL = 0xffe0306a, CELL_ON = 0xff2f6f5f,
            TEXT = 0xffeeeeee, DIM = 0xff8a8a96, ACCENT = 0xffff5c8a, WARN = 0xffffc04a;

    public PatchView(Context c) { super(c); }
    public PatchView(Context c, AttributeSet a) { super(c, a); }

    private float u() { return getHeight() / 480f; }        // one unit at the a6000's 640x480 framebuffer

    @Override
    protected void onDraw(Canvas c) {
        float u = u();
        int w = getWidth(), h = getHeight();
        p.setTypeface(Typeface.DEFAULT_BOLD);

        if (review != null) { drawReview(c, w, h, u); return; }

        // the ghost under everything else, so the panel stays readable over it
        if (ghost != null && ghostAlpha > 0) {
            src.set(0, 0, ghost.getWidth(), ghost.getHeight());
            fit(ghost.getWidth(), ghost.getHeight(), w, h, dst);
            gp.setAlpha(ghostAlpha);
            c.drawBitmap(ghost, src, dst, gp);
        }

        // status line at the top, always
        if (status.length() > 0 || progress >= 0) {
            p.setColor(BG);
            c.drawRect(0, 0, w, 30 * u, p);
            p.setColor(progress >= 0 ? WARN : TEXT);
            p.setTextSize(16 * u);
            c.drawText(status, 10 * u, 21 * u, p);
            if (progress >= 0) {
                p.setColor(0xff333340);
                c.drawRect(0, 27 * u, w, 30 * u, p);
                p.setColor(ACCENT);
                c.drawRect(0, 27 * u, w * Math.min(progress, 1000) / 1000f, 30 * u, p);
            }
        }
        if (!panel) return;

        float top = h - 220 * u;
        p.setColor(BG);
        c.drawRect(0, top, w, h, p);

        // the patch strip: one cell per output bit, the top bit on the left
        float y = top + 8 * u, cellW = (w - 20 * u) / Math.max(routes.length, 1), cellH = 46 * u;
        p.setTextAlign(Paint.Align.CENTER);
        for (int i = 0; i < routes.length; i++) {
            int bit = routes.length - 1 - i;
            boolean sel = row == ROW_MATRIX && col == i;
            boolean straight = routes[i].equals("d" + bitName(bit));
            rf.set(10 * u + i * cellW + 2 * u, y, 10 * u + (i + 1) * cellW - 2 * u, y + cellH);
            p.setColor(sel ? CELL_SEL : straight ? CELL : CELL_ON);
            c.drawRoundRect(rf, 5 * u, 5 * u, p);
            p.setColor(DIM);
            p.setTextSize(10 * u);
            c.drawText(String.valueOf(bit), rf.centerX(), rf.top + 13 * u, p);
            p.setColor(TEXT);
            p.setTextSize(16 * u);
            c.drawText(routes[i], rf.centerX(), rf.top + 36 * u, p);
        }
        p.setTextAlign(Paint.Align.LEFT);

        // the rows under it
        y += cellH + 10 * u;
        for (int r = ROW_SCOPE; r < ROWS; r++) {
            drawRow(c, r, y, w, u);
            y += 30 * u;
        }
        // the key legend for the row in focus
        p.setColor(DIM);
        p.setTextSize(12 * u);
        c.drawText(hint, 10 * u, h - 10 * u, p);
    }

    private static String bitName(int bit) { return bit < 10 ? String.valueOf(bit) : String.valueOf((char) ('a' + bit - 10)); }

    private void drawRow(Canvas c, int r, float y, int w, float u) {
        String[] f = fields[r];
        boolean focus = row == r;
        p.setTextSize(11 * u);
        p.setColor(focus ? ACCENT : DIM);
        c.drawText(ROW_NAMES[r], 10 * u, y + 18 * u, p);
        if (f == null) return;
        int nf = f.length / 2;
        float[] cws = new float[nf];
        for (int i = 0; i < nf; i++) {
            p.setTextSize(10 * u);
            float lw = p.measureText(f[2 * i]);
            p.setTextSize(14 * u);
            cws[i] = Math.max(lw, p.measureText(f[2 * i + 1])) + 14 * u;
        }
        // Scrolled so the selected field is always on screen: drop fields off the left until it fits.
        int first = 0;
        if (focus) {
            float span;
            do {
                span = 0;
                for (int i = first; i <= Math.min(col, nf - 1); i++) span += cws[i] + 5 * u;
            } while (78 * u + span > w - 6 * u && ++first < col);
        }
        float x = 78 * u;
        for (int i = 2 * first; i + 1 < f.length; i += 2) {
            String label = f[i], value = f[i + 1];
            float cw = cws[i / 2];
            if (x + cw > w - 6 * u) break;
            boolean sel = focus && col == i / 2;
            rf.set(x, y, x + cw, y + 27 * u);
            p.setColor(sel ? CELL_SEL : CELL);
            c.drawRoundRect(rf, 4 * u, 4 * u, p);
            p.setColor(DIM);
            p.setTextSize(9 * u);
            c.drawText(label, x + 7 * u, y + 10 * u, p);
            p.setColor(TEXT);
            p.setTextSize(14 * u);
            c.drawText(value, x + 7 * u, y + 24 * u, p);
            x += cw + 5 * u;
        }
    }

    /**
     * Where an iw x ih picture goes in a w x h view so it shows at its own shape on the selected panel: as large as
     * fits, centred. On the 16:9 LCD each of the view's pixels is wider than tall, so the picture is drawn narrower.
     */
    private void fit(int iw, int ih, int w, int h, Rect out) {
        float pixAspect = SCREEN_ASPECT[screen] / (w / (float) h);    // shown width of one view pixel / its height
        float imgAspect = iw / (float) ih;
        float dw = h * imgAspect / pixAspect, dh = h;
        if (dw > w) { dw = w; dh = w * pixAspect / imgAspect; }
        out.set((int) ((w - dw) / 2), (int) ((h - dh) / 2), (int) ((w + dw) / 2), (int) ((h + dh) / 2));
    }

    private void drawReview(Canvas c, int w, int h, float u) {
        c.drawColor(Color.BLACK);
        src.set(0, 0, review.getWidth(), review.getHeight());
        fit(review.getWidth(), review.getHeight(), w, h, dst);
        c.drawBitmap(review, src, dst, null);
        p.setColor(BG);
        c.drawRect(0, h - 26 * u, w, h, p);
        p.setColor(TEXT);
        p.setTextSize(13 * u);
        c.drawText(reviewCaption, 10 * u, h - 8 * u, p);
    }
}
