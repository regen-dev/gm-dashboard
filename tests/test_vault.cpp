/*
 * test_vault — QTest for gm-config vault internals
 *
 * Includes gm-config.cpp with GM_TESTING to access static functions.
 * Tests vault encryption/decryption, save/open, and command functions
 * without interactive password prompts (uses GM_VAULT_PASSWORD env var).
 */
#define GM_TESTING
#include "../gm-config.cpp"

#include <QTest>
#include <QTemporaryDir>

class TestVault : public QObject {
    Q_OBJECT

private:
    QTemporaryDir *tmpDir = nullptr;
    QString origHome;

    void setTestHome(const QString &path) {
        qputenv("HOME", path.toLocal8Bit());
    }

    void restoreHome() {
        qputenv("HOME", origHome.toLocal8Bit());
    }

private slots:

    void initTestCase() {
        QVERIFY(sodium_init() >= 0);
        origHome = qgetenv("HOME");

        tmpDir = new QTemporaryDir;
        QVERIFY(tmpDir->isValid());

        /* Create config dir for test vault */
        QDir().mkpath(tmpDir->path() + "/.config/gm-dashboard");
        setTestHome(tmpDir->path());
    }

    /* ── Key derivation ── */

    void testDeriveKeySucceeds() {
        unsigned char salt[crypto_pwhash_SALTBYTES];
        randombytes_buf(salt, sizeof(salt));
        unsigned char key[crypto_secretbox_KEYBYTES];
        QCOMPARE(deriveKey("testpassword", salt, key), 0);
    }

    /* ── Vault save + open ── */

    void testVaultSaveOpen() {
        QJsonObject data;
        data["weather"] = QJsonObject{{"CITY", "Brasilia"}};

        QCOMPARE(vaultSave(data, "testpass1234"), 0);
        QVERIFY(QFile::exists(vaultPath()));

        bool ok;
        QJsonObject loaded = vaultOpen("testpass1234", &ok);
        QVERIFY(ok);
        QVERIFY(loaded.contains("weather"));
        QCOMPARE(loaded["weather"].toObject()["CITY"].toString(),
                 QString("Brasilia"));
    }

    void testVaultMagicHeader() {
        QFile f(vaultPath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray raw = f.readAll();
        QVERIFY(raw.startsWith("GMV1"));
    }

    void testVaultWrongPassword() {
        bool ok;
        vaultOpen("wrongpassword", &ok);
        QVERIFY(!ok);
    }

    void testVaultNotExists() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        QVERIFY(!vaultExists());

        setTestHome(tmpDir->path());
    }

    /* ── Vault error paths ── */

    void testVaultOpenCorrupt() {
        /* Write garbage to vault file */
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        QFile f(vaultPath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("tiny");  /* too small */
        f.close();

        bool ok;
        vaultOpen("pass", &ok);
        QVERIFY(!ok);

        setTestHome(tmpDir->path());
    }

    void testVaultOpenBadMagic() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        QFile f(vaultPath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        /* Write enough bytes but wrong magic */
        QByteArray bad(512, '\x00');
        bad[0] = 'X'; bad[1] = 'X'; bad[2] = 'X'; bad[3] = 'X';
        f.write(bad);
        f.close();

        bool ok;
        vaultOpen("pass", &ok);
        QVERIFY(!ok);

        setTestHome(tmpDir->path());
    }

    void testVaultModifyNoVault() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        /* No vault exists — vaultModify should fail */
        SetCtx ctx = {"sec", "K", "V"};
        QCOMPARE(vaultModify(doSet, &ctx), 1);

        setTestHome(tmpDir->path());
    }

    /* ── Vault modify callback ── */

    void testDoSet() {
        QJsonObject data;
        SetCtx ctx = {"test_section", "KEY1", "VALUE1"};
        QCOMPARE(doSet(data, &ctx), 0);
        QCOMPARE(data["test_section"].toObject()["KEY1"].toString(),
                 QString("VALUE1"));
    }

    void testDoDelKey() {
        QJsonObject data;
        QJsonObject sec;
        sec["A"] = "1";
        sec["B"] = "2";
        data["sec"] = sec;

        DelCtx ctx = {"sec", "A"};
        QCOMPARE(doDel(data, &ctx), 0);
        QVERIFY(!data["sec"].toObject().contains("A"));
        QVERIFY(data["sec"].toObject().contains("B"));
    }

    void testDoDelSection() {
        QJsonObject data;
        data["todel"] = QJsonObject{{"X", "Y"}};

        DelCtx ctx = {"todel", nullptr};
        QCOMPARE(doDel(data, &ctx), 0);
        QVERIFY(!data.contains("todel"));
    }

    /* ── cmdInit ── */

    void testCmdInit() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        qputenv("GM_VAULT_PASSWORD", "initpass1234");
        QCOMPARE(cmdInit(), 0);
        QVERIFY(vaultExists());

        setTestHome(tmpDir->path());
    }

    void testCmdInitAlreadyExists() {
        /* tmpDir has a vault from testVaultSaveOpen */
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdInit(), 1);
    }

    void testCmdInitShortPassword() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");

        qputenv("GM_VAULT_PASSWORD", "short");
        QCOMPARE(cmdInit(), 1);
        QVERIFY(!vaultExists());

        setTestHome(tmpDir->path());
    }

    /* ── cmdSet via vaultModify ── */

    void testCmdSet() {
        QVERIFY(vaultExists());
        qputenv("GM_VAULT_PASSWORD", "testpass1234");

        int rc = cmdSet("github", "TOKEN=ghp_fake123");
        QCOMPARE(rc, 0);

        bool ok;
        QJsonObject data = vaultOpen("testpass1234", &ok);
        QVERIFY(ok);
        QCOMPARE(data["github"].toObject()["TOKEN"].toString(),
                 QString("ghp_fake123"));
    }

    void testCmdSetBadFormat() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdSet("weather", "NOEQUALS"), 1);
    }

    /* ── cmdGet ── */

    void testCmdGet() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdGet("weather", "CITY"), 0);
    }

    void testCmdGetMissingSection() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdGet("nonexistent", nullptr), 1);
    }

    void testCmdGetMissingKey() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdGet("weather", "NONEXISTENT"), 1);
    }

    void testCmdGetAllKeys() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdGet("weather", nullptr), 0);
    }

    void testCmdGetNoVault() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdGet("weather", nullptr), 1);
        setTestHome(tmpDir->path());
    }

    /* ── cmdList ── */

    void testCmdList() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdList(), 0);
    }

    /* ── cmdDel ── */

    void testCmdDel() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        cmdSet("temp", "X=1");
        QCOMPARE(cmdDel("temp", "X"), 0);
    }

    void testCmdDelSection() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        cmdSet("temp2", "Y=2");
        QCOMPARE(cmdDel("temp2", nullptr), 0);
    }

    /* ── cmdPasswd ── */

    void testCmdPasswd() {
        /* GM_VAULT_PASSWORD provides same value for all readPassword calls:
         * "Current password" = testpass1234
         * "New password" = testpass1234
         * "Confirm" = testpass1234
         * Password stays the same (valid test of the code path) */
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdPasswd(), 0);

        /* Verify vault still works */
        bool ok;
        QJsonObject data = vaultOpen("testpass1234", &ok);
        QVERIFY(ok);
    }

    void testCmdPasswdNoVault() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdPasswd(), 1);
        setTestHome(tmpDir->path());
    }

    void testCmdPasswdShortNew() {
        /* Create a vault with long password, then try to change to short.
         * Since GM_VAULT_PASSWORD provides same value for all prompts,
         * we need a short password for both old and new.
         * But short old password won't open the vault.
         * So we create a vault with a short-ish password first (must be >=8).
         * Then set env to short — but that means vaultOpen fails first.
         * This tests the wrong-password path in cmdPasswd. */
        qputenv("GM_VAULT_PASSWORD", "wrongpassXXX");
        QCOMPARE(cmdPasswd(), 1);  /* wrong password for current vault */
    }

    /* ── cmdLock / cmdUnlock ── */

    void testCmdUnlockNoVault() {
        QTemporaryDir tmp2;
        setTestHome(tmp2.path());
        QDir().mkpath(tmp2.path() + "/.config/gm-dashboard");
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdUnlock(), 1);
        setTestHome(tmpDir->path());
    }

    void testCmdUnlock() {
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        int rc = cmdUnlock();
        QCOMPARE(rc, 0);
        QVERIFY(QDir(TMPFS_DIR).exists());
    }

    void testCmdLock() {
        int rc = cmdLock();
        QCOMPARE(rc, 0);
        QVERIFY(!QDir(TMPFS_DIR).exists());
    }

    void testCmdLockAlreadyLocked() {
        QCOMPARE(cmdLock(), 0);
    }

    /* ── cmdMigrate ── */

    void testCmdMigrate() {
        QTemporaryDir tmp2;
        QVERIFY(tmp2.isValid());
        setTestHome(tmp2.path());
        QString cfgd = tmp2.path() + "/.config/gm-dashboard";
        QDir().mkpath(cfgd);

        /* Write plaintext configs */
        QFile gc(cfgd + "/config");
        QVERIFY(gc.open(QIODevice::WriteOnly));
        gc.write("# comment\nNAME=TEST\n");
        gc.close();

        QFile wc(cfgd + "/weather.conf");
        QVERIFY(wc.open(QIODevice::WriteOnly));
        wc.write("CITY=TestCity\n");
        wc.close();

        qputenv("GM_VAULT_PASSWORD", "migratepass1234");
        int rc = cmdMigrate();
        QCOMPARE(rc, 0);

        /* Verify vault was created */
        QVERIFY(QFile::exists(vaultPath()));
        bool ok;
        QJsonObject data = vaultOpen("migratepass1234", &ok);
        QVERIFY(ok);
        QCOMPARE(data["global"].toObject()["NAME"].toString(), QString("TEST"));
        QCOMPARE(data["weather"].toObject()["CITY"].toString(), QString("TestCity"));

        /* Restore */
        setTestHome(tmpDir->path());
    }

    void testCmdMigrateAlreadyExists() {
        /* tmpDir still has a vault */
        qputenv("GM_VAULT_PASSWORD", "testpass1234");
        QCOMPARE(cmdMigrate(), 1);
    }

    void testCmdMigrateNoConfigs() {
        QTemporaryDir tmp3;
        setTestHome(tmp3.path());
        QDir().mkpath(tmp3.path() + "/.config/gm-dashboard");
        qputenv("GM_VAULT_PASSWORD", "pass12345678");
        QCOMPARE(cmdMigrate(), 1);
        setTestHome(tmpDir->path());
    }

    /* ── usage() ── */

    void testUsage() {
        usage();
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        cmdLock();
        restoreHome();
        delete tmpDir;
        qunsetenv("GM_VAULT_PASSWORD");
    }
};

QTEST_MAIN(TestVault)
#include "test_vault.moc"
