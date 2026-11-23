// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo2.game;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The first screen: starts the game and moves files in and out of the app's folder (the ROM, Game Boy ROMs and saves, a backup of the saves, the log for a bug
 * report). The game itself is Game (SDL). No layout files: the screen is built here, so the package has no resources besides the icon.
 */
public class ImportRom extends Activity {
    private static final int REQ_ROM = 1, REQ_BACKUP_SAVE = 3, REQ_BACKUP_LOAD = 4, REQ_LOG_SAVE = 5;

    private TextView status;
    private Button play;
    private int selectedPort;
    private Button controls;
    private SharedPreferences prefs;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences(Game.PREFS, MODE_PRIVATE);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(16, 18, 28));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, pad, pad, pad);
        scroll.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText("Kyujo2");
        title.setTextColor(Color.WHITE);
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(200, 205, 225));
        status.setTextSize(14);
        status.setPadding(0, dp(10), 0, dp(14));
        col.addView(status);

        play = button(col, "Play", new View.OnClickListener() {
            @Override public void onClick(View v) { startGame(); }
        });
        button(col, "Choose the Pokémon Stadium 2 (USA) ROM ...", new View.OnClickListener() {
            @Override public void onClick(View v) { pick(REQ_ROM, false, "*/*"); }
        });
        button(col, "Add a Game Boy game and its save (for the Game Boy Tower) ...", new View.OnClickListener() {
            @Override public void onClick(View v) { new android.app.AlertDialog.Builder(ImportRom.this)
                .setTitle("Choose a Transfer Pak")
                .setItems(new String[]{"Controller 1","Controller 2","Controller 3","Controller 4"},(dlg,index)-> {
                    selectedPort=index+1;
                    gbFlow.startRom();
                }).show(); }
        });
        controls = button(col, "", new View.OnClickListener() {
            @Override public void onClick(View v) { cycleControls(); }
        });
        button(col, "Save a backup of the saves and settings ...", new View.OnClickListener() {
            @Override public void onClick(View v) { create(REQ_BACKUP_SAVE, "Kyujo2-backup.zip", "application/zip"); }
        });
        button(col, "Restore a backup ...", new View.OnClickListener() {
            @Override public void onClick(View v) { pick(REQ_BACKUP_LOAD, false, "*/*"); }
        });
        button(col, "Save the log (for a bug report) ...", new View.OnClickListener() {
            @Override public void onClick(View v) { create(REQ_LOG_SAVE, "Kyujo2-log.txt", "text/plain"); }
        });

        TextView note = new TextView(this);
        note.setTextColor(Color.rgb(150, 156, 180));
        note.setTextSize(12);
        note.setPadding(0, dp(16), 0, 0);
        note.setText("The game, your saves and the ROM are kept in the app's private storage. Uninstalling the app or clearing its data deletes them: use \"Save a backup\" first. "
                + "The ROM is never uploaded or backed up; it is the file you chose, copied into the app. Game Boy files go to the folder GB. The same can be done from the game's own menu (Game Boy cartridges): choose the game and its save in two steps. "
                + "On-screen controls: Auto shows them while no gamepad is connected.");
        col.addView(note);

        setContentView(scroll);
        if (getIntent().getBooleanExtra("restartGame",false) && Storage.rom(this) != null) {
            getIntent().removeExtra("restartGame");
            play.postDelayed(this::startGame, 500);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private Button button(LinearLayout col, String text, View.OnClickListener l) {
        Button bt = new Button(this);
        bt.setText(text);
        bt.setAllCaps(false);
        bt.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        col.addView(bt, lp);
        return bt;
    }

    private void refresh() {
        if (busy) return;                                       // the message of the copy in progress stays
        File rom = Storage.rom(this);
        int saves = Storage.backupFiles(this).size();
        StringBuilder s = new StringBuilder();
        s.append(rom != null ? "ROM: " + rom.getName() + " (" + (rom.length() >> 20) + " MiB)" : "ROM: none yet. Choose your Pokémon Stadium 2 (USA) ROM file (.z64, .n64 or .v64).");
        s.append("\nSaves and settings files: ").append(saves);
        status.setText(s.toString());
        play.setEnabled(rom != null);
        controls.setText("On-screen controls: " + controlsName(prefs.getString(Game.PREF_CONTROLS, Game.CONTROLS_AUTO)) + "  (tap to change)");
    }

    private static String controlsName(String v) {
        if (Game.CONTROLS_STICK.equals(v)) return "Auto, analog stick on the left";
        if (Game.CONTROLS_OFF.equals(v)) return "Off";
        return "Auto, D-pad on the left";
    }

    private void cycleControls() {
        String v = prefs.getString(Game.PREF_CONTROLS, Game.CONTROLS_AUTO);
        String next = Game.CONTROLS_AUTO.equals(v) ? Game.CONTROLS_STICK : Game.CONTROLS_STICK.equals(v) ? Game.CONTROLS_OFF : Game.CONTROLS_AUTO;
        prefs.edit().putString(Game.PREF_CONTROLS, next).apply();
        refresh();
    }

    private void startGame() {
        if (Storage.rom(this) == null) {
            toast("Choose the ROM first.");
            return;
        }
        startActivity(new Intent(this, Game.class));
    }

    private void pick(int code, boolean multiple, String type) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(type);
        if (multiple) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(i, code);
        } catch (RuntimeException e) {
            toast("No file picker is available on this device.");
        }
    }

    private void create(int code, String name, String type) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(type);
        i.putExtra(Intent.EXTRA_TITLE, name);
        try {
            startActivityForResult(i, code);
        } catch (RuntimeException e) {
            toast("No file picker is available on this device.");
        }
    }

    /** A job that moves files; it returns the message to show. */
    private interface Job {
        String run() throws IOException;
    }

    private boolean busy;
    private final GbFlow gbFlow = new GbFlow(this, new GbFlow.Done() {      // the Game Boy game and its save, in two steps
        @Override public void done(final String path, String brief, final String full) {
            runOnUiThread(new Runnable() {
                @Override public void run() {
                    if (!path.isEmpty() && selectedPort>=1 && selectedPort<=4) {
                        android.util.AtomicFile file=new android.util.AtomicFile(new File(getFilesDir(),"incoming-pak-"+selectedPort));
                        java.io.FileOutputStream stream=null;
                        try {
                            stream=file.startWrite();
                            stream.write(path.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                            file.finishWrite(stream);
                            toast("Game Pak ready for controller "+selectedPort+".");
                        } catch(IOException failure) {
                            if(stream!=null)file.failWrite(stream);
                            toast("Could not remember the selected port.");
                        }
                    } else toast(full.isEmpty() ? "Nothing was added." : full);
                    refresh();
                }
            });
        }
    });

    /** Runs the job on a thread (a ROM is 32 MiB: copying it must not hold the screen), then shows its message. */
    private void background(final Job job) {
        if (busy) {
            toast("Please wait: the last file is still being copied.");
            return;
        }
        busy = true;
        status.setText("Working ...");
        play.setEnabled(false);
        new Thread(() -> {
            String msg;
            try {
                msg = job.run();
            } catch (IOException | RuntimeException e) {
                msg = "Failed: " + e.getMessage();
            }
            final String m = msg;
            runOnUiThread(() -> {
                busy = false;
                toast(m);
                refresh();
            });
        }, "file-copy").start();
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (gbFlow.onResult(code, result, data)) return;
        if (result != RESULT_OK || data == null) return;
        final Context c = getApplicationContext();
        final Uri u = data.getData();
        switch (code) {
            case REQ_ROM:
                if (u != null) background(() -> "ROM stored: " + Storage.importRom(c, u));
                break;
            case REQ_BACKUP_SAVE:
                if (u != null) {
                    background(() -> {
                        try (OutputStream os = c.getContentResolver().openOutputStream(u)) {
                            if (os == null) throw new IOException("the file could not be written");
                            return "Backup written: " + Storage.writeBackup(c, os) + " file(s).";
                        }
                    });
                }
                break;
            case REQ_BACKUP_LOAD:
                if (u != null) {
                    background(() -> {
                        try (InputStream in = c.getContentResolver().openInputStream(u)) {
                            if (in == null) throw new IOException("the file could not be opened");
                            return "Restored " + Storage.restoreBackup(c, in) + " file(s).";
                        }
                    });
                }
                break;
            case REQ_LOG_SAVE:
                if (u != null) {
                    background(() -> {
                        try (OutputStream os = c.getContentResolver().openOutputStream(u)) {
                            if (os == null) throw new IOException("the file could not be written");
                            return Storage.writeLogs(c, os) == 0 ? "There is no log yet: start the game once." : "Log written.";
                        }
                    });
                }
                break;
            default:
                break;
        }
    }

    private void toast(String s) {
        Toast t = Toast.makeText(this, s, Toast.LENGTH_LONG);
        t.setGravity(Gravity.CENTER, 0, 0);
        t.show();
    }
}
