package com.libertyrecomp;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Hosts the Graine RexGlue runtime.
 *
 * <p>SDL owns the window, the event loop and the native thread. Before SDL
 * loads libmain.so this class only prepares what the runtime reads from the
 * environment:</p>
 * <ul>
 *   <li>{@code XDG_DATA_HOME} - the app's external files directory. The title
 *       keeps everything under {@code LibertyRecomp/} there, so the game is
 *       pushed to {@code /sdcard/Android/data/com.libertyrecomp/files/LibertyRecomp/game}
 *       without any storage permission.</li>
 *   <li>{@code REX_RESOURCES_DIR} - fonts, button prompts and the RPF key,
 *       copied out of the APK on the first launch of each install.</li>
 *   <li>{@code args.txt} next to {@code LibertyRecomp/} - one cvar per line,
 *       passed to SDL_main, so settings change without a rebuild.</li>
 *   <li>{@code driver.txt} beside it - "system" to bypass the bundled Turnip
 *       Vulkan driver.</li>
 * </ul>
 */
public class LibertyActivity extends SDLActivity {

    private static final String TAG = "LibertyRecomp";
    private static final String RESOURCES = "Resources";
    private static final String RESOURCES_STAMP = ".stamp";
    private static final String DEFAULT_ARGS = "default_args.txt";
    static final String OVERLAY_FILE = "overlay.txt";
    static final String GRAPHICS_FILE = "graphics.txt";

    private String[] mArguments = new String[0];
    /** "--android_surface=WxH" in args.txt: fixed swapchain size, scaled to the panel by the display hardware. */
    private static final String SURFACE_ARGUMENT = "--android_surface=";
    private int mSurfaceWidth;
    private int mSurfaceHeight;
    private String mDriverMode = DriverBridge.defaultMode();
    /** driver.txt as found, for the launch report. */
    private String mDriverText;
    private File mDataRoot;
    private static final String LAUNCH_REPORT = "last_launch.txt";
    /** Intent extra from the launcher's "Play (Debug Log)". */
    static final String EXTRA_DEBUG_LOG = "debug_log";
    /** Launcher "Performance test settings": no sun shadow map. */
    static final String EXTRA_SHADOWS_OFF = "perf_shadows_off";
    /** Launcher "Performance test settings": fixed 0.5 draw distance. */
    static final String EXTRA_HALF_DRAW_DISTANCE = "perf_half_draw_distance";
    /** Launcher "Performance test settings": GPU held at its highest clock. */
    static final String EXTRA_GPU_TURBO = "perf_gpu_turbo";
    /** Launcher "Performance test settings": reflection intervals in frames. */
    static final String EXTRA_WATER_REFLECTION_INTERVAL = "perf_water_reflection_interval";
    static final String EXTRA_OTHER_REFLECTION_INTERVAL = "perf_other_reflection_interval";
    private boolean mDebugLog;
    private File mDebugLogFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        prepareEnvironment();
        super.onCreate(savedInstanceState);
        // A controller-driven game sends no touch events for minutes at a time.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /**
     * Picks the Vulkan driver before SDL loads libmain.so; see DriverBridge.
     * When no driver can run the game, the error thrown here is shown by
     * SDLActivity in a dialog instead of the game closing without a word.
     */
    @Override
    public void loadLibraries() {
        String status = DriverBridge.initialize(this, mDriverMode);
        String reason = DriverBridge.unusableReason(status);
        writeLaunchReport(status, reason);
        if (reason != null) {
            throw new UnsatisfiedLinkError(reason
                    + (DriverBridge.isMaliBuild()
                            ? "\n\nThis is the Mali build: it needs a GPU driver with Vulkan 1.2 or newer."
                            : "\n\nSupported GPUs: Qualcomm Adreno 6xx and 7xx.")
                    + "\nA report was saved to Android/data/" + getPackageName()
                    + "/files/" + LAUNCH_REPORT + ".");
        }
        super.loadLibraries();
    }

    /**
     * Device, GPU and driver facts of this launch in the data folder, so a
     * player without adb can attach them to a bug report.
     */
    @SuppressWarnings("deprecation")
    private void writeLaunchReport(String driverStatus, String problem) {
        if (mDataRoot == null) return;
        StringBuilder report = new StringBuilder();
        String version = "?";
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = info.versionName + " (" + info.versionCode + ")";
        } catch (Exception ignored) {
        }
        report.append("Liberty Recompiled for Android ").append(version).append('\n');
        report.append("time: ").append(new java.util.Date()).append('\n');
        report.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append(", board ").append(Build.BOARD)
                .append(", hardware ").append(Build.HARDWARE).append(")\n");
        if (Build.VERSION.SDK_INT >= 31) {
            report.append("soc: ").append(Build.SOC_MANUFACTURER).append(' ')
                    .append(Build.SOC_MODEL).append('\n');
        }
        report.append("android: ").append(Build.VERSION.RELEASE).append(" (API ")
                .append(Build.VERSION.SDK_INT).append(")\n");
        report.append("abis: ").append(String.join(", ", Build.SUPPORTED_ABIS)).append('\n');
        try {
            report.append("page size: ")
                    .append(Os.sysconf(android.system.OsConstants._SC_PAGESIZE)).append('\n');
        } catch (Exception ignored) {
        }
        String kgsl = readFirstLine(new File("/sys/class/kgsl/kgsl-3d0/gpu_model"));
        report.append("kgsl gpu: ").append(kgsl != null ? kgsl : "(not readable or not Adreno)")
                .append('\n');
        report.append("build: ").append(BuildConfig.GPU_FAMILY)
                .append(" (default driver ").append(DriverBridge.defaultMode()).append(")\n");
        report.append("driver.txt: ")
                .append(mDriverText == null ? "(none)" : "\"" + mDriverText + "\"")
                .append(" -> ").append(mDriverMode).append('\n');
        report.append("driver status: ").append(driverStatus).append('\n');
        if (mDebugLog) {
            report.append("debug log: ")
                    .append(mDebugLogFile != null ? mDebugLogFile.getAbsolutePath() : "could not start")
                    .append('\n');
        }
        report.append("result: ").append(problem == null ? "driver usable, starting the game"
                : "cannot start: " + problem.replace('\n', ' ')).append('\n');
        try (OutputStream out = new FileOutputStream(new File(mDataRoot, LAUNCH_REPORT))) {
            out.write(report.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "writing the launch report failed", e);
        }
        Log.i(TAG, "launch report:\n" + report);
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        if (mSurfaceWidth > 0 && mSurfaceHeight > 0) {
            surface.getHolder().setFixedSize(mSurfaceWidth, mSurfaceHeight);
            Log.i(TAG, "fixed surface " + mSurfaceWidth + "x" + mSurfaceHeight);
        }
        return surface;
    }

    @Override
    protected String[] getArguments() {
        return mArguments;
    }

    /**
     * SDL re-requests the orientation when it creates its window. It reads the
     * SDL_ORIENTATIONS hint from application-level SDL_ENV.* metadata only, so
     * the manifest's activity-level entry never reached it: with no hint and a
     * resizable window it asked for FULL_USER, and phones held upright turned
     * the game to portrait (seen on a moto edge 30 fusion and a Samsung).
     * The game is landscape only, in either direction.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        super.setOrientationBis(w, h, resizable, "LandscapeLeft LandscapeRight");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    // The launcher's performance test settings replace these cvars' lines in
    // args.txt for this run (the file itself is left as it is).
    private void applyPerformanceTestArguments(List<String> arguments) {
        Intent intent = getIntent();
        if (intent == null || !intent.hasExtra(EXTRA_SHADOWS_OFF)) return;
        boolean shadowsOff = intent.getBooleanExtra(EXTRA_SHADOWS_OFF, false);
        // 0 = off.
        int water = Math.max(0, Math.min(intent.getIntExtra(EXTRA_WATER_REFLECTION_INTERVAL, 1), 3));
        int environment =
                Math.max(0, Math.min(intent.getIntExtra(EXTRA_OTHER_REFLECTION_INTERVAL, 1), 3));
        arguments.removeIf(a -> a.startsWith("--gta4_native_shadow_interval")
                || a.startsWith("--gta4_native_water_reflection_interval")
                || a.startsWith("--gta4_native_environment_reflection_interval"));
        arguments.add("--gta4_native_shadow_interval=" + (shadowsOff ? 0 : 1));
        arguments.removeIf(a -> a.startsWith("--android_gpu_turbo"));
        arguments.add("--android_gpu_turbo=" + intent.getBooleanExtra(EXTRA_GPU_TURBO, false));
        if (intent.getBooleanExtra(EXTRA_HALF_DRAW_DISTANCE, false)) {
            // Fixed: the dynamic controller would lower it further.
            arguments.removeIf(a -> a.startsWith("--gta4_draw_distance_scale")
                    || a.startsWith("--gta4_dynamic_draw_distance="));
            arguments.add("--gta4_draw_distance_scale=0.5");
            arguments.add("--gta4_dynamic_draw_distance=false");
        }
        arguments.add("--gta4_native_water_reflection_interval=" + water);
        arguments.add("--gta4_native_environment_reflection_interval=" + environment);
    }

    private void prepareEnvironment() {
        File external = getExternalFilesDir(null);
        File dataRoot = external != null ? external : getFilesDir();
        mDataRoot = dataRoot;
        File resources = new File(getFilesDir(), RESOURCES);
        try {
            syncResources(this);
        } catch (IOException e) {
            Log.e(TAG, "copying bundled resources failed", e);
        }
        setenv("XDG_DATA_HOME", dataRoot.getAbsolutePath());
        setenv("HOME", dataRoot.getAbsolutePath());
        setenv("REX_RESOURCES_DIR", resources.getAbsolutePath());
        // Turnip's GMEM (tiled) mode loses ~15% on this renderer's many small
        // passes and resolves; direct system-memory rendering is faster on the
        // Adreno 650. TU_DEBUG means nothing to any other driver, so the Mali
        // build leaves it unset. env.txt may override it either way.
        if (!DriverBridge.isMaliBuild()) {
            setenv("TU_DEBUG", "sysmem");
        }
        applyEnvironmentFile(new File(dataRoot, "env.txt"));
        File argsFile = new File(dataRoot, "args.txt");
        if (!argsFile.isFile()) {
            // First launch: start from the tuned handheld profile bundled in
            // the APK; the copy stays editable next to LibertyRecomp/.
            try {
                copyAssetTree(getAssets(), DEFAULT_ARGS, argsFile);
            } catch (IOException e) {
                Log.e(TAG, "writing default args failed", e);
            }
        }
        migrateArguments(argsFile);
        List<String> arguments = new ArrayList<>(Arrays.asList(readArguments(argsFile)));
        // Performance overlay lines chosen on the launcher (written by
        // LauncherActivity); later arguments win, so these override args.txt.
        // Graphics settings chosen on the launcher; like the overlay lines they
        // come after args.txt and so override it.
        arguments.addAll(Arrays.asList(readArguments(new File(dataRoot, GRAPHICS_FILE))));
        arguments.addAll(Arrays.asList(readArguments(new File(dataRoot, OVERLAY_FILE))));
        // The runtime's own log (warnings and errors at the default level) goes
        // to logcat and Liberty Recompiled/logs, and ends up in crash reports.
        // Both switches together: categories without --diagnostics=true make
        // the runtime refuse to start.
        boolean diagnosticsSet = false;
        for (String argument : arguments) {
            if (argument.startsWith("--diagnostics")) diagnosticsSet = true;
        }
        // "Play (Debug Log)" from the launcher: the runtime logs at info level
        // and the whole process log is recorded into files/debug_logs/.
        applyPerformanceTestArguments(arguments);
        mDebugLog = getIntent() != null && getIntent().getBooleanExtra(EXTRA_DEBUG_LOG, false);
        if (mDebugLog) {
            arguments.removeIf(a -> a.startsWith("--diagnostics") || a.startsWith("--log_level"));
            diagnosticsSet = false;
            arguments.add("--log_level=info");
            mDebugLogFile = DebugLog.start(dataRoot);
        }
        if (!diagnosticsSet) {
            arguments.add("--diagnostics=true");
            arguments.add("--diagnostics_categories=logging");
        }
        addInstallSources(new File(dataRoot, "install"), arguments);
        for (int i = arguments.size() - 1; i >= 0; --i) {
            String argument = arguments.get(i);
            if (!argument.startsWith(SURFACE_ARGUMENT)) continue;
            arguments.remove(i);  // Java-side setting; the runtime does not know it.
            String[] size = argument.substring(SURFACE_ARGUMENT.length()).split("x");
            try {
                if (size.length == 2) {
                    mSurfaceWidth = Integer.parseInt(size[0].trim());
                    mSurfaceHeight = Integer.parseInt(size[1].trim());
                }
            } catch (NumberFormatException e) {
                Log.w(TAG, "bad " + argument);
            }
        }
        mArguments = withoutDuplicateOptions(arguments).toArray(new String[0]);
        String driver = readFirstLine(new File(dataRoot, "driver.txt"));
        mDriverText = driver;
        String lower = driver == null ? "" : driver.toLowerCase(Locale.ROOT);
        if (lower.startsWith(DriverBridge.CUSTOM_PREFIX)) {
            mDriverMode = DriverBridge.CUSTOM_PREFIX
                    + driver.substring(DriverBridge.CUSTOM_PREFIX.length()).trim();
        } else if (lower.equals(DriverBridge.SYSTEM)) {
            mDriverMode = DriverBridge.SYSTEM;
        } else if (lower.equals(DriverBridge.TURNIP)) {
            mDriverMode = DriverBridge.TURNIP;
        } else if (!lower.isEmpty()) {
            Log.w(TAG, "unknown driver.txt value \"" + driver + "\", using "
                    + DriverBridge.defaultMode());
        }
        Log.i(TAG, "data=" + dataRoot + " resources=" + resources
                + " args=" + mArguments.length + " driver=" + mDriverMode);
    }

    /**
     * Optional NAME=value lines (e.g. Turnip's TU_DEBUG) exported before the
     * Vulkan driver loads; '#' starts a comment.
     */
    private static void applyEnvironmentFile(File file) {
        if (!file.isFile()) {
            return;
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = cleanLine(line);
                int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals <= 0) {
                    continue;
                }
                String name = line.substring(0, equals).trim();
                String value = line.substring(equals + 1).trim();
                setenv(name, value);
                Log.i(TAG, "env " + name + "=" + value);
            }
        } catch (IOException e) {
            Log.e(TAG, "reading " + file + " failed", e);
        }
    }

    /**
     * The SDL file picker returns content:// URIs the installer cannot read, so
     * sources are taken from files/install/ (writable over USB without any
     * storage permission): the first *.iso is the disc, any other file the
     * title update. Explicit --install_* lines in args.txt win.
     */
    private static void addInstallSources(File dir, List<String> arguments) {
        if (!dir.isDirectory() && !dir.mkdirs()) {
            return;
        }
        for (String argument : arguments) {
            if (argument.startsWith("--install_")) {
                return;
            }
        }
        File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        File disc = null;
        File update = null;
        for (File file : files) {
            if (!file.isFile() || file.getName().startsWith(".")) {
                continue;
            }
            if (file.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".iso")) {
                if (disc == null) disc = file;
            } else if (update == null || file.length() > update.length()) {
                update = file;
            }
        }
        if (disc != null) {
            arguments.add("--install_game_source=" + disc.getAbsolutePath());
            Log.i(TAG, "install disc " + disc);
        }
        if (update != null) {
            arguments.add("--install_update_source=" + update.getAbsolutePath());
            Log.i(TAG, "install title update " + update);
        }
    }

    private static void setenv(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv " + name + " failed", e);
        }
    }

    /**
     * Re-copies the bundled resources into files/Resources whenever the
     * installed APK changes, and returns that folder. Shared with the
     * launcher's installer, which needs the RPF key from it.
     */
    static synchronized File syncResources(Context context) throws IOException {
        File target = new File(context.getFilesDir(), RESOURCES);
        String stamp;
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            stamp = info.versionCode + ":" + info.lastUpdateTime;
        } catch (Exception e) {
            stamp = "unknown";
        }
        File stampFile = new File(target, RESOURCES_STAMP);
        if (stamp.equals(readFirstLine(stampFile))) {
            return target;
        }
        deleteRecursively(target);
        copyAssetTree(context.getAssets(), RESOURCES, target);
        try (OutputStream out = new FileOutputStream(stampFile)) {
            out.write(stamp.getBytes(StandardCharsets.UTF_8));
        }
        Log.i(TAG, "bundled resources copied to " + target);
        return target;
    }

    private static void copyAssetTree(AssetManager assets, String path, File target)
            throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            try (InputStream in = assets.open(path);
                 OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            }
            return;
        }
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("cannot create " + target);
        }
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(target, child));
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        if (file.exists() && !file.delete()) {
            Log.w(TAG, "cannot delete " + file);
        }
    }

    private static String readFirstLine(File file) {
        if (!file.isFile()) {
            return null;
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line = reader.readLine();
            return line == null ? null : cleanLine(line);
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * Trims a line of a settings file, including the byte-order mark some
     * editors (Windows Notepad among them) put at the start of a file.
     */
    private static String cleanLine(String line) {
        return line.replace("\uFEFF", "").trim();
    }

    /**
     * Earlier defaults replaced in args.txt files copied by older versions.
     * Only the exact old line is rewritten; edited values are left alone.
     */
    private static final String[][] ARGUMENT_MIGRATIONS = {
            // 0.5.7.9: dynamic draw distance floor 0.80 (0.75 broke dynamic
            // shadows; 0.85 was an interim default of test builds).
            {"--gta4_dynamic_draw_distance_min=0.75", "--gta4_dynamic_draw_distance_min=0.80"},
            {"--gta4_dynamic_draw_distance_min=0.85", "--gta4_dynamic_draw_distance_min=0.80"},
    };

    private static void migrateArguments(File file) {
        if (!file.isFile()) return;
        try {
            List<String> lines = Files.readAllLines(file.toPath(), StandardCharsets.UTF_8);
            boolean changed = false;
            for (int i = 0; i < lines.size(); ++i) {
                for (String[] migration : ARGUMENT_MIGRATIONS) {
                    if (cleanLine(lines.get(i)).equals(migration[0])) {
                        lines.set(i, migration[1]);
                        changed = true;
                    }
                }
            }
            if (changed) {
                Files.write(file.toPath(), lines, StandardCharsets.UTF_8);
                Log.i(TAG, "updated old defaults in " + file);
            }
        } catch (IOException e) {
            Log.w(TAG, "cannot update " + file, e);
        }
    }

    /** One argument per line; blank lines and lines starting with '#' are skipped. */

    /**
     * The runtime's command-line parser (CLI11) rejects an option given twice,
     * and on that error it ignores the whole command line. graphics.txt and
     * overlay.txt repeat options that args.txt already sets, so keep only the
     * last occurrence of each option: launcher choices override args.txt.
     * "--no-name" counts as the same option as "--name".
     */
    static List<String> withoutDuplicateOptions(List<String> arguments) {
        java.util.Map<String, Integer> lastIndex = new java.util.HashMap<>();
        for (int i = 0; i < arguments.size(); ++i) {
            String key = optionKey(arguments.get(i));
            if (key != null) lastIndex.put(key, i);
        }
        List<String> result = new ArrayList<>(arguments.size());
        for (int i = 0; i < arguments.size(); ++i) {
            String argument = arguments.get(i);
            String key = optionKey(argument);
            if (key == null || lastIndex.get(key) == i) {
                result.add(argument);
            } else {
                Log.i(TAG, "dropping overridden argument " + argument);
            }
        }
        return result;
    }

    private static String optionKey(String argument) {
        if (argument == null || !argument.startsWith("--")) return null;
        String name = argument.substring(2);
        int equals = name.indexOf('=');
        if (equals >= 0) name = name.substring(0, equals);
        if (name.startsWith("no-")) name = name.substring(3);
        return name.trim().toLowerCase(Locale.ROOT);
    }

    private static String[] readArguments(File file) {
        List<String> arguments = new ArrayList<>();
        if (file.isFile()) {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    new FileInputStream(file), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = cleanLine(line);
                    if (!line.isEmpty() && !line.startsWith("#")) {
                        arguments.add(line);
                    }
                }
            } catch (IOException e) {
                Log.w(TAG, "cannot read " + file, e);
            }
        }
        return arguments.toArray(new String[0]);
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
