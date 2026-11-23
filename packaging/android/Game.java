// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo2.game;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Process;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/**
 * The game's screen: SDL's activity (it makes the window, the audio device and the event loop, and runs the game's main() in a thread of libmain.so), plus the
 * on-screen controls on top of the game's surface, and the system's file picker for Game Boy files, which the game's own Game Pak menu opens (pickGb). The game takes
 * the app's private folder as its folder (see Storage and shell/sdl/main_sdl.c).
 */
public class Game extends SDLActivity {
    static final String PREFS = "kyujo";
    static final String PREF_CONTROLS = "controls";
    static final String CONTROLS_AUTO = "auto";      // on-screen controls while no gamepad is connected, a D-pad on the left
    static final String CONTROLS_STICK = "stick";    // the same with an analog stick on the left
    static final String CONTROLS_OFF = "off";

    private TouchOverlay overlay;
    private GbFlow gbFlow;                            // the Game Boy file choosing that is under way (UI thread)

    /** The game's answer: `path` is the game's path of the file now in use ("" if none), `brief` a short line for the game's menu, `detail` the whole sentence (for the log). */
    private static native void nativeGbPicked(int port, int kind, String path, String brief, String detail);

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "Kyujo2" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] { Storage.rom(this).getAbsolutePath(), Storage.dir(this).getAbsolutePath() };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        String mode = p.getString(PREF_CONTROLS, CONTROLS_AUTO);
        overlay = new TouchOverlay(this, CONTROLS_STICK.equals(mode), !CONTROLS_OFF.equals(mode));
        if (mLayout != null) {
            mLayout.addView(overlay, new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    // ---- the Game Boy file picker (called by the game, through JNI, on its own thread) ---------------------------------------------------

    /**
     * Starts the choosing of a Game Boy file for the game's Game Pak page (GbFlow: the player allows one folder, then taps the game; the save of the same name beside it comes with it). kind 0 is the Game Boy ROM item; the save is stored as
     * "<rom>.sav", the automatic name, as on every other system. kind 1 is the Save file item: a save file on its own, kept under its own name. The answer goes to the game with nativeGbPicked.
     */
    public void pickGb(final int port, final int kind, final String rom) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                gbFlow = new GbFlow(Game.this, new GbFlow.Done() {
                    @Override public void done(String path, String brief, String full) { answer(port, kind, path, brief, full); }
                });
                if (kind == 0) gbFlow.startRom();
                else gbFlow.startSaveOnly();
            }
        });
    }

    public void restartGame() {
        runOnUiThread(() -> {
            Intent restart = new Intent(this, ImportRom.class);
            restart.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
            restart.putExtra("restartGame", true);
            startActivity(restart);
            finish();
        });
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (gbFlow != null && gbFlow.onResult(code, result, data)) return;
        super.onActivityResult(code, result, data);
    }

    private void answer(int port, int kind, String path, String brief, final String full) {
        if (!full.isEmpty()) {
            runOnUiThread(new Runnable() {
                @Override public void run() { Toast.makeText(Game.this, full, Toast.LENGTH_LONG).show(); }
            });
        }
        try {
            nativeGbPicked(port, kind, path, brief, full);
        } catch (UnsatisfiedLinkError e) {
            // the game's library is not loaded: nobody is waiting
        }
    }

    @Override
    protected void onPause() {
        if (overlay != null) overlay.release();      // no button stays held while the game is in the background
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (overlay != null) overlay.shutdown();
        super.onDestroy();
        // The game's native state (static variables, the translated game's memory) is made for one run per process, as on every other platform: when the game screen is closed
        // the process of the game ends (it is its own process, ":game", so the launcher is not affected) and the next start is a clean one.
        if (isFinishing()) Process.killProcess(Process.myPid());
    }
}
