package com.libertyrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;

public class DriverTrustTest {

    private File mRoot;
    private File mPackage;
    private DriverTrust mTrust;

    @Before
    public void setUp() throws IOException {
        mRoot = Files.createTempDirectory("driver-trust").toFile();
        mPackage = new File(mRoot, "turnip-gen8");
        assertTrue(mPackage.mkdirs());
        write(new File(mPackage, "libvulkan_freedreno.so"), "driver code");
        write(new File(mPackage, "meta.json"), "{\"libraryName\":\"libvulkan_freedreno.so\"}");
        mTrust = DriverTrust.inPrivateDir(new File(mRoot, "private"));
    }

    @After
    public void tearDown() {
        delete(mRoot);
    }

    @Test
    public void unknownPackageIsNotTrusted() throws IOException {
        assertFalse(mTrust.isTrusted("turnip-gen8"));
        assertFalse(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));
    }

    @Test
    public void acceptedPackageMatchesUntilAFileChanges() throws IOException {
        mTrust.trust("turnip-gen8", DriverTrust.hashDirectory(mPackage));
        assertTrue(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));

        write(new File(mPackage, "libvulkan_freedreno.so"), "something else");
        assertFalse(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));
        assertTrue("the record is still there, so the launcher can say it changed",
                mTrust.isTrusted("turnip-gen8"));
    }

    @Test
    public void anAddedFileBreaksTheMatch() throws IOException {
        mTrust.trust("turnip-gen8", DriverTrust.hashDirectory(mPackage));
        write(new File(mPackage, "libextra.so"), "planted");
        assertFalse(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));
    }

    @Test
    public void aRemovedFileBreaksTheMatch() throws IOException {
        mTrust.trust("turnip-gen8", DriverTrust.hashDirectory(mPackage));
        assertTrue(new File(mPackage, "meta.json").delete());
        assertFalse(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));
    }

    @Test
    public void emptyDirectoryNeverMatches() throws IOException {
        File empty = new File(mRoot, "empty");
        assertTrue(empty.mkdirs());
        mTrust.trust("empty", DriverTrust.hashDirectory(empty));
        assertFalse(mTrust.matches("empty", DriverTrust.hashDirectory(empty)));
    }

    @Test
    public void storeSurvivesANewInstanceAndForgetRemovesIt() throws IOException {
        mTrust.trust("turnip-gen8", DriverTrust.hashDirectory(mPackage));
        DriverTrust reopened = DriverTrust.inPrivateDir(new File(mRoot, "private"));
        assertTrue(reopened.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));

        reopened.forget("turnip-gen8");
        assertFalse(mTrust.isTrusted("turnip-gen8"));
    }

    @Test
    public void packagesAreIndependent() throws IOException {
        File other = new File(mRoot, "panvk");
        assertTrue(other.mkdirs());
        write(new File(other, "libvulkan_panfrost.so"), "other driver");
        mTrust.trust("turnip-gen8", DriverTrust.hashDirectory(mPackage));
        mTrust.trust("panvk", DriverTrust.hashDirectory(other));
        mTrust.forget("panvk");
        assertTrue(mTrust.matches("turnip-gen8", DriverTrust.hashDirectory(mPackage)));
        assertFalse(mTrust.isTrusted("panvk"));
    }

    @Test
    public void malformedStoreLinesTrustNothing() throws IOException {
        File store = new File(new File(mRoot, "private"), DriverTrust.STORE_NAME);
        assertTrue(store.getParentFile().mkdirs());
        String hash = DriverTrust.sha256(new File(mPackage, "meta.json"));
        write(store, "turnip-gen8\tmeta.json\tNOTAHASH\n"
                + "../escape\tmeta.json\t" + hash + "\n"
                + "too\tmany\tfields\there\n");
        assertFalse(mTrust.isTrusted("turnip-gen8"));
        assertFalse(mTrust.isTrusted("../escape"));
    }

    @Test
    public void sha256OfKnownContent() throws IOException {
        File file = new File(mRoot, "abc");
        write(file, "abc");
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                DriverTrust.sha256(file));
    }

    @Test
    public void plainFileNames() {
        assertTrue(DriverTrust.isPlainFileName("libvulkan_freedreno.so"));
        assertTrue(DriverTrust.isPlainFileName("v1..2.so"));
        assertFalse(DriverTrust.isPlainFileName(null));
        assertFalse(DriverTrust.isPlainFileName(""));
        assertFalse(DriverTrust.isPlainFileName("."));
        assertFalse(DriverTrust.isPlainFileName(".."));
        assertFalse(DriverTrust.isPlainFileName("../libevil.so"));
        assertFalse(DriverTrust.isPlainFileName("sub/lib.so"));
        assertFalse(DriverTrust.isPlainFileName("..\\lib.so"));
        assertFalse(DriverTrust.isPlainFileName("/data/local/tmp/lib.so"));
        assertFalse(DriverTrust.isPlainFileName("a\tb"));
        assertFalse(DriverTrust.isPlainFileName("a\nb"));
    }

    @Test
    public void trustRejectsPathNames() throws IOException {
        try {
            mTrust.trust("../x", DriverTrust.hashDirectory(mPackage));
            fail("a package name with a path was accepted");
        } catch (IOException expected) {
        }
        try {
            mTrust.trust("ok", Map.of("../lib.so",
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
            fail("a file name with a path was accepted");
        } catch (IOException expected) {
        }
        assertFalse(mTrust.isTrusted("ok"));
    }

    private static void write(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
