/*
 * test_dashboard — QTest for the main gm-dashboard binary
 *
 * Verifies:
 * 1. Binary exists and runs
 * 2. --json output is valid JSON with correct structure
 * 3. Greeting matches current time of day
 * 4. Date field present and formatted correctly
 * 5. Timezone field present
 * 6. Plugins section present with expected plugins
 * 7. --no-browser generates HTML file
 * 8. HTML file contains expected markers
 * 9. --help works
 */
#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTime>

class TestDashboard : public QObject {
    Q_OBJECT

private:
    QString dashBin;

    QJsonObject runJson() {
        QProcess proc;
        proc.start(dashBin, {"--json"});
        if (!proc.waitForFinished(90000))
            return {};
        if (proc.exitCode() != 0)
            return {};
        QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
        return doc.object();
    }

private slots:

    void initTestCase() {
        dashBin = QDir::homePath() + "/.local/bin/gm-dashboard";
        QVERIFY2(QFile::exists(dashBin), "gm-dashboard not installed");
    }

    /* ── CLI ── */

    void testHelp() {
        QProcess proc;
        proc.start(dashBin, {"--help"});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 0);

        QByteArray out = proc.readAllStandardOutput();
        QVERIFY2(out.contains("Usage:"), "Help doesn't show usage");
    }

    /* ── JSON output structure ── */

    void testJsonValid() {
        QProcess proc;
        proc.start(dashBin, {"--json"});
        QVERIFY2(proc.waitForFinished(90000),
            "gm-dashboard --json timed out (90s)");
        QCOMPARE(proc.exitCode(), 0);

        QByteArray out = proc.readAllStandardOutput();
        QVERIFY2(!out.isEmpty(), "Empty stdout");

        QJsonDocument doc = QJsonDocument::fromJson(out);
        QVERIFY2(doc.isObject(), "Output is not valid JSON object");
    }

    void testGreetingField() {
        QJsonObject root = runJson();
        QVERIFY2(!root.isEmpty(), "Failed to get JSON output");

        QVERIFY2(root.contains("greeting"), "Missing greeting field");

        QString greeting = root["greeting"].toString();
        QStringList valid = {
            "GOOD MORNING", "GOOD AFTERNOON",
            "GOOD EVENING", "GOOD NIGHT"
        };
        QVERIFY2(valid.contains(greeting),
            qPrintable(QString("Invalid greeting: %1").arg(greeting)));
    }

    void testGreetingMatchesTime() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QString greeting = root["greeting"].toString();
        int hour = QTime::currentTime().hour();

        QString expected;
        if (hour >= 5  && hour < 12) expected = "GOOD MORNING";
        else if (hour >= 12 && hour < 18) expected = "GOOD AFTERNOON";
        else if (hour >= 18 && hour < 23) expected = "GOOD EVENING";
        else expected = "GOOD NIGHT";

        QCOMPARE(greeting, expected);
    }

    void testDateField() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QVERIFY2(root.contains("date"), "Missing date field");

        QString date = root["date"].toString();
        QVERIFY2(!date.isEmpty(), "date is empty");
        /* Must be all uppercase */
        QCOMPARE(date, date.toUpper());
        /* Must contain current year */
        QVERIFY2(date.contains("2026") || date.contains("2027"),
            qPrintable(QString("Date missing year: %1").arg(date)));
    }

    void testTimezoneField() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QVERIFY2(root.contains("timezone"), "Missing timezone field");
        QVERIFY2(!root["timezone"].toString().isEmpty(), "timezone is empty");
    }

    void testPluginsSection() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QVERIFY2(root.contains("plugins"), "Missing plugins section");
        QVERIFY2(root["plugins"].isObject(), "plugins is not an object");

        QJsonObject plugins = root["plugins"].toObject();
        QVERIFY2(!plugins.isEmpty(), "plugins section is empty");
    }

    void testWeatherPluginPresent() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QJsonObject plugins = root["plugins"].toObject();
        QVERIFY2(plugins.contains("weather"), "weather plugin missing from output");
    }

    void testGithubPluginPresent() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QJsonObject plugins = root["plugins"].toObject();
        QVERIFY2(plugins.contains("github"), "github plugin missing from output");
    }

    void testSystemPluginPresent() {
        QJsonObject root = runJson();
        if (root.isEmpty()) QSKIP("Failed to get JSON");

        QJsonObject plugins = root["plugins"].toObject();
        QVERIFY2(plugins.contains("system"), "system plugin missing from output");
    }

    /* ── HTML generation ── */

    void testNoBrowserGeneratesHtml() {
        QProcess proc;
        proc.start(dashBin, {"--no-browser"});
        QVERIFY2(proc.waitForFinished(90000),
            "gm-dashboard --no-browser timed out");
        QCOMPARE(proc.exitCode(), 0);

        QVERIFY2(QFile::exists("/dev/shm/gm-dashboard.html"),
            "HTML file not generated");

        QFile f("/dev/shm/gm-dashboard.html");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray html = f.readAll();

        QVERIFY2(html.size() > 1000,
            "HTML file too small — likely empty or corrupt");
    }

    void testHtmlContainsCards() {
        QFile f("/dev/shm/gm-dashboard.html");
        if (!f.exists()) QSKIP("No HTML file");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray html = f.readAll();

        QVERIFY2(html.contains("data-plugin=\"weather\""),
            "HTML missing weather card");
        QVERIFY2(html.contains("data-plugin=\"github\""),
            "HTML missing github card");
        QVERIFY2(html.contains("data-plugin=\"system\""),
            "HTML missing system card");
    }

    void testHtmlContainsGmData() {
        QFile f("/dev/shm/gm-dashboard.html");
        if (!f.exists()) QSKIP("No HTML file");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray html = f.readAll();

        QVERIFY2(html.contains("\"greeting\""),
            "HTML missing GM data JSON");
        QVERIFY2(html.contains("\"plugins\""),
            "HTML missing plugins in GM data");
    }

    void testHtmlContainsBridge() {
        QFile f("/dev/shm/gm-dashboard.html");
        if (!f.exists()) QSKIP("No HTML file");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray html = f.readAll();

        QVERIFY2(html.contains("qrc:///qtwebchannel/qwebchannel.js"),
            "HTML missing QWebChannel bridge script");
    }
};

QTEST_MAIN(TestDashboard)
#include "test_dashboard.moc"
