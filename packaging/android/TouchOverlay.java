// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo2.game;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

/**
 * The on-screen controls: an N64 pad drawn over the game's surface. It reports the held buttons and the stick to the game (nativeTouch) as one more controller, "Touch
 * controls", so the game's own button assignment applies. It is shown while no gamepad is connected (nativeActive tells the game the same, which then ignores it as soon as
 * a pad is there). Layout in a unit u (a tenth of the screen's height): D-pad or stick bottom left, A and B bottom right, the four C buttons above them, L and Z top left,
 * R top right, Start in the middle, a small Menu button (the game's menu) next to it.
 *
 * Touch handling: every finger is followed by its id. A finger that went down on the stick keeps it until it lifts; any other finger is hit-tested against the buttons at each event,
 * so sliding from one button to the next works and several buttons can be held together (A+B, a C button while moving).
 */
final class TouchOverlay extends View implements InputManager.InputDeviceListener {
    // the bits shared with shell/sdl/main_sdl.c (TB_*)
    static final int A = 1, B = 1 << 1, START = 1 << 2, L = 1 << 3, R = 1 << 4, Z = 1 << 5, MENU = 1 << 6, CUP = 1 << 7, CDOWN = 1 << 8, CLEFT = 1 << 9, CRIGHT = 1 << 10,
            DUP = 1 << 11, DDOWN = 1 << 12, DLEFT = 1 << 13, DRIGHT = 1 << 14;

    private static native void nativeTouch(int mask, int lx, int ly);   // lx right positive, ly UP positive (the game's raw pad convention), -32767..32767
    private static native void nativeActive(boolean on);

    /** One round or rounded button. */
    private static final class Btn {
        final int bit;
        final String label;
        final int color;
        float cx, cy, r;      // round buttons (r > 0)
        RectF rect;           // rectangular buttons
        boolean down;

        Btn(int bit, String label, int color) {
            this.bit = bit;
            this.label = label;
            this.color = color;
        }

        boolean hit(float x, float y, float slop) {
            if (rect != null) return x >= rect.left - slop && x <= rect.right + slop && y >= rect.top - slop && y <= rect.bottom + slop;
            float dx = x - cx, dy = y - cy;
            float rr = r + slop;
            return dx * dx + dy * dy <= rr * rr;
        }

        float dist2(float x, float y) {
            float px = rect != null ? rect.centerX() : cx, py = rect != null ? rect.centerY() : cy;
            return (x - px) * (x - px) + (y - py) * (y - py);
        }
    }

    private final boolean stickMode;
    private final boolean enabledByPref;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Btn[] buttons;
    private final Btn bA = new Btn(A, "A", 0xFF3B7BFF), bB = new Btn(B, "B", 0xFF2FBF5B), bStart = new Btn(START, "START", 0xFFE0443E), bL = new Btn(L, "L", 0xFFB0B4C0),
            bR = new Btn(R, "R", 0xFFB0B4C0), bZ = new Btn(Z, "Z", 0xFFB0B4C0), bMenu = new Btn(MENU, "MENU", 0xFF8A90A6), bCup = new Btn(CUP, "▲", 0xFFFFD23F),
            bCdown = new Btn(CDOWN, "▼", 0xFFFFD23F), bCleft = new Btn(CLEFT, "◀", 0xFFFFD23F), bCright = new Btn(CRIGHT, "▶", 0xFFFFD23F);

    // the stick / D-pad area
    private float padX, padY, padR;
    private int stickPointer = -1;
    private float stickDx, stickDy;       // -1..1
    private int dpadMask;
    private boolean active;               // reported to the game
    private boolean gamepadPresent;
    private int lastMask = -1, lastLx, lastLy;
    private final InputManager inputManager;

    TouchOverlay(Context c, boolean stick, boolean enabled) {
        super(c);
        stickMode = stick;
        enabledByPref = enabled;
        buttons = new Btn[] { bA, bB, bStart, bL, bR, bZ, bMenu, bCup, bCdown, bCleft, bCright };
        setFocusable(false);
        setFocusableInTouchMode(false);
        setWillNotDraw(false);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(0xFFFFFFFF);
        line.setStyle(Paint.Style.STROKE);
        inputManager = (InputManager) c.getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(this, null);
        gamepadPresent = scanGamepads();
        updateActive();
    }

    // ---- gamepads ---------------------------------------------------------------------------------------------------------------------

    private static boolean scanGamepads() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    @Override public void onInputDeviceAdded(int id) { gamepadPresent = scanGamepads(); updateActive(); }
    @Override public void onInputDeviceRemoved(int id) { gamepadPresent = scanGamepads(); updateActive(); }
    @Override public void onInputDeviceChanged(int id) { gamepadPresent = scanGamepads(); updateActive(); }

    private void updateActive() {
        boolean on = enabledByPref && !gamepadPresent;
        if (on == active && getVisibility() == (on ? VISIBLE : GONE)) return;
        active = on;
        post(new Runnable() {
            @Override public void run() {
                setVisibility(active ? VISIBLE : GONE);
                if (!active) clear();
            }
        });
        try {
            nativeActive(active);
        } catch (UnsatisfiedLinkError e) {
            // the library is not loaded (the game did not start): nothing to tell
        }
    }

    /** Called when the activity pauses: nothing stays held. */
    void release() {
        post(new Runnable() { @Override public void run() { clear(); } });
    }

    void shutdown() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        try {
            nativeActive(false);
        } catch (UnsatisfiedLinkError e) {
            // see updateActive
        }
    }

    private void clear() {
        for (Btn b : buttons) b.down = false;
        stickPointer = -1;
        stickDx = stickDy = 0;
        dpadMask = 0;
        report();
        invalidate();
    }

    // ---- layout -----------------------------------------------------------------------------------------------------------------------

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float u = h / 10f;
        float m = u * 0.5f;                                   // margin
        padR = u * 2.0f;
        padX = m + padR + u * 0.4f;
        padY = h - m - padR - u * 0.2f;

        float ar = u * 1.15f;                                 // A and B
        bA.cx = w - m - ar - u * 0.2f;
        bA.cy = h - m - ar - u * 0.2f;
        bA.r = ar;
        bB.cx = bA.cx - ar * 2.1f;
        bB.cy = bA.cy - ar * 0.55f;
        bB.r = ar * 0.95f;

        float cr = u * 0.72f;                                 // the C diamond above A and B
        float ccx = (bA.cx + bB.cx) / 2f + ar * 0.55f, ccy = bA.cy - ar * 2.75f, off = cr * 1.5f;
        bCup.cx = ccx; bCup.cy = ccy - off;
        bCdown.cx = ccx; bCdown.cy = ccy + off;
        bCleft.cx = ccx - off; bCleft.cy = ccy;
        bCright.cx = ccx + off; bCright.cy = ccy;
        for (Btn c : new Btn[] { bCup, bCdown, bCleft, bCright }) c.r = cr;

        bL.rect = new RectF(m, m, m + u * 2.6f, m + u * 1.2f);
        bZ.rect = new RectF(m, m + u * 1.5f, m + u * 2.6f, m + u * 2.7f);
        bR.rect = new RectF(w - m - u * 2.6f, m, w - m, m + u * 1.2f);
        bStart.rect = new RectF(w / 2f - u * 1.2f, h - m - u * 1.1f, w / 2f + u * 1.2f, h - m);
        bMenu.rect = new RectF(w / 2f - u * 1.0f, m, w / 2f + u * 1.0f, m + u * 0.95f);
    }

    // ---- touch ------------------------------------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!active) return false;
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            clear();
            return true;
        }
        int down = action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ? e.getActionIndex() : -1;
        int up = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? e.getActionIndex() : -1;
        if (action == MotionEvent.ACTION_UP) up = 0;

        // a new finger in the stick's area owns the stick
        if (down >= 0 && stickPointer < 0) {
            float x = e.getX(down), y = e.getY(down);
            if (inPadArea(x, y)) stickPointer = e.getPointerId(down);
        }
        if (up >= 0 && e.getPointerId(up) == stickPointer) {
            stickPointer = -1;
        }

        boolean[] pressed = new boolean[buttons.length];
        float sx = 0, sy = 0;
        boolean haveStick = false;
        for (int i = 0; i < e.getPointerCount(); i++) {
            if (i == up) continue;
            float x = e.getX(i), y = e.getY(i);
            if (e.getPointerId(i) == stickPointer) {
                sx = x; sy = y;
                haveStick = true;
                continue;
            }
            // the nearest button this finger touches (with some slop: fingers are bigger than the drawings)
            int best = -1;
            float bestD = Float.MAX_VALUE;
            float slop = getHeight() / 10f * 0.25f;
            for (int k = 0; k < buttons.length; k++) {
                if (!buttons[k].hit(x, y, slop)) continue;
                float d = buttons[k].dist2(x, y);
                if (d < bestD) { bestD = d; best = k; }
            }
            if (best >= 0) pressed[best] = true;
        }
        for (int k = 0; k < buttons.length; k++) {
            if (pressed[k] && !buttons[k].down) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            buttons[k].down = pressed[k];
        }
        if (haveStick) setStick(sx, sy);
        else { stickDx = stickDy = 0; dpadMask = 0; }
        report();
        invalidate();
        return true;
    }

    private boolean inPadArea(float x, float y) {
        float dx = x - padX, dy = y - padY, r = padR * 1.45f;
        return dx * dx + dy * dy <= r * r;
    }

    /** The stick or D-pad from the finger's position. */
    private void setStick(float x, float y) {
        float dx = (x - padX) / padR, dy = (y - padY) / padR;
        float len = (float) Math.hypot(dx, dy);
        if (len > 1f) { dx /= len; dy /= len; len = 1f; }
        stickDx = dx;
        stickDy = dy;
        dpadMask = 0;
        if (!stickMode && len > 0.28f) {
            // 8 directions: a direction is on when the finger is within 67.5 degrees of it (a bit of overlap gives the diagonals)
            double ang = Math.toDegrees(Math.atan2(-dy, dx));      // 0 = right, 90 = up
            if (Math.abs(angDiff(ang, 0)) < 67.5) dpadMask |= DRIGHT;
            if (Math.abs(angDiff(ang, 90)) < 67.5) dpadMask |= DUP;
            if (Math.abs(angDiff(ang, 180)) < 67.5) dpadMask |= DLEFT;
            if (Math.abs(angDiff(ang, -90)) < 67.5) dpadMask |= DDOWN;
        }
    }

    private static double angDiff(double a, double b) {
        double d = a - b;
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return d;
    }

    private void report() {
        int mask = dpadMask;
        for (Btn b : buttons) if (b.down) mask |= b.bit;
        int lx = 0, ly = 0;
        if (stickMode) {
            float dz = 0.12f;                                  // the game has its own dead zone: this one is only to keep a resting thumb from drifting
            float len = (float) Math.hypot(stickDx, stickDy);
            if (len > dz) {
                float s = (len - dz) / (1f - dz) / len;
                lx = Math.round(stickDx * s * 32767f);
                ly = Math.round(-stickDy * s * 32767f);        // screen y grows downward; the game's raw pad has up positive
            }
        }
        if (mask == lastMask && lx == lastLx && ly == lastLy) return;
        lastMask = mask; lastLx = lx; lastLy = ly;
        try {
            nativeTouch(mask, lx, ly);
        } catch (UnsatisfiedLinkError e) {
            // the library is not loaded: nothing to tell
        }
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas c) {
        if (!active) return;
        float u = getHeight() / 10f;
        // stick base or D-pad
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x33FFFFFF);
        c.drawCircle(padX, padY, padR, fill);
        line.setStrokeWidth(u * 0.08f);
        line.setColor(0x66FFFFFF);
        c.drawCircle(padX, padY, padR, line);
        if (stickMode) {
            fill.setColor(0x88C8CCDA);
            c.drawCircle(padX + stickDx * padR * 0.65f, padY + stickDy * padR * 0.65f, padR * 0.42f, fill);
        } else {
            drawPadArrow(c, DUP, 0, -1);
            drawPadArrow(c, DDOWN, 0, 1);
            drawPadArrow(c, DLEFT, -1, 0);
            drawPadArrow(c, DRIGHT, 1, 0);
        }
        for (Btn b : buttons) drawButton(c, b, u);
    }

    private void drawPadArrow(Canvas c, int bit, int sx, int sy) {
        boolean on = (dpadMask & bit) != 0;
        float r = padR * 0.3f, x = padX + sx * padR * 0.62f, y = padY + sy * padR * 0.62f;
        fill.setColor(on ? 0xCCFFFFFF : 0x55FFFFFF);
        c.drawCircle(x, y, r, fill);
    }

    private void drawButton(Canvas c, Btn b, float u) {
        int alpha = b.down ? 0xE0 : 0x66;
        fill.setStyle(Paint.Style.FILL);
        fill.setColor((alpha << 24) | (b.color & 0xFFFFFF));
        float tx, ty;
        if (b.rect != null) {
            c.drawRoundRect(b.rect, u * 0.3f, u * 0.3f, fill);
            tx = b.rect.centerX();
            ty = b.rect.centerY();
            text.setTextSize(b.bit == MENU || b.bit == START ? u * 0.5f : u * 0.7f);
        } else {
            c.drawCircle(b.cx, b.cy, b.r, fill);
            tx = b.cx;
            ty = b.cy;
            text.setTextSize(b.r * (b.label.length() == 1 && b.label.charAt(0) < 128 ? 1.1f : 0.8f));
        }
        text.setColor(b.down ? 0xFF000000 : 0xCCFFFFFF);
        c.drawText(b.label, tx, ty - (text.descent() + text.ascent()) / 2f, text);
    }
}
