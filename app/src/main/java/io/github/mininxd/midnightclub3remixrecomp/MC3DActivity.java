package io.github.mininxd.midnightclub3remixrecomp;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import android.util.Log;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

// SDL's activity running libmain.so. The game goes in the mc3dr folder on the phone's storage
// (/storage/emulated/0/Black) or at the top of an SD card. Reading it needs all files access, a special access Android grants on a settings page
// of its own, never among the app's ordinary permissions. This asks for it and says where the game goes, each time
// the app comes back without them; the game itself waits and starts as soon as it finds the files.
// At launch it also asks GitHub for a newer release (UpdateChecker) and tells of it once nothing else is asked.
public class MC3DActivity extends SDLActivity {
    static final String SHARED = "mc3dr";

    private AlertDialog dialog;  // the one up, at most one at a time
    private boolean notNow;      // the player turned the access down this run
    private boolean resumed;
    private UpdateChecker.Release update;  // a newer release not shown yet
    private final GameImporter importer = new GameImporter(this);
    private boolean importing;  // the file picker or the copy is up: nothing else is asked meanwhile
    private boolean checking;   // a check asked from the menu is under way

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    public static native void nativeRecordCrash(String category, String details);

    private static void recordJavaCrash(Thread thread, Throwable throwable) {
        StringBuilder sb = new StringBuilder();
        sb.append("Uncaught Java Exception on thread: ").append(thread.getName())
          .append(" (id=").append(thread.getId()).append(")\n\n");
        sb.append("Exception: ").append(throwable.toString()).append("\n\n");
        sb.append("Stack Trace:\n");
        StringWriter sw = new StringWriter();
        PrintWriter pw = new PrintWriter(sw);
        throwable.printStackTrace(pw);
        sb.append(sw.toString());

        String details = sb.toString();
        Log.e("MC3DR_Crash", details);

        // 1. Pass to native crash reporter so it captures with in-memory execution logs
        try {
            nativeRecordCrash("Java Uncaught Exception", details);
        } catch (Throwable t) {
            Log.e("MC3DR_Crash", "Failed to forward crash to native logger: " + t);
        }

        // 2. Persist directly to external storage in case native code cannot be reached
        String[] paths = new String[] {
            "/storage/emulated/0/mc3dr/crash_log.txt",
            "/sdcard/mc3dr/crash_log.txt"
        };
        for (String p : paths) {
            try {
                File f = new File(p);
                File dir = f.getParentFile();
                if (dir != null && !dir.exists()) dir.mkdirs();
                try (FileOutputStream fos = new FileOutputStream(f)) {
                    fos.write(details.getBytes(StandardCharsets.UTF_8));
                    fos.flush();
                    fos.getFD().sync();
                }
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    public void setRequestedOrientation(int requestedOrientation) {
        // Always honor sensorLandscape so both normal & flipped landscape work via accelerometer
        // (SDL3's setOrientationBis otherwise locks non-resizable fullscreen windows to reverseLandscape).
        super.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        final Thread.UncaughtExceptionHandler defaultHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                recordJavaCrash(thread, throwable);
            } catch (Throwable ignored) {
            }
            if (defaultHandler != null) {
                defaultHandler.uncaughtException(thread, throwable);
            }
        });

        try {
            for (View v : new View[] {mSurface, mLayout})
                if (v != null) v.setOnCapturedPointerListener(MC3DActivity::onCapturedPointer);
        } catch (Throwable ignored) {
        }
        File own = getExternalFilesDir(null);
        if (own != null) new File(own, "assets").mkdirs();
        // every launch unless turned off: a newer release is shown once nothing else is asked
        if (UpdateChecker.startupCheck(this)) {
            UpdateChecker.check(this, false, (r, failed) -> {
                if (r == null) return;
                update = r;
                tellOfUpdate();
            });
        }
    }

    // STARTUP UPDATE CHECK in the options menu, from the game's thread
    public boolean isStartupCheckEnabled() {
        return UpdateChecker.startupCheck(this);
    }

    public void setStartupCheckEnabled(boolean on) {
        UpdateChecker.setStartupCheck(this, on);
    }

    // A controller some phones' game services pass on as a virtual device (MIUI's took a DualShock 4 on a Redmi Note
    // 8 Pro): SDL sees no controller in it and drops its input. Its buttons and sticks go to the game as port 0's
    // here (native/input.cpp); a controller SDL has goes its own way as before.
    private static native void nativeLooseKey(int keyCode, boolean down);
    private static native void nativeLooseAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx,
                                               float hy);
    private static native void nativeLooseRelease();

    private static boolean loose(InputEvent e, int sources) {
        return !mBrokenLibraries && (e.getSource() & sources) != 0
                && !SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId());
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        boolean pad = KeyEvent.isGamepadButton(e.getKeyCode()) || (e.getKeyCode() >= KeyEvent.KEYCODE_DPAD_UP
                && e.getKeyCode() <= KeyEvent.KEYCODE_DPAD_RIGHT);
        if (pad && loose(e, InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) {
            if (e.getRepeatCount() == 0) nativeLooseKey(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (loose(e, InputDevice.SOURCE_JOYSTICK) && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // the right stick on Z and RZ, the triggers on LTRIGGER and RTRIGGER or BRAKE and GAS, as Android has them
            nativeLooseAxes(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                    e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                    e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    // The mouse while the game holds it (SDL's relative mode, Android's pointer capture). SDL's own handler passes on
    // only moves with no button held and the wheel: clicks and the moves while firing were lost. Each sample of a
    // batch is a move of its own, as SDL's C side takes them.
    private static boolean onCapturedPointer(View view, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_MOVE:
                for (int h = 0; h < e.getHistorySize(); h++)
                    onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getHistoricalX(h), e.getHistoricalY(h), true);
                onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getX(), e.getY(), true);
                return true;
            case MotionEvent.ACTION_BUTTON_PRESS:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_DOWN, 0, 0, true);
                return true;
            case MotionEvent.ACTION_BUTTON_RELEASE:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_UP, 0, 0, true);
                return true;
            case MotionEvent.ACTION_SCROLL:
                onNativeMouse(0, MotionEvent.ACTION_SCROLL, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                        e.getAxisValue(MotionEvent.AXIS_VSCROLL), false);
                return true;
            default:
                // the down and up a press brings with it: the button events above carry it
                return true;
        }
    }

    // CHECK FOR UPDATES in the options menu, from the game's thread: the answer always shows, up to date or offline
    public void checkForUpdates() {
        runOnUiThread(() -> {
            if (checking) return;
            checking = true;
            Toast.makeText(this, "Checking for updates…", Toast.LENGTH_SHORT).show();
            UpdateChecker.check(this, true, (r, failed) -> {
                checking = false;
                if (r != null) {
                    update = r;
                    tellOfUpdate();
                    return;
                }
                if (isFinishing() || (dialog != null && dialog.isShowing())) return;
                String msg = failed ? "Couldn't reach GitHub. Check your internet connection and try again."
                                    : "You have the latest version (" + UpdateChecker.installedVersion(this) + ").";
                dialog = new AlertDialog.Builder(this)
                        .setTitle(failed ? "Update check failed" : "No update")
                        .setMessage(msg)
                        .setPositiveButton("OK", null)
                        .show();
            });
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        resumed = true;
        askForGame();
        tellOfUpdate();
    }

    @Override
    protected void onPause() {
        resumed = false;
        super.onPause();
    }

    // SDL's older immersive flags don't always come back after a dialog (MIUI on Android 11 kept the status bar):
    // the bars are hidden again each time the game has the focus, a swipe shows them for a moment
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (!mBrokenLibraries) nativeLooseRelease();  // its ups go elsewhere while the game is not in front
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < 30) {
            // Android 10: the older immersive flags
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // Android 11 on has all files access, a page of its own in the settings; Android 10 the storage permission, with
    // the legacy storage the manifest asks for
    private static final boolean ALL_FILES_ACCESS = Build.VERSION.SDK_INT >= 30;
    private static final int STORAGE_REQUEST = 0x426C;
    private boolean storageAsked;  // the permission was asked for this run

    private boolean hasStorageAccess() {
        if (ALL_FILES_ACCESS) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    public boolean isExtracting() {
        return importing;
    }

    // checks whether Midnight Club 3 is extracted (default.xbe and assets_x.dat exist)
    private static boolean isGameExtracted(File dir) {
        if (dir == null || !dir.exists()) return false;
        return new File(dir, "default.xbe").exists() && new File(dir, "assets_x.dat").exists();
    }

    private void askForGame() {
        if (importing || (dialog != null && dialog.isShowing())) return;

        // 1. Detect whether storage permission is allowed
        if (!hasStorageAccess()) {
            if (!notNow) askStoragePermission();
            return;
        }

        // 2. Storage is allowed: check shared storage /storage/emulated/0/mc3dr
        File shared = new File(Environment.getExternalStorageDirectory(), SHARED);
        if (!shared.exists()) {
            shared.mkdirs();
        }

        // Check if game is already extracted
        if (isGameExtracted(shared) || isGameExtracted(new File(shared, "assets"))) return;
        for (File card : cardFolders()) {
            if (isGameExtracted(card) || isGameExtracted(new File(card, "assets"))) return;
        }
        File own = getExternalFilesDir(null);
        if (own != null && isGameExtracted(new File(own, "assets"))) return;

        // 3. Game is not extracted: check if an .iso exists in mc3dr
        File[] isos = shared.listFiles((d, name) -> name.toLowerCase().endsWith(".iso"));
        if (isos != null && isos.length > 0) {
            File isoToExtract = isos[0];
            for (File f : isos) {
                if (f.getName().toLowerCase().contains("midnight") || f.getName().toLowerCase().contains("mc3")) {
                    isoToExtract = f;
                    break;
                }
            }
            startExtraction(isoToExtract, shared);
            return;
        }

        // 4. mc3dr folder or game does not exist: prompt to pick the ISO file
        promptPickIso();
    }

    // the mc3dr folder on each SD card or USB stick mounted, as native/game_files.cpp looks in them
    private List<File> cardFolders() {
        List<File> out = new ArrayList<>();
        if (!ALL_FILES_ACCESS) {
            // Android 10: each card's root above the app's own folder on it, <root>/Android/data/<package>/files
            File[] own = getExternalFilesDirs(null);
            for (int i = 1; i < own.length; i++) {
                File root = own[i];
                for (int up = 0; root != null && up < 4; up++) root = root.getParentFile();
                if (root != null) out.add(new File(root, SHARED));
            }
            return out;
        }
        StorageManager sm = getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (v.isPrimary() || root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            out.add(new File(root, SHARED));
        }
        return out;
    }

    // the same folders for native/game_files.cpp, from the game's thread: some phones (a Sony on Android 11) don't let
    // the app list /storage, though each card's folder opens
    public String[] cardFolderPaths() {
        List<File> cards = cardFolders();
        String[] out = new String[cards.size()];
        for (int i = 0; i < out.length; i++) out[i] = cards.get(i).getPath();
        return out;
    }

    // the newer release, once the app is in front with no other dialog up; shown once a run
    private void tellOfUpdate() {
        if (update == null || importing || !resumed || isFinishing() || (dialog != null && dialog.isShowing())) return;
        UpdateChecker.Release r = update;
        update = null;
        String installed = "";
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            // only for the message
        }
        StringBuilder msg = new StringBuilder(r.name + " is available");
        if (!installed.isEmpty()) msg.append(" (you have ").append(installed).append(")");
        msg.append('.');
        if (!r.changes.isEmpty()) msg.append("\n\nWhat's new:\n").append(r.changes);
        msg.append("\n\nInstall it over this one: your settings and saves stay.");
        dialog = new AlertDialog.Builder(this)
                .setTitle("Update available")
                .setMessage(msg)
                .setPositiveButton("Download", (d, w) -> open(r.apk != null ? r.apk : r.page))
                .setNeutralButton("Skip this version", (d, w) -> UpdateChecker.skip(this, r.tag))
                .setNegativeButton("Later", null)
                .show();
    }

    private void open(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            // no browser: nothing to open it with
        }
    }

    private void askStoragePermission() {
        if (!ALL_FILES_ACCESS) {
            boolean ask = !storageAsked || shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE);
            if (ask) {
                storageAsked = true;
                requestPermissions(new String[] {
                        Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, STORAGE_REQUEST);
            } else {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
            return;
        }
        dialog = new AlertDialog.Builder(this)
                .setTitle("Storage Permission Required")
                .setMessage("MC3D Remix requires storage access to extract and run the game files in /storage/emulated/0/" + SHARED + ".\n\n"
                        + "Please turn on \"Allow access to manage all files\" on the next screen.")
                .setCancelable(false)
                .setPositiveButton("Allow Access", (d, w) -> openAccessSettings())
                .setNegativeButton("Not Now", (d, w) -> notNow = true)
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    private void promptPickIso() {
        dialog = new AlertDialog.Builder(this)
                .setTitle("Select Game ISO")
                .setMessage("Please select your Midnight Club 3: DUB Edition Remix (.iso) disc image to extract and install the game.")
                .setCancelable(false)
                .setPositiveButton("Select ISO", (d, w) -> startImport())
                .setNegativeButton("Later", null)
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    private void startImport() {
        importing = importer.pick();
    }

    private void startExtraction(File isoFile, File targetDir) {
        importing = true;
        importer.extract(isoFile, targetDir, ok -> {
            importing = false;
            if (!ok) {
                askForGame();
            } else {
                Toast.makeText(this, "Game extracted successfully! Starting...", Toast.LENGTH_SHORT).show();
            }
            tellOfUpdate();
        });
    }

    private void startExtraction(Uri uri, File targetDir) {
        importing = true;
        importer.extract(uri, targetDir, ok -> {
            importing = false;
            if (!ok) {
                askForGame();
            } else {
                Toast.makeText(this, "Game extracted successfully! Starting...", Toast.LENGTH_SHORT).show();
            }
            tellOfUpdate();
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != GameImporter.REQUEST) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            importing = false;
            return;
        }
        File target = hasStorageAccess()
                ? new File(Environment.getExternalStorageDirectory(), SHARED)
                : new File(getExternalFilesDir(null), "assets");
        startExtraction(uri, target);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == STORAGE_REQUEST) {
            if (hasStorageAccess()) {
                notNow = false;
                askForGame();
            }
        }
    }

    // the app's own page of the access, or the list of every app's where a phone has no such page. Android 10 asks
    // for the storage permission, or opens the app's details once the player has turned it down for good
    private void openAccessSettings() {
        if (!ALL_FILES_ACCESS) {
            boolean ask = !storageAsked || shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE);
            if (ask) {
                storageAsked = true;
                requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE}, STORAGE_REQUEST);
            } else {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
            return;
        }
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (ActivityNotFoundException e2) {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
        }
    }
}
