// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo2.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/**
 * Choosing a Game Boy game the way every other system does it: the game's file and its save sit in one folder, the save has the game's name with another ending
 * (Game.gb, Game.sav), and the player does not have to name the save.
 *
 * Android does not let an app look into the phone's folders, so the player allows ONE folder once ("Choose your Game Boy folder", remembered afterwards). The game's menu then
 * lists the games of that folder; the player taps one; the game and the save of the same name from the same folder are copied into the app (folder GB, save stored as
 * "<game>.sav", the automatic save name). A game without a save starts with a new one. "Pick one file instead" is the way for a game that is not in a folder that can be allowed.
 *
 * startSaveOnly() is the same for a save alone (the "Save file" item of the Game Pak page): the saves of the folder are listed; the one chosen is kept under its own name.
 * The host activity passes its onActivityResult on to onResult(); the answer comes to `Done` (on any thread).
 */
final class GbFlow {
    interface Done {
        /** `path`: the game's path of the file now in use ("" if nothing was chosen or it failed); `brief`: a short line for the game's menu; `full`: the whole sentence for the player. */
        void done(String path, String brief, String full);
    }

    static final int REQ_FOLDER = 0x5046, REQ_ROM = 0x5047, REQ_SAVE = 0x5048;
    private static final String PREF_FOLDER = "gb_folder";
    private static final int MAX_ENTRIES = 600;

    /** A file of the allowed folder. */
    private static final class Entry {
        String docId, name, parentId, parentName;
    }

    private final Activity act;
    private final Done done;
    private boolean saveOnly;

    GbFlow(Activity act, Done done) {
        this.act = act;
        this.done = done;
    }

    /** The game item. Call on the UI thread. */
    void startRom() {
        saveOnly = false;
        begin();
    }

    /** The save item: a save file on its own. Call on the UI thread. */
    void startSaveOnly() {
        saveOnly = true;
        begin();
    }

    private void nothing() {
        done.done("", "Nothing was chosen.", "");
    }

    // ---- the folder -------------------------------------------------------------------------------------------------------------------

    private SharedPreferences prefs() {
        return act.getSharedPreferences(Game.PREFS, Context.MODE_PRIVATE);
    }

    /** The folder the player allowed earlier, if the permission is still there. */
    private Uri savedFolder() {
        String s = prefs().getString(PREF_FOLDER, null);
        if (s == null) return null;
        Uri u = Uri.parse(s);
        for (UriPermission p : act.getContentResolver().getPersistedUriPermissions()) {
            if (p.getUri().equals(u) && p.isReadPermission()) return u;
        }
        return null;
    }

    private void begin() {
        Uri folder = savedFolder();
        if (folder != null) list(folder);
        else askFolder();
    }

    private void askFolder() {
        String what = saveOnly ? "its save files" : "their save files";
        AlertDialog.Builder b = new AlertDialog.Builder(act).setTitle("Choose your Game Boy folder");
        b.setMessage("Choose the folder on your phone that holds your Game Boy games and " + what + ". The game and its save are found by their names (Game.gb and Game.sav), "
                + "so you do not have to pick the save. You choose the folder only once.\n\nIf your files are in the Download folder, put them in a folder of their own first: newer Android versions do not allow choosing Download itself.");
        b.setPositiveButton("Choose folder", new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) { pickFolder(); }
        });
        b.setNeutralButton("Pick one file instead", new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) { pickFile(); }
        });
        b.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) { nothing(); }
        });
        b.setOnCancelListener(new DialogInterface.OnCancelListener() {
            @Override public void onCancel(DialogInterface d) { nothing(); }
        });
        show(b);
    }

    private void show(AlertDialog.Builder b) {
        try {
            b.show();
        } catch (RuntimeException e) {
            done.done("", "Could not show the question.", "Could not show the question: " + e.getMessage());
        }
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            act.startActivityForResult(i, REQ_FOLDER);
        } catch (RuntimeException e) {
            pickFile();                                         // no folder picker on this device
        }
    }

    private void pickFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            act.startActivityForResult(i, saveOnly ? REQ_SAVE : REQ_ROM);
        } catch (RuntimeException e) {
            done.done("", "No file picker on this device.", "No file picker is available on this device.");
        }
    }

    // ---- listing the folder ---------------------------------------------------------------------------------------------------------

    private static void collect(Context c, Uri tree, String dirId, String dirName, int depth, List<Entry> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        List<String[]> subdirs = new ArrayList<>();
        try (Cursor q = c.getContentResolver().query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            if (q == null) return;
            while (q.moveToNext() && out.size() < MAX_ENTRIES) {
                String id = q.getString(0), name = q.getString(1), mime = q.getString(2);
                if (id == null || name == null) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    subdirs.add(new String[] {id, name});
                } else {
                    Entry e = new Entry();
                    e.docId = id;
                    e.name = name;
                    e.parentId = dirId;
                    e.parentName = dirName;
                    out.add(e);
                }
            }
        }
        if (depth > 0) for (String[] d : subdirs) collect(c, tree, d[0], d[1], depth - 1, out);
    }

    private void list(final Uri tree) {
        final Context c = act.getApplicationContext();
        new Thread(new Runnable() {
            @Override public void run() {
                final List<Entry> all = new ArrayList<>();
                try {
                    collect(c, tree, DocumentsContract.getTreeDocumentId(tree), "", 1, all);
                } catch (RuntimeException e) {                       // the folder is gone or the permission was taken back
                    prefs().edit().remove(PREF_FOLDER).apply();
                    act.runOnUiThread(new Runnable() {
                        @Override public void run() { askFolder(); }
                    });
                    return;
                }
                act.runOnUiThread(new Runnable() {
                    @Override public void run() { choose(tree, all); }
                });
            }
        }, "gb-list").start();
    }

    private void choose(final Uri tree, final List<Entry> all) {
        final List<Entry> items = new ArrayList<>();
        for (Entry e : all) if (saveOnly ? Storage.isGbSave(e.name) : Storage.isGbRom(e.name)) items.add(e);
        Collections.sort(items, new Comparator<Entry>() {
            @Override public int compare(Entry a, Entry b) { return a.name.toLowerCase(Locale.ROOT).compareTo(b.name.toLowerCase(Locale.ROOT)); }
        });
        AlertDialog.Builder b = new AlertDialog.Builder(act).setTitle(saveOnly ? "Choose the save file" : "Choose your Game Boy game");
        if (items.isEmpty()) {
            b.setMessage(saveOnly ? "There is no save file (.sav) in this folder." : "There is no Game Boy game (.gb or .gbc) in this folder.");
        } else {
            String[] labels = new String[items.size()];
            for (int k = 0; k < labels.length; k++) {
                Entry e = items.get(k);
                labels[k] = e.parentName.isEmpty() ? e.name : e.parentName + " / " + e.name;
            }
            b.setItems(labels, new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) { take(tree, all, items.get(which)); }
            });
        }
        b.setNeutralButton("Other folder", new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) { pickFolder(); }
        });
        b.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) { nothing(); }
        });
        b.setOnCancelListener(new DialogInterface.OnCancelListener() {
            @Override public void onCancel(DialogInterface d) { nothing(); }
        });
        show(b);
    }

    // ---- taking the chosen file (and the save of the same name beside it) ---------------------------------------------------------------

    private void take(final Uri tree, final List<Entry> all, final Entry e) {
        final Context c = act.getApplicationContext();
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, e.docId);
                    if (saveOnly) {
                        Storage.GbResult r = Storage.importGbSaveOwn(c, uri);
                        done.done(r.path, r.brief, r.message);
                        return;
                    }
                    Entry save = null;                                // the save beside the game: its name, another ending
                    String stem = Storage.stem(e.name);
                    for (Entry s : all) {
                        if (s.parentId.equals(e.parentId) && Storage.isGbSave(s.name) && Storage.stem(s.name).equalsIgnoreCase(stem)) {
                            save = s;
                            break;
                        }
                    }
                    Uri saveUri = save == null ? null : DocumentsContract.buildDocumentUriUsingTree(tree, save.docId);
                    finishRom(c, uri, saveUri);
                } catch (IOException | RuntimeException ex) {
                    done.done("", "The file could not be stored.", "The file could not be stored: " + ex.getMessage());
                }
            }
        }, "gb-import").start();
    }

    /** The game (and its save, if one was found beside it) is copied into the app. */
    private void finishRom(Context c, Uri rom, Uri save) throws IOException {
        Storage.GbResult g = Storage.importGbRom(c, rom);
        if (g.path.isEmpty()) {
            done.done("", g.brief, g.message);
            return;
        }
        if (save != null) {
            try {
                Storage.GbResult s = Storage.importGbSave(c, save, g.path);
                done.done(g.path, "Game and its save are in.", g.message + " " + s.message);
            } catch (IOException | RuntimeException ex) {
                done.done(g.path, "Game is in; the save failed.", g.message + " Its save could not be stored: " + ex.getMessage());
            }
            return;
        }
        done.done(g.path, g.hasSave ? "Game is in; its save is kept." : "Game is in; no save found: new.",
                g.message + (g.hasSave ? " The save of this game that is already here is used." : " No save with the same name was found next to it: it starts with a new save."));
    }

    // ---- the answers of the system's pickers ----------------------------------------------------------------------------------------------

    /** Returns true if the result was one of this flow's. */
    boolean onResult(int code, int result, Intent data) {
        if (code != REQ_FOLDER && code != REQ_ROM && code != REQ_SAVE) return false;
        final Uri uri = result == Activity.RESULT_OK && data != null ? data.getData() : null;
        final Context c = act.getApplicationContext();
        if (uri == null) {
            nothing();
            return true;
        }
        if (code == REQ_FOLDER) {
            try {
                act.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (RuntimeException ignored) {
                // some providers give no lasting permission: it works for this time
            }
            prefs().edit().putString(PREF_FOLDER, uri.toString()).apply();
            list(uri);
            return true;
        }
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    if (saveOnly) {
                        Storage.GbResult r = Storage.importGbSaveOwn(c, uri);
                        done.done(r.path, r.brief, r.message);
                    } else {
                        finishRom(c, uri, null);
                    }
                } catch (IOException | RuntimeException ex) {
                    done.done("", "The file could not be stored.", "The file could not be stored: " + ex.getMessage());
                }
            }
        }, "gb-import").start();
        return true;
    }
}
