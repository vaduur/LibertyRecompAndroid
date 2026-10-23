package com.libertyrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Crash reports for players without adb.
 *
 * <p>When the game process ({@code :game}) dies abnormally, Android keeps the
 * reason (ApplicationExitInfo, Android 11+), and for a native crash the
 * tombstone: signal, fault address, the crashing thread's backtrace and the
 * process's last log lines. The launcher turns that into
 * {@code files/crash_reports/crash-DATE.txt} (plus the raw tombstone) and
 * shows it, so the report can be attached to an issue or shared directly.</p>
 */
final class CrashReports {

    private static final String TAG = "LibertyCrash";
    private static final String PREFS = "crash_reports";
    private static final String SEEN = "seen_until";
    private static final String DISMISSED = "dismissed";
    private static final int KEEP = 10;

    /** A saved report. */
    static final class Report {
        final File file;
        final String headline;
        final String text;

        Report(File file, String headline, String text) {
            this.file = file;
            this.headline = headline;
            this.text = text;
        }
    }

    private CrashReports() {}

    static File directory(File dataRoot) {
        return new File(dataRoot, "crash_reports");
    }

    /**
     * Saves a report for every abnormal exit of the game process since the
     * last check. Returns the newest report that has not been dismissed, or
     * null.
     */
    static Report collect(Context context, File dataRoot) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                collectNew(context, dataRoot, prefs);
            } catch (Exception e) {
                Log.w(TAG, "reading exit reasons failed", e);
            }
        }
        File[] files = directory(dataRoot).listFiles((d, n) -> n.startsWith("crash-") && n.endsWith(".txt"));
        if (files == null || files.length == 0) return null;
        Arrays.sort(files);
        File newest = files[files.length - 1];
        if (newest.getName().equals(prefs.getString(DISMISSED, ""))) return null;
        String text = read(newest);
        String headline = text;
        int start = text.indexOf("summary: ");
        if (start >= 0) {
            int end = text.indexOf('\n', start);
            headline = text.substring(start + 9, end < 0 ? text.length() : end);
        }
        return new Report(newest, headline, text);
    }

    static void dismiss(Context context, Report report) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString(DISMISSED, report.file.getName()).apply();
    }

    private static void collectNew(Context context, File dataRoot, SharedPreferences prefs)
            throws IOException {
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return;
        List<ApplicationExitInfo> exits =
                manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 16);
        long seen = prefs.getLong(SEEN, 0);
        long newest = seen;
        String game = context.getPackageName() + ":game";
        for (ApplicationExitInfo exit : exits) {
            if (exit.getTimestamp() <= seen) continue;
            newest = Math.max(newest, exit.getTimestamp());
            if (!game.equals(exit.getProcessName()) || !abnormal(exit)) continue;
            write(context, dataRoot, exit);
        }
        if (newest != seen) prefs.edit().putLong(SEEN, newest).apply();
        prune(directory(dataRoot));
    }

    /** Crashes, ANRs, failed starts, low-memory kills and non-zero exits; not our own kills. */
    private static boolean abnormal(ApplicationExitInfo exit) {
        switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_CRASH:
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
            case ApplicationExitInfo.REASON_ANR:
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
            case ApplicationExitInfo.REASON_LOW_MEMORY:
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                return true;
            case ApplicationExitInfo.REASON_EXIT_SELF:
                // Status 4 is the game saying this device cannot run it. The
                // reason is on the launcher's own screen, not in a crash
                // report: nothing crashed.
                return exit.getStatus() != 0 && exit.getStatus() != 4;
            case ApplicationExitInfo.REASON_SIGNALED:
                // The launcher ends a leftover game process with SIGKILL before Play.
                return exit.getStatus() != 9;
            default:
                return false;
        }
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case ApplicationExitInfo.REASON_ANR: return "not responding (ANR)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "failed to start";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "killed by the system: low memory";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "killed by the system: excessive resource use";
            case ApplicationExitInfo.REASON_EXIT_SELF: return "the game exited with an error";
            case ApplicationExitInfo.REASON_SIGNALED: return "killed by a signal";
            default: return "exit reason " + reason;
        }
    }

    private static void write(Context context, File dataRoot, ApplicationExitInfo exit)
            throws IOException {
        File dir = directory(dataRoot);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT)
                .format(new Date(exit.getTimestamp()));
        byte[] trace = null;
        try (InputStream in = exit.getTraceInputStream()) {
            if (in != null) trace = readAll(in, 16 * 1024 * 1024);
        } catch (Exception e) {
            Log.w(TAG, "no trace for " + stamp, e);
        }

        StringBuilder text = new StringBuilder();
        String summary = reasonName(exit.getReason());
        Tombstone tombstone = null;
        if (trace != null && exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                && !looksLikeText(trace)) {
            try {
                tombstone = Tombstone.parse(trace);
            } catch (RuntimeException e) {
                Log.w(TAG, "tombstone not decodable", e);
            }
        }
        if (tombstone != null && tombstone.signal != null) {
            summary = tombstone.crashSignal() + " in " + tombstone.crashSite();
        } else if (exit.getReason() == ApplicationExitInfo.REASON_EXIT_SELF
                || exit.getReason() == ApplicationExitInfo.REASON_SIGNALED) {
            summary += " (status " + exit.getStatus() + ")";
        }

        text.append("Liberty Recompiled for Android - crash report\n");
        text.append("summary: ").append(summary).append('\n');
        text.append("time: ").append(new Date(exit.getTimestamp())).append('\n');
        text.append("process: ").append(exit.getProcessName()).append(" pid ").append(exit.getPid())
                .append('\n');
        text.append("reason: ").append(reasonName(exit.getReason())).append(", status ")
                .append(exit.getStatus()).append(", importance ").append(exit.getImportance())
                .append('\n');
        if (exit.getDescription() != null) {
            text.append("description: ").append(exit.getDescription()).append('\n');
        }
        text.append("memory at exit: pss ").append(exit.getPss() / 1024).append(" MB, rss ")
                .append(exit.getRss() / 1024).append(" MB\n");
        text.append('\n');

        String launch = read(new File(dataRoot, "last_launch.txt"));
        if (!launch.isEmpty()) {
            text.append("---- last launch (device, GPU, driver)\n").append(launch).append('\n');
        }
        if (tombstone != null) {
            text.append(tombstone.format()).append('\n');
        } else if (trace != null && looksLikeText(trace)) {
            String trace_text = new String(trace, StandardCharsets.UTF_8);
            if (trace_text.length() > 60000) trace_text = trace_text.substring(0, 60000) + "\n...";
            text.append("---- trace\n").append(trace_text).append('\n');
        }
        String runtimeLog = newestRuntimeLog(dataRoot);
        if (!runtimeLog.isEmpty()) text.append("---- runtime log (end)\n").append(runtimeLog).append('\n');

        File report = new File(dir, "crash-" + stamp + ".txt");
        writeFile(report, text.toString().getBytes(StandardCharsets.UTF_8));
        if (trace != null && tombstone != null) {
            writeFile(new File(dir, "crash-" + stamp + ".tombstone.pb"), trace);
        }
        Log.i(TAG, "saved " + report + ": " + summary);
    }

    private static String newestRuntimeLog(File dataRoot) {
        File[] logs = new File(dataRoot, "Liberty Recompiled/logs").listFiles(File::isFile);
        if (logs == null || logs.length == 0) return "";
        File newest = logs[0];
        for (File log : logs) if (log.lastModified() > newest.lastModified()) newest = log;
        String text = read(newest);
        String[] lines = text.split("\n");
        int from = Math.max(0, lines.length - 80);
        StringBuilder out = new StringBuilder(newest.getName()).append('\n');
        for (int i = from; i < lines.length; ++i) out.append(lines[i]).append('\n');
        return out.toString();
    }

    private static void prune(File dir) {
        File[] files = dir.listFiles((d, n) -> n.startsWith("crash-") && n.endsWith(".txt"));
        if (files == null || files.length <= KEEP) return;
        Arrays.sort(files);
        for (int i = 0; i < files.length - KEEP; ++i) {
            String base = files[i].getName().replace(".txt", "");
            //noinspection ResultOfMethodCallIgnored
            files[i].delete();
            //noinspection ResultOfMethodCallIgnored
            new File(dir, base + ".tombstone.pb").delete();
        }
    }

    private static boolean looksLikeText(byte[] data) {
        int n = Math.min(data.length, 64);
        for (int i = 0; i < n; ++i) {
            int c = data[i] & 0xFF;
            if (c < 9 || (c > 13 && c < 32)) return false;
        }
        return true;
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[65536];
        int read;
        while ((read = in.read(buffer)) > 0 && out.size() < limit) out.write(buffer, 0, read);
        return out.toByteArray();
    }

    private static String read(File file) {
        if (!file.isFile()) return "";
        try (InputStream in = new java.io.FileInputStream(file)) {
            return new String(readAll(in, 4 * 1024 * 1024), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeFile(File file, byte[] data) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(data);
        }
    }

    // ------------------------------------------------------------- tombstone

    /**
     * The parts of an Android tombstone (system/core/debuggerd/proto/tombstone.proto)
     * a crash report needs, read with a minimal protobuf decoder.
     */
    static final class Tombstone {
        String abi = "";
        String fingerprint = "";
        int tid;
        String signal;
        String abortMessage = "";
        final List<String> causes = new ArrayList<>();
        String threadName = "";
        final List<String> frames = new ArrayList<>();
        final List<String> frameLibraries = new ArrayList<>();
        final List<String> frameFunctions = new ArrayList<>();
        final List<String> logLines = new ArrayList<>();

        static Tombstone parse(byte[] data) {
            Tombstone t = new Tombstone();
            Proto p = new Proto(data, 0, data.length);
            List<byte[]> threads = new ArrayList<>();
            while (p.more()) {
                int key = p.varint32();
                int field = key >>> 3;
                int type = key & 7;
                switch (field) {
                    case 1: t.abi = arch(p.varint32()); break;
                    case 2: t.fingerprint = p.string(); break;
                    case 6: t.tid = p.varint32(); break;
                    case 10: t.signal = signal(p.bytes()); break;
                    case 14: t.abortMessage = p.string(); break;
                    case 15: t.causes.add(cause(p.bytes())); break;
                    case 16: threads.add(p.bytes()); break;
                    case 18: t.logs(p.bytes()); break;
                    default: p.skip(type); break;
                }
            }
            for (byte[] entry : threads) {
                Proto e = new Proto(entry, 0, entry.length);
                int id = -1;
                byte[] thread = null;
                while (e.more()) {
                    int key = e.varint32();
                    if ((key >>> 3) == 1) id = e.varint32();
                    else if ((key >>> 3) == 2) thread = e.bytes();
                    else e.skip(key & 7);
                }
                if (id == t.tid && thread != null) t.thread(thread);
            }
            return t;
        }

        private static String arch(int value) {
            switch (value) {
                case 0: return "arm";
                case 1: return "arm64";
                case 2: return "x86";
                case 3: return "x86_64";
                default: return "arch " + value;
            }
        }

        private static String signal(byte[] data) {
            Proto p = new Proto(data, 0, data.length);
            int number = 0;
            String name = "", code = "";
            long fault = 0;
            boolean hasFault = false;
            while (p.more()) {
                int key = p.varint32();
                switch (key >>> 3) {
                    case 1: number = p.varint32(); break;
                    case 2: name = p.string(); break;
                    case 4: code = p.string(); break;
                    case 8: hasFault = p.varint() != 0; break;
                    case 9: fault = p.varint(); break;
                    default: p.skip(key & 7); break;
                }
            }
            String text = (name.isEmpty() ? "signal " + number : name)
                    + (code.isEmpty() ? "" : " (" + code + ")");
            if (hasFault) text += String.format(Locale.ROOT, ", fault address 0x%x", fault);
            return text;
        }

        private static String cause(byte[] data) {
            Proto p = new Proto(data, 0, data.length);
            String text = "";
            while (p.more()) {
                int key = p.varint32();
                if ((key >>> 3) == 1) text = p.string();
                else p.skip(key & 7);
            }
            return text;
        }

        private void thread(byte[] data) {
            Proto p = new Proto(data, 0, data.length);
            while (p.more()) {
                int key = p.varint32();
                switch (key >>> 3) {
                    case 2: threadName = p.string(); break;
                    case 4: frame(p.bytes()); break;
                    default: p.skip(key & 7); break;
                }
            }
        }

        private void frame(byte[] data) {
            Proto p = new Proto(data, 0, data.length);
            long relPc = 0, offset = 0;
            String function = "", file = "", buildId = "";
            while (p.more()) {
                int key = p.varint32();
                switch (key >>> 3) {
                    case 1: relPc = p.varint(); break;
                    case 4: function = p.string(); break;
                    case 5: offset = p.varint(); break;
                    case 6: file = p.string(); break;
                    case 8: buildId = p.string(); break;
                    default: p.skip(key & 7); break;
                }
            }
            String library = file.substring(file.lastIndexOf('/') + 1);
            frameLibraries.add(library);
            frameFunctions.add(function);
            frames.add(String.format(Locale.ROOT, "#%02d pc %08x  %s%s%s", frames.size(), relPc,
                    library.isEmpty() ? "?" : library,
                    function.isEmpty() ? "" : " (" + function + "+" + offset + ")",
                    buildId.isEmpty() ? "" : " [" + buildId + "]"));
        }

        private void logs(byte[] data) {
            Proto p = new Proto(data, 0, data.length);
            String buffer = "";
            while (p.more()) {
                int key = p.varint32();
                if ((key >>> 3) == 1) {
                    buffer = p.string();
                } else if ((key >>> 3) == 2) {
                    byte[] message = p.bytes();
                    Proto m = new Proto(message, 0, message.length);
                    String timestamp = "", tag = "", text = "";
                    int priority = 0;
                    while (m.more()) {
                        int k = m.varint32();
                        switch (k >>> 3) {
                            case 1: timestamp = m.string(); break;
                            case 4: priority = m.varint32(); break;
                            case 5: tag = m.string(); break;
                            case 6: text = m.string(); break;
                            default: m.skip(k & 7); break;
                        }
                    }
                    // Android log priorities: 2 verbose ... 7 fatal.
                    String level = priority >= 2 && priority <= 7
                            ? String.valueOf("VDIWEF".charAt(priority - 2)) : "?";
                    logLines.add(timestamp + " " + level + " " + buffer + "/" + tag + ": "
                            + text.trim());
                } else {
                    p.skip(key & 7);
                }
            }
        }

        /**
         * The signal that really hit the game. A fault the runtime's handler
         * passes on ends as SIGABRT from libsigchain with "exiting due to
         * SIG_DFL handler for signal N"; N is the original signal.
         */
        String crashSignal() {
            java.util.regex.Matcher m = java.util.regex.Pattern
                    .compile("for signal (\\d+)").matcher(abortMessage);
            if (m.find()) {
                switch (Integer.parseInt(m.group(1))) {
                    case 4: return "SIGILL (illegal instruction)";
                    case 6: return "SIGABRT";
                    case 7: return "SIGBUS (bus error)";
                    case 8: return "SIGFPE";
                    case 11: return "SIGSEGV (invalid memory access)";
                    default: return "signal " + m.group(1);
                }
            }
            return signal;
        }

        /**
         * Where it happened: the first frame below the signal trampoline (the
         * interrupted code) that is not part of libc or the signal chain.
         */
        String crashSite() {
            int start = 0;
            for (int i = 0; i < frameFunctions.size(); ++i) {
                if (frameFunctions.get(i).contains("sigreturn")) {
                    start = i + 1;
                    break;
                }
            }
            for (int i = start; i < frameLibraries.size(); ++i) {
                String library = frameLibraries.get(i);
                if (library.isEmpty() || library.equals("libc.so") || library.equals("libsigchain.so")
                        || library.equals("[vdso]")) {
                    continue;
                }
                String function = frameFunctions.get(i);
                return library + (function.isEmpty() ? "" : " (" + function + ")");
            }
            return frameLibraries.isEmpty() ? "?" : frameLibraries.get(0);
        }

        String format() {
            StringBuilder out = new StringBuilder("---- native crash\n");
            out.append("signal: ").append(signal).append('\n');
            if (!abortMessage.isEmpty()) out.append("abort message: ").append(abortMessage).append('\n');
            for (String cause : causes) if (!cause.isEmpty()) out.append("cause: ").append(cause).append('\n');
            out.append("thread: ").append(threadName).append(" (tid ").append(tid).append(")\n");
            out.append("abi: ").append(abi).append('\n');
            out.append("build: ").append(fingerprint).append('\n');
            out.append("backtrace:\n");
            for (int i = 0; i < Math.min(frames.size(), 64); ++i) out.append("  ").append(frames.get(i)).append('\n');
            if (!logLines.isEmpty()) {
                out.append("---- last log lines of the game process\n");
                int from = Math.max(0, logLines.size() - 120);
                for (int i = from; i < logLines.size(); ++i) out.append(logLines.get(i)).append('\n');
            }
            return out.toString();
        }
    }

    /** A minimal protobuf wire-format reader. */
    private static final class Proto {
        private final byte[] data;
        private int position;
        private final int end;

        Proto(byte[] data, int offset, int length) {
            this.data = data;
            this.position = offset;
            this.end = offset + length;
        }

        boolean more() {
            return position < end;
        }

        long varint() {
            long result = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (position >= end) throw new IllegalStateException("truncated varint");
                byte b = data[position++];
                result |= (long) (b & 0x7F) << shift;
                if ((b & 0x80) == 0) return result;
            }
            throw new IllegalStateException("bad varint");
        }

        int varint32() {
            return (int) varint();
        }

        byte[] bytes() {
            int length = varint32();
            if (length < 0 || position + length > end) throw new IllegalStateException("bad length");
            byte[] out = Arrays.copyOfRange(data, position, position + length);
            position += length;
            return out;
        }

        String string() {
            return new String(bytes(), StandardCharsets.UTF_8);
        }

        void skip(int type) {
            switch (type) {
                case 0: varint(); break;
                case 1: position += 8; break;
                case 2: bytes(); break;
                case 5: position += 4; break;
                default: throw new IllegalStateException("wire type " + type);
            }
            if (position > end) throw new IllegalStateException("truncated");
        }
    }
}
