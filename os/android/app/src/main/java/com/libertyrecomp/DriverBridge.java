package com.libertyrecomp;

import android.content.Context;
import android.os.Process;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Chooses the Vulkan driver once per process, before SDL loads libmain.so.
 *
 * <p>The runtime opens Vulkan with a bare {@code dlopen("libvulkan.so")}. The
 * APK ships its own libvulkan.so (os/android/native, built on libadrenotools)
 * in nativeLibraryDir, which is searched before the system namespace, so that
 * lookup always lands on the proxy - whether or not anyone loaded it. The proxy
 * is therefore always loaded and always initialized: "turnip" hands it the
 * bundled Mesa Turnip build, "system" forwards to /system/lib64/libvulkan.so.</p>
 *
 * <p>Turnip is the default: the stock Adreno 650 driver exposes a Vulkan 1.1
 * device and the native renderer needs 1.2. {@code driver.txt} next to
 * {@code args.txt} selects "system" instead.</p>
 *
 * <p>Ported from andrewnakas/skate3-android (MIT), which took it from
 * AlanConstantino/skate3-pocket; native/PROVENANCE.md has the details.</p>
 */
final class DriverBridge {

    static final String TURNIP = "turnip";
    static final String SYSTEM = "system";
    /** "custom:NAME" loads an AdrenoTools package unpacked in drivers/NAME next to driver.txt. */
    static final String CUSTOM_PREFIX = "custom:";

    private static final String TAG = "LibertyDriver";
    private static final String BUNDLE_ASSET = "drivers/turnip-r6.zip";
    private static final String LIBRARY = "libvulkan_freedreno.so";
    private static final String META = "meta.json";
    private static final String ZIP_HASH =
            "5fa2305eb9eff64d9347a670e6ca5ef7c518f60e9e782a4db7ee5588c9852d81";
    private static final String LIB_HASH =
            "4fd2fd30311af301ae5564139bc24cc727390d4dc3c90a20192c25ec91287c59";
    private static final String META_HASH =
            "b6dae160802aec1ab48a0953fd9b09d76465a282c4a1d134645218af2a62d9d3";

    private static boolean sInitialized;
    /** What the first initialization returned; the proxy is set up once per process. */
    private static String sStatus;

    private DriverBridge() {}

    /**
     * The driver used while driver.txt does not exist: the bundled Turnip in
     * the Adreno build, the device's own driver in the Mali build (Turnip is
     * Adreno-only and cannot load on any other GPU).
     */
    static String defaultMode() {
        return SYSTEM.equals(BuildConfig.DEFAULT_DRIVER) ? SYSTEM : TURNIP;
    }

    /** True in the APK built with -PlibertyGpu=mali. */
    static boolean isMaliBuild() {
        return "mali".equals(BuildConfig.GPU_FAMILY);
    }

    static native String nativeInit(String nativeLibraryDir, String internalDriverDir,
                                    String driverFilename, boolean custom,
                                    boolean allowSystemFallback);

    /** VK_API_VERSION_1_2: the native renderer's minimum. */
    private static final long VULKAN_1_2 = (1L << 22) | (2L << 12);

    /** Loads and initializes the proxy. Returns the native status JSON. */
    static synchronized String initialize(Context context, String mode) {
        if (sInitialized) {
            return sStatus;
        }
        String nativeDir = context.getApplicationInfo().nativeLibraryDir;
        boolean custom = TURNIP.equals(mode);
        File driverDir = new File(context.getFilesDir(), "drivers");
        String library = LIBRARY;
        if (mode != null && mode.startsWith(CUSTOM_PREFIX)) {
            try {
                String name = mode.substring(CUSTOM_PREFIX.length()).trim();
                driverDir = prepareCustom(context, name);
                library = customLibrary(driverDir);
                custom = true;
                Log.i(TAG, "custom driver " + name + " library=" + library);
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "custom driver unusable, using the bundled Turnip", e);
                mode = TURNIP;
                custom = true;
            }
        }
        if (custom && LIBRARY.equals(library)) {
            try {
                driverDir = prepareBundle(context);
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Turnip bundle unusable, using the system driver", e);
                custom = false;
            }
        }
        if (!driverDir.isDirectory() && !driverDir.mkdirs()) {
            Log.w(TAG, "cannot create " + driverDir);
        }
        System.load(new File(nativeDir, "libvulkan.so").getAbsolutePath());
        // A custom driver that cannot run this GPU falls back to the system
        // driver; an explicit "system" choice has nothing to fall back to.
        String status = nativeInit(nativeDir, driverDir.getAbsolutePath(),
                custom ? library : "", custom, custom);
        sInitialized = true;
        sStatus = status;
        boolean ok = false;
        try {
            ok = new JSONObject(status).optBoolean("ok", false);
        } catch (JSONException ignored) {
        }
        if (ok) {
            Log.i(TAG, "driver ready: " + status);
        } else {
            Log.e(TAG, "driver failed: " + status);
        }
        return status;
    }

    /** Renderer-required Vulkan features, in the order the report names them. */
    private static final String[] REQUIRED_FEATURES = {
            "shaderInt64", "depthClamp", "sampleRateShading",
            "independentBlend", "runtimeDescriptorArray", "descriptorBindingPartiallyBound",
            "bufferDeviceAddress",
    };

    /**
     * Names of required features the driver explicitly reports as absent
     * (value 0). A feature that could not be queried (-1) or a status without
     * the block at all counts as present: the renderer makes the final check.
     */
    private static String missingFeatures(JSONObject features) {
        if (features == null) return "";
        StringBuilder missing = new StringBuilder();
        for (String feature : REQUIRED_FEATURES) {
            if (features.optInt(feature, -1) == 0) {
                if (missing.length() > 0) missing.append(", ");
                missing.append(feature);
            }
        }
        return missing.toString();
    }

    /**
     * Null when the initialized driver can run the game, otherwise a message
     * for the player saying what is missing.
     */
    static String unusableReason(String status) {
        JSONObject json;
        try {
            json = new JSONObject(status);
        } catch (JSONException e) {
            return "The Vulkan driver could not be initialized (unreadable status).";
        }
        if (!json.optBoolean("ok", false)) {
            return "No usable Vulkan driver was found on this device.\n\n"
                    + json.optString("error", "unknown error");
        }
        org.json.JSONArray devices = json.optJSONArray("devices");
        long best = 0;
        String name = "unknown GPU";
        JSONObject bestFeatures = null;
        for (int i = 0; devices != null && i < devices.length(); ++i) {
            JSONObject device = devices.optJSONObject(i);
            if (device == null) continue;
            long api = device.optLong("api_version", 0);
            if (api >= best) {
                best = api;
                name = device.optString("name", name);
                bestFeatures = device.optJSONObject("features");
            }
        }
        if (best >= VULKAN_1_2) {
            String missing = missingFeatures(bestFeatures);
            if (missing.isEmpty()) return null;
            return "This device's GPU (" + name + ") has Vulkan 1.2 but its driver lacks "
                    + "features the game's renderer requires: " + missing + "."
                    + "\n\nA newer system update may add them. Please attach the launch report "
                    + "when reporting this.";
        }
        String fallbackFrom = json.optString("fallback_from");
        return "This device's GPU (" + name + ") only offers Vulkan " + (best >> 22) + "."
                + ((best >> 12) & 0x3FF) + " through its own driver, and the game needs Vulkan 1.2."
                + (fallbackFrom.isEmpty()
                        ? (isMaliBuild()
                                ? "\n\nThe phone's system GPU driver is too old for the game. Updating the "
                                        + "system software may bring a newer driver."
                                : "\n\nIf driver.txt says \"system\", delete it to use the bundled Turnip driver.")
                        : "\n\nThe selected custom driver (the bundled Turnip unless driver.txt names "
                                + "another) could not be used either: " + fallbackFrom);
    }

    /** Copies an unpacked package from external storage into exec-capable private storage. */
    private static File prepareCustom(Context context, String name) throws IOException {
        if (name.isEmpty() || name.contains("/") || name.contains("..")) {
            throw new IOException("bad custom driver name: " + name);
        }
        File source = new File(context.getExternalFilesDir(null), "drivers/" + name);
        File[] files = source.listFiles();
        if (files == null || files.length == 0) {
            throw new IOException("no custom driver files in " + source);
        }
        File destination = new File(new File(context.getFilesDir(), "drivers"), "custom-" + name);
        deleteRecursively(destination);
        if (!destination.mkdirs()) {
            throw new IOException("cannot create " + destination);
        }
        for (File file : files) {
            if (!file.isFile()) continue;
            try (InputStream in = new java.io.FileInputStream(file);
                 OutputStream out = new FileOutputStream(new File(destination, file.getName()))) {
                copy(in, out);
            }
        }
        return destination;
    }

    /** meta.json libraryName, else the only .so in the package. */
    private static String customLibrary(File dir) throws IOException {
        File meta = new File(dir, META);
        if (meta.isFile()) {
            try (InputStream in = new java.io.FileInputStream(meta)) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                copy(in, bytes);
                String name = new JSONObject(bytes.toString("UTF-8")).optString("libraryName", "");
                if (!name.isEmpty() && new File(dir, name).isFile()) return name;
            } catch (JSONException ignored) {
            }
        }
        File[] libraries = dir.listFiles((d, n) -> n.endsWith(".so"));
        if (libraries == null || libraries.length != 1) {
            throw new IOException("cannot pick the driver library in " + dir);
        }
        return libraries[0].getName();
    }

    /** Extracts the hash-pinned two-file Turnip package into private storage. */
    private static File prepareBundle(Context context) throws IOException {
        File parent = new File(context.getFilesDir(), "drivers");
        File destination = new File(parent, "r6-" + LIB_HASH.substring(0, 12));
        if (valid(new File(destination, LIBRARY), LIB_HASH)
                && valid(new File(destination, META), META_HASH)) {
            return destination;
        }
        if (!parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        File archive = File.createTempFile("turnip-r6-", ".zip", context.getCacheDir());
        File staging = new File(parent, ".r6-staging-" + Process.myPid());
        try {
            try (InputStream in = context.getAssets().open(BUNDLE_ASSET);
                 OutputStream out = new FileOutputStream(archive)) {
                copy(in, out);
            }
            if (!ZIP_HASH.equals(digest(archive))) {
                throw new IOException("bundled Turnip archive failed its integrity check");
            }
            deleteRecursively(staging);
            if (!staging.mkdirs()) {
                throw new IOException("cannot create " + staging);
            }
            try (ZipFile zip = new ZipFile(archive)) {
                Set<String> names = new HashSet<>();
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    names.add(e.nextElement().getName());
                }
                Set<String> expected = new HashSet<>();
                expected.add(META);
                expected.add(LIBRARY);
                if (!names.equals(expected)) {
                    throw new IOException("unexpected Turnip archive members: " + names);
                }
                extract(zip, META, new File(staging, META), META_HASH);
                extract(zip, LIBRARY, new File(staging, LIBRARY), LIB_HASH);
            }
            deleteRecursively(destination);
            if (!staging.renameTo(destination)) {
                throw new IOException("cannot activate " + destination);
            }
            return destination;
        } finally {
            if (!archive.delete()) {
                archive.deleteOnExit();
            }
            deleteRecursively(staging);
        }
    }

    private static void extract(ZipFile zip, String name, File target, String hash)
            throws IOException {
        try (InputStream in = zip.getInputStream(zip.getEntry(name));
             OutputStream out = new FileOutputStream(target)) {
            copy(in, out);
        }
        if (!valid(target, hash)) {
            throw new IOException(name + " failed its integrity check");
        }
        if (!target.setReadOnly()) {
            throw new IOException("cannot protect " + target);
        }
    }

    private static boolean valid(File file, String hash) {
        try {
            return file.isFile() && hash.equals(digest(file));
        } catch (IOException e) {
            return false;
        }
    }

    private static String digest(File file) throws IOException {
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                sha.update(buffer, 0, read);
            }
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : sha.digest()) {
            hex.append(String.format("%02x", b & 0xff));
        }
        return hex.toString();
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int read;
        while ((read = in.read(buffer)) > 0) {
            out.write(buffer, 0, read);
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        if (file.exists()) {
            file.setWritable(true);
            file.delete();
        }
    }
}
