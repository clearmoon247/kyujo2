// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo2.game;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * The app's folder and what moves in and out of it. Everything the game uses lives in ONE folder, the app's private files folder (no permission is needed and it
 * does not move when a card is removed): the ROM, the Game Boy ROMs and saves, the settings, the game save and the log. The native shell gets the folder as
 * "--root" and the game sees it as "/" (the in-game file browser starts there).
 *
 * Files are moved with the system's file picker: the player chooses a document, this class copies its bytes (nothing is ever read from a path of another app).
 */
final class Storage {
    private Storage() {}

    static final String LOG = "kyujo-sdl-log.txt";
    static final String LOG_PREV = "kyujo-sdl-log.prev.txt";

    static File dir(Context c) {
        File d = c.getFilesDir();
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        return d;
    }

    static boolean isRom(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".z64") || n.endsWith(".n64") || n.endsWith(".v64");
    }

    static boolean isSave(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".bin") || n.endsWith(".sav") || n.endsWith(".srm") || n.endsWith(".cfg") || n.endsWith(".eep") || n.endsWith(".fla") || n.endsWith(".mpk");
    }

    /** The ROM file in the folder (the shell takes the first one it finds), or null. */
    static File rom(Context c) {
        File current = new File(dir(c),Cartridge.FILE);
        if (current.isFile()) return current;
        File old = new File(dir(c),Cartridge.LEGACY_FILE);
        return old.isFile() ? old : null;
    }

    static String displayName(ContentResolver r, Uri uri) {
        String name = null;
        try (Cursor q = r.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (q != null && q.moveToFirst()) name = q.getString(0);
        } catch (RuntimeException ignored) {
            // some providers refuse the query: the last part of the path is used
        }
        if (name == null) name = uri.getLastPathSegment();
        if (name == null) name = "file";
        int slash = name.lastIndexOf('/');
        return slash >= 0 ? name.substring(slash + 1) : name;
    }

    /** A file name that cannot leave the folder (no path parts, no hidden-file dots). */
    static String safeName(String name) {
        String n = name.replace('\\', '/');
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        while (n.startsWith(".")) n = n.substring(1);
        return n.isEmpty() ? "file" : n;
    }

    static long copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        long total = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            total += n;
        }
        return total;
    }

    /** Copies the document into `target`; the data is written to a temporary name first, so a failed copy leaves the old file alone. */
    static long importTo(ContentResolver r, Uri uri, File target) throws IOException {
        File tmp = new File(target.getParentFile(), target.getName() + ".part");
        long n;
        try (InputStream in = r.openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException("the file could not be opened");
            n = copy(in, out);
        }
        
        if (!tmp.renameTo(target)) throw new IOException("the file could not be stored");
        return n;
    }

    /** Imports the game ROM: replaces any ROM already there (the shell takes the first ROM file it finds). */
    static String importRom(Context c, Uri uri) throws IOException {
        File tmp=File.createTempFile("stadium-import-", ".tmp", dir(c));
        try {
            java.security.MessageDigest hash;
            try { hash=java.security.MessageDigest.getInstance("SHA-1"); }
            catch(java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
            try (InputStream src=c.getContentResolver().openInputStream(uri);
                 java.io.DataInputStream in=new java.io.DataInputStream(src);
                 OutputStream dst=new FileOutputStream(tmp)) {
                byte[] block=new byte[65536];
                int swap=0;
                for(int at=0;at<67108864;at+=block.length) {
                    in.readFully(block);
                    if(at==0) {
                        int first=block[0]&255;
                        if(first==0x37) swap=1;
                        else if(first==0x40) swap=3;
                        else if(first!=0x80) throw new IOException("Choose the English Stadium 2 cartridge.");
                    }
                    if(swap!=0) for(int i=0;i<block.length;i++) if((i^swap)>i) {
                        byte b=block[i];block[i]=block[i^swap];block[i^swap]=b;
                    }
                    dst.write(block); hash.update(block);
                }
                if(in.read()!=-1) throw new IOException("Expected a 64 MiB USA Stadium 2 ROM.");
            }
            StringBuilder digest=new StringBuilder();
            for(byte b:hash.digest()) digest.append(String.format(Locale.ROOT,"%02x",b&255));
            if(!Cartridge.SHA1.contentEquals(digest)) throw new IOException("This ROM does not match "+Cartridge.GAME+".");
            if(!tmp.renameTo(new File(dir(c),Cartridge.FILE))) throw new IOException("Could not store the imported ROM.");
            return Cartridge.GAME;
        } finally { tmp.delete(); }
    }

    static boolean isGbRom(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".gb") || n.endsWith(".gbc");
    }

    /** The file name a save is kept under: always ends in ".sav" (a save chosen as "Pokemon Red" or "Pokemon Red.dat" is stored as "Pokemon Red.sav" / "Pokemon Red.dat.sav"). */
    static String saveName(String name) {
        return isGbSave(name) ? name : name + ".sav";
    }

    static boolean isGbSave(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".sav") || n.endsWith(".srm");
    }

    /** The name without its extension. */
    static String stem(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    static File gbDir(Context c) {
        File gb = new File(dir(c), "GB");
        //noinspection ResultOfMethodCallIgnored
        gb.mkdirs();
        return gb;
    }

    /** The path the game uses for a file of the folder: the folder is "/" for the game. */
    static String gamePath(Context c, File f) {
        String root = dir(c).getAbsolutePath();
        String p = f.getAbsolutePath();
        return p;
    }

    private static boolean sameContent(File a, File b) throws IOException {
        if (a.length() != b.length()) return false;
        try (InputStream x = new FileInputStream(a); InputStream y = new FileInputStream(b)) {
            byte[] p = new byte[1 << 15], q = new byte[1 << 15];
            int n;
            while ((n = x.read(p)) > 0) {
                int got = 0;
                while (got < n) {
                    int m = y.read(q, got, n - got);
                    if (m <= 0) return false;
                    got += m;
                }
                for (int i = 0; i < n; i++) if (p[i] != q[i]) return false;
            }
        }
        return true;
    }

    /**
     * Puts a save file under its final name. An existing save of that name is never lost: unless it is identical it is kept as "<name>-backup-N.sav" first (a player who
     * chooses a save for a game that already has progress here must be able to get that progress back).
     */
    static String placeSave(File incoming, File target) throws IOException {
        String note = "";
        if (target.exists()) {
            if (sameContent(incoming, target)) {
                //noinspection ResultOfMethodCallIgnored
                incoming.delete();
                return note;
            }
            File backup;
            int n = 1;
            do {
                backup = new File(target.getParentFile(), stem(target.getName()) + "-backup-" + n++ + ".sav");
            } while (backup.exists());
            if (!target.renameTo(backup)) throw new IOException("the old save could not be kept");
            note = " The save that was there is kept as " + backup.getName() + ".";
        }
        if (!incoming.renameTo(target)) throw new IOException("the save could not be stored");
        return note;
    }

    /** What an import of a Game Boy file came to: the game's path of the ROM (or of the save) that is now in use, and the text for the player. */
    static final class GbResult {
        String path = "";
        String name = "";          // the file's name
        String message = "";       // all the words, for the player (a toast, the launcher)
        String brief = "";         // one short line (at most about 40 letters): the game's menu has room for no more
        boolean hasSave;           // the game already has a save under its automatic name (<rom>.sav)
    }

    /**
     * The game file chosen in step 1: copied into the folder "GB" (a name without .gb/.gbc gets ".gb": the player chose it as the game, and the game checks the header itself).
     * A save file or a zip is refused with a plain sentence.
     */
    static GbResult importGbRom(Context c, Uri uri) throws IOException {
        GbResult res = new GbResult();
        String name = safeName(displayName(c.getContentResolver(), uri));
        String lower = name.toLowerCase(Locale.ROOT);
        if (isGbSave(name) || lower.endsWith(".zip") || lower.endsWith(".7z") || lower.endsWith(".rar")) {
            res.message = name + " is not a game file. Choose the Game Boy game itself (.gb or .gbc).";
            res.brief = "That is not a game file (.gb or .gbc).";
            return res;
        }
        if (!isGbRom(name)) name = name + ".gb";
        File gb = gbDir(c);
        File rom = new File(gb, name);
        importTo(c.getContentResolver(), uri, rom);
        res.name = name;
        res.path = gamePath(c, rom);
        res.hasSave = new File(gb, stem(name) + ".sav").exists();
        res.message = name + " is in.";
        res.brief = "Game is in.";
        return res;
    }

    /** One save file chosen on its own (the Save file item of the Game Pak page): stored under its own name, like a save file picked in the file browser on every other system. */
    static GbResult importGbSaveOwn(Context c, Uri uri) throws IOException {
        GbResult res = new GbResult();
        String name = saveName(safeName(displayName(c.getContentResolver(), uri)));
        File gb = gbDir(c);
        File incoming = new File(gb, name + ".incoming");
        importTo(c.getContentResolver(), uri, incoming);
        File target = new File(gb, name);
        String note = placeSave(incoming, target);
        res.path = gamePath(c, target);
        res.message = name + " is the save file." + note;
        res.brief = note.isEmpty() ? "Save file chosen." : "Save file chosen; the old one is kept.";
        return res;
    }

    /** A save file chosen for a ROM that is in use (`romGamePath`, the game's path): it becomes that ROM's save under the automatic name (the ROM's name + ".sav"). */
    static GbResult importGbSave(Context c, Uri uri, String romGamePath) throws IOException {
        GbResult res = new GbResult();
        if (romGamePath == null || romGamePath.isEmpty()) {
            res.message = "Choose a Game Boy ROM first: the save belongs to it.";
            res.brief = "Choose the ROM first.";
            return res;
        }
        File rom = new File(romGamePath);
        File dirOfRom = rom.getParentFile() != null ? rom.getParentFile() : gbDir(c);
        File incoming = new File(dirOfRom, stem(rom.getName()) + ".sav.incoming");
        importTo(c.getContentResolver(), uri, incoming);
        File target = new File(dirOfRom, stem(rom.getName()) + ".sav");
        String note = placeSave(incoming, target);
        res.path = gamePath(c, target);
        res.message = "The save is in use as " + target.getName() + "." + note;
        res.brief = note.isEmpty() ? "Save is in." : "Save is in; the old one is kept.";
        return res;
    }

    /** The files of the backup: saves and settings in the folder and in GB (never the ROMs, which belong to the player and are large). */
    static List<File> backupFiles(Context c) {
        List<File> out = new ArrayList<>();
        File[] top = dir(c).listFiles();
        if (top != null) for (File f : top) if (f.isFile() && isSave(f.getName())) out.add(f);
        File[] gb = new File(dir(c), "GB").listFiles();
        if (gb != null) for (File f : gb) if (f.isFile() && isSave(f.getName())) out.add(f);
        File[] stadium = new File(dir(c), "saves").listFiles();
        if (stadium != null) for(File f:stadium) if(f.isFile() && isSave(f.getName())) out.add(f);
        File[] slots = new File(dir(c), "Game Paks").listFiles();
        if(slots != null) for(File slot:slots) {
            File[] files=slot.listFiles();
            if(files != null) for(File f:files) if(f.isFile() && isSave(f.getName())) out.add(f);
        }
        Collections.sort(out);
        return out;
    }

    static int writeBackup(Context c, OutputStream os) throws IOException {
        int n = 0;
        File root = dir(c);
        try (ZipOutputStream z = new ZipOutputStream(os)) {
            for (File f : backupFiles(c)) {
                String rel = f.getAbsolutePath().substring(root.getAbsolutePath().length()+1);
                z.putNextEntry(new ZipEntry(rel));
                try (InputStream in = new FileInputStream(f)) {
                    copy(in, z);
                }
                z.closeEntry();
                n++;
            }
        }
        return n;
    }

    /** Restores a backup made by writeBackup: only plain files in the folder or in GB, with a save or settings extension; nothing else is written. */
    static int restoreBackup(Context c, InputStream is) throws IOException {
        int n = 0;
        try (ZipInputStream z = new ZipInputStream(is)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String path = e.getName().replace('\\', '/');
                if (e.isDirectory()) continue;
                File target;
                if (path.startsWith("GB/") && path.indexOf('/', 3) < 0) {
                    File gb = new File(dir(c), "GB");
                    //noinspection ResultOfMethodCallIgnored
                    gb.mkdirs();
                    target = new File(gb, safeName(path.substring(3)));
                } else if (path.matches("saves/[A-Za-z0-9_-]+[.]bin") || path.matches("Game Paks/port-[1-4]-[0-9]+/(battery|original-with-clock)[.]sav")) {
                    target=new File(dir(c),path);
                    target.getParentFile().mkdirs();
                } else if (path.indexOf('/') < 0) {
                    String name = safeName(path);
                    target = new File(dir(c), name);
                } else {
                    continue;
                }
                if (!isSave(target.getName())) continue;
                try (OutputStream out = new FileOutputStream(target)) {
                    copy(z, out);
                }
                n++;
            }
        }
        return n;
    }

    /** The log of this run and of the one before, for a bug report. */
    static int writeLogs(Context c, OutputStream os) throws IOException {
        int n = 0;
        for (String name : new String[] {LOG_PREV, LOG}) {
            File f = new File(dir(c), name);
            if (!f.isFile()) continue;
            os.write(("==== " + name + " ====\n").getBytes("UTF-8"));
            try (InputStream in = new FileInputStream(f)) {
                copy(in, os);
            }
            os.write('\n');
            n++;
        }
        return n;
    }
}
