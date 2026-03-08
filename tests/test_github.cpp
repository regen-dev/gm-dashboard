/*
 * test_github — QTest for the GitHub plugin
 *
 * Verifies:
 * 1. Plugin ABI compliance (symbols, metadata, caps)
 * 2. Fetch returns valid JSON with expected fields
 * 3. Numeric fields are sane
 * 4. Login/name fields are non-empty
 *
 * Note: Requires gh CLI configured with auth.
 *       Tests will SKIP (not fail) if gh is not available.
 */
#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <dlfcn.h>
#include "../gm-plugin.h"

class TestGithub : public QObject {
    Q_OBJECT

private:
    void *handle = nullptr;
    gm_info_fn  infoFn  = nullptr;
    gm_init_fn  initFn  = nullptr;
    gm_fetch_fn fetchFn = nullptr;
    gm_cleanup_fn cleanFn = nullptr;
    bool ghAvailable = false;

    QString pluginPath() {
        return QDir::homePath() + "/.local/lib/gm-dashboard/github.so";
    }

private slots:

    void initTestCase() {
        QVERIFY2(QFile::exists(pluginPath()),
            "github.so not installed — run 'make install' first");

        /* Check if gh CLI is available and authenticated */
        QProcess gh;
        gh.start("gh", {"auth", "status"});
        if (gh.waitForFinished(5000) && gh.exitCode() == 0)
            ghAvailable = true;
        else
            qDebug() << "gh CLI not authenticated — network tests will skip";

        handle = dlopen(qPrintable(pluginPath()), RTLD_LAZY);
        QVERIFY2(handle, qPrintable(QString("dlopen: %1").arg(dlerror())));

        infoFn  = reinterpret_cast<gm_info_fn>(dlsym(handle, "gm_info"));
        initFn  = reinterpret_cast<gm_init_fn>(dlsym(handle, "gm_init"));
        fetchFn = reinterpret_cast<gm_fetch_fn>(dlsym(handle, "gm_fetch"));
        cleanFn = reinterpret_cast<gm_cleanup_fn>(dlsym(handle, "gm_cleanup"));

        QVERIFY2(infoFn && initFn && fetchFn && cleanFn,
            "Missing plugin ABI symbols");
    }

    /* ── ABI ── */

    void testApiVersion() {
        struct gm_info info = infoFn();
        QCOMPARE(info.api_version, GM_API_VERSION);
    }

    void testPluginId() {
        struct gm_info info = infoFn();
        QCOMPARE(QString(info.id), QString("github"));
    }

    void testPluginName() {
        struct gm_info info = infoFn();
        QCOMPARE(QString(info.name), QString("GITHUB"));
    }

    void testPluginOrder() {
        struct gm_info info = infoFn();
        QCOMPARE(info.order, 20);
    }

    void testCapsExec() {
        struct gm_info info = infoFn();
        QVERIFY2(info.caps_required & GM_CAP_EXEC,
            "GitHub plugin must require GM_CAP_EXEC (for gh CLI)");
    }

    void testCapsNetwork() {
        struct gm_info info = infoFn();
        QVERIFY2(info.caps_required & GM_CAP_NETWORK,
            "GitHub plugin must require GM_CAP_NETWORK");
    }

    /* ── Init ── */

    void testInitSucceeds() {
        QCOMPARE(initFn("/tmp"), 0);
    }

    /* ── Fetch (requires gh CLI auth) ── */

    void testFetchReturnsJson() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY2(json, "gm_fetch returned null");

        QJsonDocument doc = QJsonDocument::fromJson(json);
        QVERIFY2(doc.isObject(), "Response is not a JSON object");
        cleanFn();
    }

    void testFetchFields() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        initFn("/tmp");
        const char *json = fetchFn();
        if (!json) QSKIP("gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        QVERIFY2(obj.contains("login"),         "Missing login");
        QVERIFY2(obj.contains("name"),          "Missing name");
        QVERIFY2(obj.contains("followers"),     "Missing followers");
        QVERIFY2(obj.contains("following"),     "Missing following");
        QVERIFY2(obj.contains("repos"),         "Missing repos");
        QVERIFY2(obj.contains("stars"),         "Missing stars");
        QVERIFY2(obj.contains("forks"),         "Missing forks");
        QVERIFY2(obj.contains("contributions"), "Missing contributions");

        cleanFn();
    }

    void testLoginNotEmpty() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        initFn("/tmp");
        const char *json = fetchFn();
        if (!json) QSKIP("gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        QString login = obj["login"].toString();
        QVERIFY2(!login.isEmpty(), "login is empty");

        qDebug() << "GitHub login:" << login;
        cleanFn();
    }

    void testNumericFieldsSanity() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        initFn("/tmp");
        const char *json = fetchFn();
        if (!json) QSKIP("gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        int repos = obj["repos"].toInt();
        QVERIFY2(repos >= 0, qPrintable(QString("repos=%1 negative").arg(repos)));

        int stars = obj["stars"].toInt();
        QVERIFY2(stars >= 0, qPrintable(QString("stars=%1 negative").arg(stars)));

        int followers = obj["followers"].toInt();
        QVERIFY2(followers >= 0, qPrintable(QString("followers=%1 negative").arg(followers)));

        int contributions = obj["contributions"].toInt();
        QVERIFY2(contributions >= 0,
            qPrintable(QString("contributions=%1 negative").arg(contributions)));

        cleanFn();
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        if (handle) dlclose(handle);
    }
};

QTEST_MAIN(TestGithub)
#include "test_github.moc"
