/*
 * test_config — QTest for gm-config encrypted vault
 *
 * Uses GM_VAULT_PASSWORD env var for non-interactive testing.
 *
 * Verifies:
 * 1. Binary exists
 * 2. Usage on no args
 * 3. Init creates vault file
 * 4. Set stores key-value pairs
 * 5. Get retrieves stored values
 * 6. List shows sections
 * 7. Del removes keys and sections
 * 8. Lock wipes tmpfs
 * 9. Unlock decrypts to tmpfs
 * 10. Error: wrong password
 * 11. Error: vault not found
 * 12. Error: invalid command
 */
#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

class TestConfig : public QObject {
    Q_OBJECT

private:
    QString cfgBin;
    QTemporaryDir *tmpHome = nullptr;
    QProcessEnvironment env;

    int run(const QStringList &args, QByteArray *out = nullptr,
            QByteArray *err = nullptr, const char *password = "testpass1234") {
        QProcess proc;
        QProcessEnvironment e = env;
        if (password)
            e.insert("GM_VAULT_PASSWORD", password);
        proc.setProcessEnvironment(e);
        proc.start(cfgBin, args);
        if (!proc.waitForFinished(30000)) return -1;
        if (out) *out = proc.readAllStandardOutput();
        if (err) *err = proc.readAllStandardError();
        return proc.exitCode();
    }

private slots:

    void initTestCase() {
        cfgBin = QDir::homePath() + "/.local/bin/gm-config";
        QVERIFY2(QFile::exists(cfgBin), "gm-config not installed");

        tmpHome = new QTemporaryDir;
        QVERIFY(tmpHome->isValid());

        /* Use temp HOME so we don't touch real vault */
        env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", tmpHome->path());

        /* Create config dir */
        QDir().mkpath(tmpHome->path() + "/.config/gm-dashboard");
    }

    /* ── CLI basics ── */

    void testUsage() {
        QByteArray err;
        int rc = run({}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("Usage:"));
    }

    void testUnknownCommand() {
        QByteArray err;
        int rc = run({"bogus"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("Unknown command"));
    }

    /* ── Init ── */

    void testInit() {
        int rc = run({"init"});
        QCOMPARE(rc, 0);

        QString vault = tmpHome->path() + "/.config/gm-dashboard/vault.enc";
        QVERIFY2(QFile::exists(vault), "Vault file not created");

        QFile f(vault);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray data = f.readAll();
        QVERIFY2(data.startsWith("GMV1"), "Vault missing magic header");
        QVERIFY2(data.size() > 44, "Vault too small");
    }

    void testInitAlreadyExists() {
        /* Vault already exists from testInit */
        QByteArray err;
        int rc = run({"init"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("already exists"));
    }

    /* ── Set / Get ── */

    void testSetAndGet() {
        int rc = run({"set", "weather", "CITY=Brasilia"});
        QCOMPARE(rc, 0);

        QByteArray out;
        rc = run({"get", "weather", "CITY"}, &out);
        QCOMPARE(rc, 0);
        QCOMPARE(out.trimmed(), QByteArray("Brasilia"));
    }

    void testSetMultipleKeys() {
        run({"set", "weather", "CITY=Goiania"});
        run({"set", "weather", "UNITS=metric"});

        QByteArray out;
        int rc = run({"get", "weather"}, &out);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("CITY=Goiania"));
        QVERIFY(out.contains("UNITS=metric"));
    }

    void testSetDifferentSections() {
        run({"set", "global", "NAME=REGEN"});
        run({"set", "github", "TOKEN=fake123"});

        QByteArray out;
        run({"get", "global", "NAME"}, &out);
        QCOMPARE(out.trimmed(), QByteArray("REGEN"));

        out.clear();
        run({"get", "github", "TOKEN"}, &out);
        QCOMPARE(out.trimmed(), QByteArray("fake123"));
    }

    void testGetMissingSection() {
        QByteArray err;
        int rc = run({"get", "nonexistent"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("not found"));
    }

    void testGetMissingKey() {
        QByteArray err;
        int rc = run({"get", "weather", "NONEXISTENT"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("not found"));
    }

    void testGetMissingArgs() {
        QByteArray err;
        int rc = run({"get"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("Usage:"));
    }

    void testSetMissingArgs() {
        QByteArray err;
        int rc = run({"set"}, nullptr, &err);
        QCOMPARE(rc, 1);
    }

    void testSetBadFormat() {
        QByteArray err;
        int rc = run({"set", "weather", "NOEQUALS"}, nullptr, &err);
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("KEY=VALUE"));
    }

    /* ── List ── */

    void testList() {
        QByteArray out;
        int rc = run({"list"}, &out);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("[weather]"));
        QVERIFY(out.contains("[global]"));
    }

    /* ── Del ── */

    void testDelKey() {
        /* UNITS was set earlier */
        int rc = run({"del", "weather", "UNITS"});
        QCOMPARE(rc, 0);

        QByteArray out;
        run({"get", "weather"}, &out);
        QVERIFY(!out.contains("UNITS"));
        QVERIFY(out.contains("CITY"));  /* other keys survive */
    }

    void testDelSection() {
        run({"set", "temp", "A=1"});
        int rc = run({"del", "temp"});
        QCOMPARE(rc, 0);

        QByteArray err;
        rc = run({"get", "temp"}, nullptr, &err);
        QCOMPARE(rc, 1);
    }

    void testDelMissingArgs() {
        QByteArray err;
        int rc = run({"del"}, nullptr, &err);
        QCOMPARE(rc, 1);
    }

    /* ── Lock / Unlock ── */

    void testUnlock() {
        int rc = run({"unlock"});
        QCOMPARE(rc, 0);

        QVERIFY(QDir("/dev/shm/gm-cfg").exists());
        /* Weather config should be there */
        QVERIFY(QFile::exists("/dev/shm/gm-cfg/weather.conf"));

        QFile f("/dev/shm/gm-cfg/weather.conf");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray content = f.readAll();
        QVERIFY(content.contains("CITY="));
    }

    void testLock() {
        int rc = run({"lock"});
        QCOMPARE(rc, 0);

        QVERIFY(!QDir("/dev/shm/gm-cfg").exists());
    }

    void testLockAlreadyLocked() {
        QByteArray out;
        int rc = run({"lock"}, &out);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("Already locked"));
    }

    /* ── Wrong password ── */

    void testWrongPassword() {
        QByteArray err;
        int rc = run({"list"}, nullptr, &err, "wrongpassword");
        QCOMPARE(rc, 1);
        QVERIFY(err.contains("wrong password") || err.contains("corrupted"));
    }

    /* ── Vault not found ── */

    void testNoVault() {
        /* Create a fresh temp home with no vault */
        QTemporaryDir tmp2;
        QVERIFY(tmp2.isValid());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        QProcess proc;
        QProcessEnvironment e = env;
        e.insert("HOME", tmp2.path());
        e.insert("GM_VAULT_PASSWORD", "testpass1234");
        proc.setProcessEnvironment(e);
        proc.start(cfgBin, {"list"});
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 1);
        QVERIFY(proc.readAllStandardError().contains("not found"));
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        /* Make sure we clean up tmpfs */
        run({"lock"});
        delete tmpHome;
    }
};

QTEST_MAIN(TestConfig)
#include "test_config.moc"
