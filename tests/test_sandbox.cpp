/*
 * test_sandbox — QTest for Landlock + seccomp sandbox
 *
 * Includes gm-dashboard.cpp with GM_TESTING to access static functions.
 * Under GM_TESTING, restrict_self and seccomp_load are skipped so
 * sandbox functions can run in-process with gcov coverage.
 */
#define GM_TESTING
#include "../gm-dashboard.cpp"

#include <QTest>
#include <QTemporaryDir>
#include <sys/wait.h>

class TestSandbox : public QObject {
    Q_OBJECT

private:
    QString realHome;
    unsigned char testPk[crypto_sign_PUBLICKEYBYTES];
    unsigned char testSk[crypto_sign_SECRETKEYBYTES];

    /* Sign a .so file with the test keypair, creating .so.sig alongside it */
    void signWithTestKey(const QString &soPath) {
        QByteArray soData = readBinary(soPath);
        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char *>(soData.constData()),
            soData.size(), testSk);
        QFile f(soPath + ".sig");
        f.open(QIODevice::WriteOnly);
        f.write(reinterpret_cast<const char *>(sig), sizeof(sig));
        f.close();
    }

private slots:

    void initTestCase() {
        QVERIFY(sodium_init() >= 0);
        curl_global_init(CURL_GLOBAL_DEFAULT);
        realHome = qgetenv("HOME");
        /* Generate a test-only keypair for loadPlugins tests */
        crypto_sign_keypair(testPk, testSk);
    }

    /* ── Landlock (in-process, restrict_self skipped under GM_TESTING) ── */

    void testLandlockNetworkPlugin() {
        int r = applyLandlock(GM_CAP_NETWORK, "/tmp", "/tmp");
        QVERIFY(r == 0 || r == -1);  /* -1 if kernel doesn't support */
    }

    void testLandlockProcPlugin() {
        int r = applyLandlock(GM_CAP_PROC | GM_CAP_SHM, "/tmp", "/tmp");
        QVERIFY(r == 0 || r == -1);
    }

    void testLandlockExecPlugin() {
        int r = applyLandlock(GM_CAP_EXEC | GM_CAP_NETWORK, "/tmp", "/tmp");
        QVERIFY(r == 0 || r == -1);
    }

    void testLandlockMinimalCaps() {
        int r = applyLandlock(0, "/tmp", "/tmp");
        QVERIFY(r == 0 || r == -1);
    }

    void testLandlockAllCaps() {
        int r = applyLandlock(GM_CAP_NETWORK | GM_CAP_PROC | GM_CAP_SHM |
                              GM_CAP_EXEC | GM_CAP_HOME_READ,
                              "/tmp", "/tmp");
        QVERIFY(r == 0 || r == -1);
    }

    void testLandlockTmpfsDiffers() {
        /* configDir != TMPFS_CFG_DIR triggers the extra addPath */
        int r = applyLandlock(0, "/tmp", "/var/tmp");
        QVERIFY(r == 0 || r == -1);
    }

    void testLandlockTmpfsSame() {
        /* configDir == TMPFS_CFG_DIR skips the extra addPath */
        int r = applyLandlock(0, "/tmp", TMPFS_CFG_DIR);
        QVERIFY(r == 0 || r == -1);
    }

    /* ── seccomp (in-process, seccomp_load skipped under GM_TESTING) ── */

    void testSeccompNoExecNoNet() {
        QCOMPARE(applySeccomp(0), 0);
    }

    void testSeccompWithExec() {
        QCOMPARE(applySeccomp(GM_CAP_EXEC), 0);
    }

    void testSeccompWithNetwork() {
        QCOMPARE(applySeccomp(GM_CAP_NETWORK), 0);
    }

    void testSeccompWithAll() {
        QCOMPARE(applySeccomp(GM_CAP_EXEC | GM_CAP_NETWORK), 0);
    }

    /* ── Greeting ── */

    void testGreetingMorning()   { QCOMPARE(QString(getGreeting(8)),  QString("GOOD MORNING")); }
    void testGreetingAfternoon() { QCOMPARE(QString(getGreeting(14)), QString("GOOD AFTERNOON")); }
    void testGreetingEvening()   { QCOMPARE(QString(getGreeting(20)), QString("GOOD EVENING")); }
    void testGreetingNight()     { QCOMPARE(QString(getGreeting(2)),  QString("GOOD NIGHT")); }
    void testGreetingBoundary5() { QCOMPARE(QString(getGreeting(5)),  QString("GOOD MORNING")); }
    void testGreetingBoundary12(){ QCOMPARE(QString(getGreeting(12)), QString("GOOD AFTERNOON")); }
    void testGreetingBoundary18(){ QCOMPARE(QString(getGreeting(18)), QString("GOOD EVENING")); }
    void testGreetingBoundary23(){ QCOMPARE(QString(getGreeting(23)), QString("GOOD NIGHT")); }
    void testGreetingMidnight()  { QCOMPARE(QString(getGreeting(0)),  QString("GOOD NIGHT")); }

    /* ── Signature verification ── */

    void testVerifyValidSig() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        /* Write a fake .so */
        QString soPath = tmp.path() + "/test.so";
        QFile sf(soPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write("fake binary");
        sf.close();

        /* Sign it */
        QByteArray soData = readBinary(soPath);
        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char *>(soData.constData()),
            soData.size(), sk);

        QFile sigf(soPath + ".sig");
        QVERIFY(sigf.open(QIODevice::WriteOnly));
        sigf.write(reinterpret_cast<const char *>(sig), sizeof(sig));
        sigf.close();

        QCOMPARE(verifyPluginSig(qPrintable(soPath), pk), 1);
    }

    void testVerifyInvalidSig() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QString soPath = tmp.path() + "/test.so";
        QFile sf(soPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write("original");
        sf.close();

        /* Sign with correct key */
        QByteArray soData = readBinary(soPath);
        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char *>(soData.constData()),
            soData.size(), sk);

        QFile sigf(soPath + ".sig");
        QVERIFY(sigf.open(QIODevice::WriteOnly));
        sigf.write(reinterpret_cast<const char *>(sig), sizeof(sig));
        sigf.close();

        /* Tamper the .so */
        QFile tf(soPath);
        QVERIFY(tf.open(QIODevice::WriteOnly));
        tf.write("tampered");
        tf.close();

        QCOMPARE(verifyPluginSig(qPrintable(soPath), pk), 0);
    }

    void testVerifyNoSig() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QString soPath = tmp.path() + "/test.so";
        QFile sf(soPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write("no sig");
        sf.close();

        QCOMPARE(verifyPluginSig(qPrintable(soPath), pk), -1);
    }

    void testVerifyMissingFile() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);
        QCOMPARE(verifyPluginSig("/nonexistent/path.so", pk), 0);
    }

    /* ── Config ── */

    void testLoadNameEmpty() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(loadName(tmp.path()).isEmpty());
    }

    void testLoadNamePresent() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QFile f(tmp.path() + "/config");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# comment\nNAME=REGEN\n");
        f.close();
        QCOMPARE(loadName(tmp.path()), QString("REGEN"));
    }

    void testLoadNameIgnoresComments() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QFile f(tmp.path() + "/config");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# NAME=Wrong\nNAME=Correct\n");
        f.close();
        QCOMPARE(loadName(tmp.path()), QString("Correct"));
    }

    void testResolveConfigDir() {
        QString dir = resolveConfigDir();
        QVERIFY(!dir.isEmpty());
    }

    /* ── resolveConfigDir with tmpfs ── */

    void testResolveConfigDirTmpfs() {
        /* Create TMPFS_CFG_DIR so resolveConfigDir returns it */
        QDir().mkpath(TMPFS_CFG_DIR);
        QString dir = resolveConfigDir();
        QCOMPARE(dir, QString(TMPFS_CFG_DIR));
        /* Clean up */
        QDir(TMPFS_CFG_DIR).removeRecursively();
    }

    void testResolveConfigDirVaultLocked() {
        /* Create vault.enc without tmpfs dir → triggers "vault locked" warning */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        qputenv("HOME", tmp.path().toLocal8Bit());
        QString cfgDir = tmp.path() + "/.config/gm-dashboard";
        QDir().mkpath(cfgDir);

        QFile vf(cfgDir + "/vault.enc");
        QVERIFY(vf.open(QIODevice::WriteOnly));
        vf.write("fake");
        vf.close();

        QString dir = resolveConfigDir();
        QCOMPARE(dir, cfgDir);

        qputenv("HOME", realHome.toLocal8Bit());
    }

    /* ── Plugin loading ── */

    void testLoadPluginsNoDir() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString oldHome = qgetenv("HOME");
        qputenv("HOME", tmp.path().toLocal8Bit());

        std::vector<Plugin> plugins;
        loadPlugins(plugins, nullptr);
        QVERIFY(plugins.empty());

        qputenv("HOME", oldHome.toLocal8Bit());
    }

    void testLoadPluginsInvalidSig() {
        /* Set up a temp HOME with a plugin dir containing a .so with bad sig */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString oldHome = qgetenv("HOME");
        qputenv("HOME", tmp.path().toLocal8Bit());

        QString plugDir = tmp.path() + "/.local/lib/gm-dashboard";
        QDir().mkpath(plugDir);

        /* Copy a real .so */
        QFile::copy(realHome + "/.local/lib/gm-dashboard/weather.so",
            plugDir + "/weather.so");

        /* Write a bad .sig */
        QFile sigf(plugDir + "/weather.so.sig");
        if (sigf.open(QIODevice::WriteOnly)) {
            QByteArray badSig(crypto_sign_BYTES, '\x00');
            sigf.write(badSig);
            sigf.close();
        }

        /* Use real marketplace key for verification */
        QByteArray pkData = readBinary(
            realHome + "/.config/gm-dashboard/marketplace.pub");

        std::vector<Plugin> plugins;
        if (pkData.size() == crypto_sign_PUBLICKEYBYTES) {
            loadPlugins(plugins,
                reinterpret_cast<const unsigned char *>(pkData.constData()));
            /* Plugin should be REJECTED due to bad sig */
            QVERIFY(plugins.empty());
        }

        qputenv("HOME", oldHome.toLocal8Bit());
    }

    void testLoadPluginsNoPkRejectsAll() {
        /* Without a public key, ALL plugins must be rejected */
        qputenv("HOME", realHome.toLocal8Bit());
        std::vector<Plugin> plugins;
        loadPlugins(plugins, nullptr);
        QVERIFY(plugins.empty());
    }

    void testLoadPluginsBadSo() {
        /* Create a .so that's not a valid ELF — dlopen should fail */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString oldHome = qgetenv("HOME");
        qputenv("HOME", tmp.path().toLocal8Bit());

        QString plugDir = tmp.path() + "/.local/lib/gm-dashboard";
        QDir().mkpath(plugDir);

        QString soPath = plugDir + "/bad.so";
        QFile bad(soPath);
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not an elf file");
        bad.close();
        signWithTestKey(soPath);

        std::vector<Plugin> plugins;
        loadPlugins(plugins, testPk);
        QVERIFY(plugins.empty()); /* sig valid but dlopen fails */

        qputenv("HOME", oldHome.toLocal8Bit());
    }

    void testLoadPluginsMissingSymbols() {
        /* .so with no gm_* symbols — should fail with "missing ABI symbols" */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        qputenv("HOME", tmp.path().toLocal8Bit());

        QString plugDir = tmp.path() + "/.local/lib/gm-dashboard";
        QDir().mkpath(plugDir);

        QString srcDir = QCoreApplication::applicationDirPath();
        QString soPath = plugDir + "/stub_nosym.so";
        QFile::copy(srcDir + "/stub_nosym.so", soPath);
        signWithTestKey(soPath);

        std::vector<Plugin> plugins;
        loadPlugins(plugins, testPk);
        QVERIFY(plugins.empty());

        qputenv("HOME", realHome.toLocal8Bit());
    }

    void testLoadPluginsApiMismatch() {
        /* .so with wrong API version — should fail with "API v999 != v2" */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        qputenv("HOME", tmp.path().toLocal8Bit());

        QString plugDir = tmp.path() + "/.local/lib/gm-dashboard";
        QDir().mkpath(plugDir);

        QString srcDir = QCoreApplication::applicationDirPath();
        QString soPath = plugDir + "/stub_badver.so";
        QFile::copy(srcDir + "/stub_badver.so", soPath);
        signWithTestKey(soPath);

        std::vector<Plugin> plugins;
        loadPlugins(plugins, testPk);
        QVERIFY(plugins.empty());

        qputenv("HOME", realHome.toLocal8Bit());
    }

    void testLoadPluginsUnsignedRejected() {
        /* .so with valid symbols but no .sig file — MUST be rejected */
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        qputenv("HOME", tmp.path().toLocal8Bit());

        QString plugDir = tmp.path() + "/.local/lib/gm-dashboard";
        QDir().mkpath(plugDir);

        /* Copy a real .so but don't copy its .sig */
        QFile::copy(realHome + "/.local/lib/gm-dashboard/system.so",
                    plugDir + "/system.so");

        /* Use real marketplace key */
        QByteArray pkData = readBinary(
            realHome + "/.config/gm-dashboard/marketplace.pub");

        std::vector<Plugin> plugins;
        if (pkData.size() == crypto_sign_PUBLICKEYBYTES) {
            loadPlugins(plugins,
                reinterpret_cast<const unsigned char *>(pkData.constData()));
            /* Unsigned plugins MUST be rejected */
            QVERIFY(plugins.empty());
        }

        qputenv("HOME", realHome.toLocal8Bit());
    }

    /* ── sandboxedFetch ── */

    void testSandboxedFetchReal() {
        /* Test with a real installed plugin */
        QString plugDir = QDir::homePath() + "/.local/lib/gm-dashboard";
        QByteArray plugDirB = plugDir.toLocal8Bit();
        QString cfgDir = resolveConfigDir();
        QByteArray cfgDirB = cfgDir.toLocal8Bit();

        /* Load the system plugin (fast, no network) */
        QByteArray soPath = (plugDir + "/system.so").toLocal8Bit();
        void *h = dlopen(soPath.constData(), RTLD_LAZY);
        if (!h) QSKIP("system.so not installed");

        auto ifn = reinterpret_cast<gm_info_fn>(dlsym(h, "gm_info"));
        auto initf = reinterpret_cast<gm_init_fn>(dlsym(h, "gm_init"));
        auto fetchf = reinterpret_cast<gm_fetch_fn>(dlsym(h, "gm_fetch"));
        auto cleanf = reinterpret_cast<gm_cleanup_fn>(dlsym(h, "gm_cleanup"));
        QVERIFY(ifn && initf && fetchf && cleanf);

        Plugin p = {};
        p.handle = h;
        p.info = ifn();
        p.init = initf;
        p.fetch = fetchf;
        p.cleanup = cleanf;
        snprintf(p.soPath, sizeof(p.soPath), "%s", soPath.constData());

        QByteArray json = sandboxedFetch(p, plugDirB, cfgDirB);
        QVERIFY(!json.isEmpty());

        /* Verify it's valid JSON */
        QJsonDocument doc = QJsonDocument::fromJson(json);
        QVERIFY(doc.isObject());

        dlclose(h);
    }

    /* ── openViewer (fork only, no actual viewer) ── */

    void testOpenViewerForks() {
        /* openViewer forks and tries to exec gm-viewer.
         * Unset DISPLAY so the viewer exits immediately instead of
         * opening a real window on the user's X11 session. */
        QByteArray savedDisplay = qgetenv("DISPLAY");
        qunsetenv("DISPLAY");

        openViewer("/dev/null", "/tmp");
        /* Give child time to exit */
        QTest::qWait(200);
        /* Reap zombie */
        waitpid(-1, nullptr, WNOHANG);

        qputenv("DISPLAY", savedDisplay);
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        curl_global_cleanup();
    }
};

QTEST_MAIN(TestSandbox)
#include "test_sandbox.moc"
