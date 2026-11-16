package com.libertyrecomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Map;
import java.util.TreeMap;

/**
 * SHA-256 of every custom driver package the player has accepted, kept in the
 * app's private storage.
 *
 * <p>Custom drivers live in the shared app folder ({@code Android/data/.../files/drivers/NAME}),
 * which a file manager, a PC over USB and, on Android 10, other apps with the
 * storage permission can write. The driver is native code loaded into the
 * game process, so a package that changed after the player accepted it must
 * not load. The launcher records the hashes when it imports a zip, or after
 * the player confirms a package unpacked by hand; the game process hashes the
 * private copy it is about to load and refuses it unless the hashes match.</p>
 *
 * <p>Plain Java on purpose (no Android classes), so it runs in host unit tests.
 * The store is a text file: one {@code NAME\tFILE\tSHA256} line per file.</p>
 */
final class DriverTrust {

    static final String STORE_NAME = "driver_trust.tsv";

    private final File mStore;

    DriverTrust(File store) {
        mStore = store;
    }

    /** The store in the app's private files directory. */
    static DriverTrust inPrivateDir(File filesDir) {
        return new DriverTrust(new File(filesDir, STORE_NAME));
    }

    /**
     * A file name with no directory part: what a package's {@code meta.json}
     * may name as its {@code libraryName}, and what a package may contain.
     */
    static boolean isPlainFileName(String name) {
        return name != null && !name.isEmpty() && name.indexOf('/') < 0 && name.indexOf('\\') < 0
                && !name.equals(".") && !name.equals("..") && name.indexOf('\0') < 0
                && name.indexOf('\t') < 0 && name.indexOf('\n') < 0 && name.indexOf('\r') < 0;
    }

    /** File name to SHA-256 of the regular files directly in {@code dir}. */
    static Map<String, String> hashDirectory(File dir) throws IOException {
        File[] files = dir.listFiles();
        if (files == null) throw new IOException("cannot list " + dir);
        Map<String, String> hashes = new TreeMap<>();
        for (File file : files) {
            if (file.isFile()) hashes.put(file.getName(), sha256(file));
        }
        return hashes;
    }

    static String sha256(File file) throws IOException {
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) sha.update(buffer, 0, read);
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte b : sha.digest()) hex.append(String.format("%02x", b & 0xff));
        return hex.toString();
    }

    /** Whether {@code files} is exactly the package the player accepted as {@code name}. */
    synchronized boolean matches(String name, Map<String, String> files) throws IOException {
        Map<String, String> trusted = read().get(name);
        return trusted != null && !files.isEmpty() && trusted.equals(new TreeMap<>(files));
    }

    synchronized boolean isTrusted(String name) throws IOException {
        return read().containsKey(name);
    }

    /** Records {@code files} as the accepted contents of package {@code name}. */
    synchronized void trust(String name, Map<String, String> files) throws IOException {
        if (!isPlainFileName(name)) throw new IOException("bad driver name: " + name);
        for (String file : files.keySet()) {
            if (!isPlainFileName(file)) throw new IOException("bad driver file name: " + file);
        }
        Map<String, Map<String, String>> all = read();
        all.put(name, new TreeMap<>(files));
        write(all);
    }

    synchronized void forget(String name) throws IOException {
        Map<String, Map<String, String>> all = read();
        if (all.remove(name) != null) write(all);
    }

    private Map<String, Map<String, String>> read() throws IOException {
        Map<String, Map<String, String>> all = new TreeMap<>();
        if (!mStore.isFile()) return all;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(mStore), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] fields = line.split("\t", -1);
                // A malformed line trusts nothing; the rest of the store still counts.
                if (fields.length != 3 || !isPlainFileName(fields[0]) || !isPlainFileName(fields[1])
                        || !fields[2].matches("[0-9a-f]{64}")) {
                    continue;
                }
                all.computeIfAbsent(fields[0], k -> new TreeMap<>()).put(fields[1], fields[2]);
            }
        }
        return all;
    }

    /** Replaces the store atomically: a crash mid-write leaves the old one. */
    private void write(Map<String, Map<String, String>> all) throws IOException {
        File parent = mStore.getAbsoluteFile().getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        File temporary = new File(parent, mStore.getName() + ".tmp");
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, Map<String, String>> pkg : all.entrySet()) {
            for (Map.Entry<String, String> file : pkg.getValue().entrySet()) {
                text.append(pkg.getKey()).append('\t').append(file.getKey()).append('\t')
                        .append(file.getValue()).append('\n');
            }
        }
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(text.toString().getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        if (!temporary.renameTo(mStore)) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            throw new IOException("cannot replace " + mStore);
        }
    }
}
