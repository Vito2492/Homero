package io.github.vito2492.mansion;

import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The app: SDL's activity, plus the file chooser that brings the player's own
 * ROM into the app's folder (as addams.sfc). No game comes with the app.
 */
public class MansionActivity extends SDLActivity {
    private static final int PICK_ROM = 4243;
    // 0 asking, choosing or copying, 1 copied, 2 cancelled, 3 failed, 4 quit (read from C)
    private static volatile int pickState = 1;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /** Called from C: explain what's needed, then open the system's file chooser. */
    public void askRom(final String text) {
        pickState = 0;
        runOnUiThread(() -> new AlertDialog.Builder(this)
            .setTitle("Mansion")
            .setMessage(text)
            .setCancelable(false)
            .setPositiveButton("OK", (d, w) -> openPicker())
            .setNegativeButton("X", (d, w) -> pickState = 4)
            .show());
    }

    private void openPicker() {
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, PICK_ROM);
        } catch (Exception e) {
            pickState = 3;
        }
    }

    /** Called from C, while it waits. */
    public int pickState() {
        return pickState;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK_ROM) {
            super.onActivityResult(request, result, data);
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null) {
            pickState = 2;
            return;
        }
        final Uri uri = data.getData();
        new Thread(() -> {
            boolean ok;
            try {
                File out = new File(getFilesDir(), "addams.sfc");
                String name = displayName(uri).toLowerCase();
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (name.endsWith(".zip")) {
                        ok = false;
                        try (ZipInputStream zip = new ZipInputStream(in)) {
                            ZipEntry e;
                            while ((e = zip.getNextEntry()) != null) {
                                String n = e.getName().toLowerCase();
                                if (!e.isDirectory() && (n.endsWith(".sfc") || n.endsWith(".smc"))) {
                                    save(zip, out);
                                    ok = true;
                                    break;
                                }
                            }
                        }
                    } else {
                        save(in, out);
                        ok = true;
                    }
                }
            } catch (Exception e) {
                ok = false;
            }
            final boolean done = ok;
            runOnUiThread(() -> Toast.makeText(this, done ? "OK" : "ROM?", Toast.LENGTH_SHORT).show());
            pickState = done ? 1 : 3;
        }).start();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception e) {
            // fall through
        }
        return uri.getLastPathSegment() == null ? "" : uri.getLastPathSegment();
    }

    private void save(InputStream in, File out) throws Exception {
        File tmp = new File(out.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        }
        if (!tmp.renameTo(out)) throw new Exception("rename " + out);
    }
}
