/*
 * test_sign — QTest for the Ed25519 signing system
 *
 * Verifies:
 * 1. Keypair generation produces valid keys
 * 2. Sign + verify round-trip
 * 3. Tampered data is rejected
 * 4. Wrong key is rejected
 * 5. Corrupted signature is rejected
 * 6. Empty data can be signed
 * 7. CLI binary exists and works
 * 8. CLI keygen → sign → verify cycle
 */
#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <sodium.h>

class TestSign : public QObject {
    Q_OBJECT

private slots:

    void initTestCase() {
        QVERIFY(sodium_init() >= 0);
    }

    /* ── Core crypto ── */

    void testKeypairGeneration() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QByteArray pkBytes(reinterpret_cast<char*>(pk), sizeof(pk));
        QByteArray skBytes(reinterpret_cast<char*>(sk), sizeof(sk));

        QVERIFY(pkBytes != QByteArray(sizeof(pk), '\0'));
        QVERIFY(skBytes != QByteArray(sizeof(sk), '\0'));

        /* Key sizes */
        QCOMPARE(static_cast<int>(sizeof(pk)), static_cast<int>(crypto_sign_PUBLICKEYBYTES));
        QCOMPARE(static_cast<int>(sizeof(sk)), static_cast<int>(crypto_sign_SECRETKEYBYTES));
    }

    void testSignAndVerify() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QByteArray data("Hello, gm-dashboard!");
        unsigned char sig[crypto_sign_BYTES];

        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), sk);

        int ok = crypto_sign_verify_detached(sig,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), pk);
        QCOMPARE(ok, 0);
    }

    void testRejectTamperedData() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QByteArray data("Original data");
        unsigned char sig[crypto_sign_BYTES];

        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), sk);

        /* Tamper */
        QByteArray tampered("Tampered data");
        int ok = crypto_sign_verify_detached(sig,
            reinterpret_cast<const unsigned char*>(tampered.constData()),
            tampered.size(), pk);
        QVERIFY(ok != 0);
    }

    void testRejectWrongKey() {
        unsigned char pk1[crypto_sign_PUBLICKEYBYTES], sk1[crypto_sign_SECRETKEYBYTES];
        unsigned char pk2[crypto_sign_PUBLICKEYBYTES], sk2[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk1, sk1);
        crypto_sign_keypair(pk2, sk2);

        QByteArray data("Signed with key 1");
        unsigned char sig[crypto_sign_BYTES];

        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), sk1);

        /* Verify with wrong key */
        int ok = crypto_sign_verify_detached(sig,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), pk2);
        QVERIFY(ok != 0);
    }

    void testRejectCorruptedSignature() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        QByteArray data("Test data");
        unsigned char sig[crypto_sign_BYTES];

        crypto_sign_detached(sig, nullptr,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), sk);

        /* Corrupt one byte */
        sig[0] ^= 0xFF;

        int ok = crypto_sign_verify_detached(sig,
            reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), pk);
        QVERIFY(ok != 0);
    }

    void testEmptyDataSign() {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);

        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr, nullptr, 0, sk);

        int ok = crypto_sign_verify_detached(sig, nullptr, 0, pk);
        QCOMPARE(ok, 0);
    }

    /* ── CLI binary ── */

    void testSignBinaryExists() {
        QString bin = QDir::homePath() + "/.local/bin/gm-sign";
        QVERIFY(QFile::exists(bin));
    }

    void testSignUsage() {
        QProcess proc;
        proc.start(QDir::homePath() + "/.local/bin/gm-sign", {});
        QVERIFY(proc.waitForFinished(5000));
        QVERIFY(proc.exitCode() != 0);

        QByteArray err = proc.readAllStandardError();
        QVERIFY(err.contains("Usage:") || err.contains("keygen"));
    }

    /* ── Full CLI cycle: keygen → sign → verify ── */

    void testCliKegenSignVerify() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        QString bin = QDir::homePath() + "/.local/bin/gm-sign";
        QString cfgDir = tmp.path() + "/.config/gm-dashboard";

        QDir().mkpath(cfgDir);

        /* Set HOME so gm-sign uses our temp dir */
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", tmp.path());

        /* keygen */
        QProcess keygen;
        keygen.setProcessEnvironment(env);
        keygen.start(bin, {"keygen"});
        QVERIFY(keygen.waitForFinished(10000));
        QCOMPARE(keygen.exitCode(), 0);

        /* Check keys exist */
        QVERIFY(QFile::exists(cfgDir + "/marketplace.sec"));
        QVERIFY(QFile::exists(cfgDir + "/marketplace.pub"));

        QFile secFile(cfgDir + "/marketplace.sec");
        secFile.open(QIODevice::ReadOnly);
        QCOMPARE(secFile.size(), static_cast<qint64>(crypto_sign_SECRETKEYBYTES));

        QFile pubFile(cfgDir + "/marketplace.pub");
        pubFile.open(QIODevice::ReadOnly);
        QCOMPARE(pubFile.size(), static_cast<qint64>(crypto_sign_PUBLICKEYBYTES));

        /* Create a test file to sign */
        QString testFile = tmp.path() + "/test-plugin.so";
        QFile f(testFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("fake plugin binary data for signing test");
        f.close();

        /* sign */
        QProcess sign;
        sign.setProcessEnvironment(env);
        sign.start(bin, {"sign", testFile});
        QVERIFY(sign.waitForFinished(10000));
        QCOMPARE(sign.exitCode(), 0);
        QVERIFY(QFile::exists(testFile + ".sig"));

        QFile sigFile(testFile + ".sig");
        sigFile.open(QIODevice::ReadOnly);
        QCOMPARE(sigFile.size(), static_cast<qint64>(crypto_sign_BYTES));

        /* verify */
        QProcess verify;
        verify.setProcessEnvironment(env);
        verify.start(bin, {"verify", testFile});
        QVERIFY(verify.waitForFinished(10000));
        QCOMPARE(verify.exitCode(), 0);

        QByteArray verifyErr = verify.readAllStandardError();
        QVERIFY(verifyErr.contains("VALID"));

        /* Tamper and verify should fail */
        QFile tf(testFile);
        QVERIFY(tf.open(QIODevice::ReadWrite));
        QByteArray content = tf.readAll();
        content[0] = content[0] ^ 0xFF;
        tf.seek(0);
        tf.write(content);
        tf.close();

        QProcess verifyBad;
        verifyBad.setProcessEnvironment(env);
        verifyBad.start(bin, {"verify", testFile});
        QVERIFY(verifyBad.waitForFinished(10000));
        QVERIFY(verifyBad.exitCode() != 0);
    }
};

QTEST_MAIN(TestSign)
#include "test_sign.moc"
