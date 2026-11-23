package io.github.mininxd.midnightclub3remixrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.BitSet;
import java.util.List;
import java.util.function.Consumer;

// Extracts Xbox disc images (.iso) directly into the game folder (/storage/emulated/0/mc3dr).
// Xbox DVD file system (XISO) is parsed recursively, and all assets and default.xbe are unpacked.
// default.xbe is written last as default.xbe.tmp and renamed once complete to ensure the engine only boots full assets.
final class GameImporter {
    static final int REQUEST = 0x8A11;
    private static final String TAG = "MC3DImport";
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    // where the volume descriptor sits: the game partition alone (0), redump, XGD3, XGD1 video offsets
    private static final long[] PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L};
    private static final int BUFFER = 4 * 1024 * 1024; // 4MB buffer

    private final Activity activity;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean cancelled;

    GameImporter(Activity activity) {
        this.activity = activity;
    }

    boolean pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            activity.startActivityForResult(i, REQUEST);
            return true;
        } catch (ActivityNotFoundException e) {
            message("Import game", "This phone has no file picker to choose the disc image with.", null);
            return false;
        }
    }

    private static class IsoEntry {
        final String relativePath;
        final long offset;
        final long size;
        final boolean isDir;

        IsoEntry(String relativePath, long offset, long size, boolean isDir) {
            this.relativePath = relativePath;
            this.offset = offset;
            this.size = size;
            this.isDir = isDir;
        }
    }

    @FunctionalInterface
    private interface ChannelOpener {
        FileChannel open() throws Exception;
    }

    void extract(File isoFile, File targetDir, Consumer<Boolean> done) {
        extractInternal(() -> new FileInputStream(isoFile).getChannel(), isoFile.getName(), isoFile.length(), targetDir, done);
    }

    void extract(Uri uri, File targetDir, Consumer<Boolean> done) {
        String name = displayName(uri);
        long size = size(uri);
        if (name == null || !name.toLowerCase().endsWith(".iso")) {
            message("Select Game ISO", "Please choose a Midnight Club 3 disc image (.iso).", () -> done.accept(false));
            return;
        }
        extractInternal(() -> {
            ParcelFileDescriptor pfd = activity.getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) throw new IOException("Cannot open Uri: " + uri);
            return new FileInputStream(pfd.getFileDescriptor()).getChannel();
        }, name, size, targetDir, done);
    }

    private void extractInternal(ChannelOpener opener, String displayName, long approxSize, File targetDir, Consumer<Boolean> done) {
        targetDir.mkdirs();

        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(true);
        TextView text = new TextView(activity);
        text.setText("Scanning disc image…");
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, pad / 2);
        box.addView(bar);
        box.addView(text);

        cancelled = false;
        AlertDialog progress = new AlertDialog.Builder(activity)
                .setTitle("Extracting " + (displayName != null ? displayName : "Game"))
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancel", (d, w) -> cancelled = true)
                .show();
        if (progress.getWindow() != null) {
            progress.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }

        new Thread(() -> {
            String error = null;
            File tmpXbe = new File(targetDir, "default.xbe.tmp");
            File finalXbe = new File(targetDir, "default.xbe");

            try (FileChannel src = opener.open()) {
                // 1. Locate Xbox volume descriptor (sector 32 of partition)
                long partitionBase = -1;
                long rootSector = 0;
                long rootSize = 0;
                ByteBuffer secBuf = ByteBuffer.allocate(2048);
                secBuf.order(ByteOrder.LITTLE_ENDIAN);

                for (long part : PARTITIONS) {
                    secBuf.clear();
                    int read = src.read(secBuf, part + 32L * 2048L);
                    if (read == 2048) {
                        secBuf.flip();
                        byte[] magic = new byte[MAGIC.length];
                        secBuf.get(magic);
                        if (Arrays.equals(magic, MAGIC)) {
                            partitionBase = part;
                            rootSector = secBuf.getInt(0x14) & 0xFFFFFFFFL;
                            rootSize = secBuf.getInt(0x18) & 0xFFFFFFFFL;
                            break;
                        }
                    }
                }

                if (partitionBase == -1) {
                    error = (displayName != null ? displayName : "Selected file") + " is not a valid Xbox disc image.";
                } else {
                    // 2. Parse directory tree
                    List<IsoEntry> entries = new ArrayList<>();
                    parseDir(src, partitionBase, rootSector, rootSize, "", entries);

                    List<IsoEntry> files = new ArrayList<>();
                    long totalBytes = 0;
                    boolean hasXbe = false;
                    for (IsoEntry e : entries) {
                        if (!e.isDir) {
                            files.add(e);
                            totalBytes += e.size;
                            if (e.relativePath.equalsIgnoreCase("default.xbe")) {
                                hasXbe = true;
                            }
                        }
                    }

                    if (!hasXbe) {
                        error = "The disc image contains no default.xbe.";
                    } else if (totalBytes > 0 && targetDir.getUsableSpace() < totalBytes + (64L << 20)) {
                        error = String.format("Not enough storage space. The game needs %.2f GB free, but only %.2f GB are available.",
                                totalBytes / 1e9, targetDir.getUsableSpace() / 1e9);
                    } else {
                        main.post(() -> bar.setIndeterminate(false));

                        long totalExtracted = 0;
                        long lastShown = 0;
                        ByteBuffer directBuf = ByteBuffer.allocateDirect(BUFFER);

                        // Move default.xbe to the end of the extraction list so it is created last
                        IsoEntry xbeEntry = null;
                        for (int i = 0; i < files.size(); i++) {
                            if (files.get(i).relativePath.equalsIgnoreCase("default.xbe")) {
                                xbeEntry = files.remove(i);
                                break;
                            }
                        }
                        if (xbeEntry != null) {
                            files.add(xbeEntry);
                        }

                        for (IsoEntry fe : files) {
                            if (cancelled) break;

                            boolean isXbe = fe.relativePath.equalsIgnoreCase("default.xbe");
                            File dst = isXbe ? tmpXbe : new File(targetDir, fe.relativePath);
                            File parent = dst.getParentFile();
                            if (parent != null && !parent.exists()) parent.mkdirs();

                            try (FileOutputStream fos = new FileOutputStream(dst);
                                 FileChannel dstChan = fos.getChannel()) {
                                long remaining = fe.size;
                                long pos = fe.offset;

                                while (!cancelled && remaining > 0) {
                                    int chunk = (int) Math.min(remaining, BUFFER);
                                    directBuf.clear();
                                    directBuf.limit(chunk);
                                    src.position(pos);
                                    int got = src.read(directBuf);
                                    if (got <= 0) break;
                                    directBuf.flip();
                                    dstChan.write(directBuf);

                                    pos += got;
                                    remaining -= got;
                                    totalExtracted += got;

                                    if (totalExtracted - lastShown >= (8L << 20)) {
                                        lastShown = totalExtracted;
                                        long c = totalExtracted;
                                        long tot = totalBytes;
                                        String fname = fe.relativePath;
                                        main.post(() -> showProgress(bar, text, fname, c, tot));
                                    }
                                }
                                fos.getFD().sync();
                            }
                        }

                        if (!cancelled) {
                            if (tmpXbe.exists()) {
                                if (finalXbe.exists()) finalXbe.delete();
                                if (!tmpXbe.renameTo(finalXbe)) {
                                    error = "Failed to finalize default.xbe.";
                                }
                            }
                        }
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "Extraction failed", e);
                error = "Extraction failed: " + e.getMessage();
            }

            if (cancelled || error != null) {
                if (tmpXbe.exists()) tmpXbe.delete();
            }

            final String failReason = error;
            final boolean success = (error == null && !cancelled);

            main.post(() -> {
                try {
                    progress.dismiss();
                } catch (Exception ignored) {
                }
                if (success) {
                    Log.i(TAG, "Game extracted successfully into " + targetDir);
                    done.accept(true);
                } else if (failReason != null) {
                    message("Extraction Failed", failReason, () -> done.accept(false));
                } else {
                    done.accept(false); // user cancelled
                }
            });
        }, "GameExtractor").start();
    }

    private void parseDir(FileChannel src, long partitionBase, long sector, long size, String parentPath, List<IsoEntry> outEntries) throws IOException {
        if (size <= 0 || size > 16 * 1024 * 1024) return;
        ByteBuffer table = ByteBuffer.allocate((int) size);
        table.order(ByteOrder.LITTLE_ENDIAN);
        src.read(table, partitionBase + sector * 2048L);
        table.flip();

        ArrayDeque<Integer> todo = new ArrayDeque<>();
        BitSet seen = new BitSet((int) (size / 4 + 1));
        todo.push(0);

        while (!todo.isEmpty()) {
            int at = todo.pop();
            if (at + 14 > size || seen.get(at / 4)) continue;
            seen.set(at / 4);

            int left = table.getShort(at) & 0xFFFF;
            int right = table.getShort(at + 2) & 0xFFFF;
            if (left == 0xFFFF) continue;

            long sec = table.getInt(at + 4) & 0xFFFFFFFFL;
            long fsize = table.getInt(at + 8) & 0xFFFFFFFFL;
            int attr = table.get(at + 12) & 0xFF;
            int nameLen = table.get(at + 13) & 0xFF;

            if (at + 14 + nameLen > size) continue;
            byte[] nameBytes = new byte[nameLen];
            for (int i = 0; i < nameLen; i++) {
                nameBytes[i] = table.get(at + 14 + i);
            }
            String name = new String(nameBytes, StandardCharsets.US_ASCII);

            if (left != 0) todo.push(left * 4);
            if (right != 0) todo.push(right * 4);

            boolean isDir = (attr & 0x10) != 0;
            String relPath = parentPath.isEmpty() ? name : (parentPath + File.separator + name);

            outEntries.add(new IsoEntry(relPath, partitionBase + sec * 2048L, fsize, isDir));

            if (isDir) {
                parseDir(src, partitionBase, sec, fsize, relPath, outEntries);
            }
        }
    }

    private static void showProgress(ProgressBar bar, TextView text, String fileName, long extracted, long total) {
        if (total > 0) {
            bar.setProgress((int) (extracted * 1000 / total));
            text.setText(String.format("Extracting %s\n%.2f of %.2f GB (%d%%)",
                    fileName, extracted / 1e9, total / 1e9, (int) (extracted * 100 / total)));
        } else {
            text.setText(String.format("Extracting %s\n%.2f GB", fileName, extracted / 1e9));
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return new File(c.getString(0)).getName();
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? null : new File(last).getName();
    }

    private long size(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return -1;
    }

    private void message(String title, String text, Runnable after) {
        new AlertDialog.Builder(activity)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton("OK", null)
                .setOnDismissListener(d -> {
                    if (after != null) after.run();
                })
                .show();
    }
}
