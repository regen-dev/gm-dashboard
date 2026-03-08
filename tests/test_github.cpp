/*
 * test_github — QTest for the GitHub plugin v2
 *
 * Two test categories:
 * 1. Parsing/logic tests: include github.cpp with GM_TESTING, use cached
 *    fixture files for deterministic results. No network calls.
 * 2. Live integration tests: call gm_fetch with real gh CLI, QSKIP if
 *    gh is not available/authenticated.
 */
#define GM_TESTING
#include "../plugins/github.cpp"

#include <QTest>
#include <QTemporaryDir>
#include <QProcess>

class TestGithub : public QObject {
    Q_OBJECT

private:
    /* Path to test fixtures directory (derived from source file location) */
    QString fixturesDir;
    bool ghAvailable = false;

    QByteArray readFixture(const QString &name) {
        QFile f(fixturesDir + "/" + name);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.readAll();
    }

private slots:

    void initTestCase() {
        /* Fixtures live next to this test file */
        fixturesDir = QString(__FILE__).replace("test_github.cpp", "fixtures");
        QVERIFY2(QDir(fixturesDir).exists(),
            qPrintable("Fixtures dir not found: " + fixturesDir));

        /* Check if gh CLI is available and authenticated */
        QProcess gh;
        gh.start("gh", {"auth", "status"});
        if (gh.waitForFinished(5000) && gh.exitCode() == 0)
            ghAvailable = true;
        else
            qDebug() << "gh CLI not authenticated — live tests will skip";
    }

    /* ═══════════════════════════════════════════════════════════════
     *  ABI metadata
     * ═══════════════════════════════════════════════════════════════ */

    void testApiVersion() {
        struct gm_info info = gm_info();
        QCOMPARE(info.api_version, GM_API_VERSION);
    }

    void testPluginId() {
        struct gm_info info = gm_info();
        QCOMPARE(QString(info.id), QString("github"));
    }

    void testPluginName() {
        struct gm_info info = gm_info();
        QCOMPARE(QString(info.name), QString("GITHUB"));
    }

    void testPluginOrder() {
        struct gm_info info = gm_info();
        QCOMPARE(info.order, 20);
    }

    void testCapsExec() {
        struct gm_info info = gm_info();
        QVERIFY2(info.caps_required & GM_CAP_EXEC,
            "GitHub plugin must require GM_CAP_EXEC (for gh CLI)");
    }

    void testCapsNetwork() {
        struct gm_info info = gm_info();
        QVERIFY2(info.caps_required & GM_CAP_NETWORK,
            "GitHub plugin must require GM_CAP_NETWORK");
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Repo name validation
     * ═══════════════════════════════════════════════════════════════ */

    void testValidRepoSimple() {
        QVERIFY(isValidRepo("torvalds/linux"));
    }

    void testValidRepoWithDots() {
        QVERIFY(isValidRepo("owner/my.repo.name"));
    }

    void testValidRepoWithUnderscore() {
        QVERIFY(isValidRepo("owner/my_repo"));
    }

    void testValidRepoWithHyphen() {
        QVERIFY(isValidRepo("my-org/my-repo"));
    }

    void testInvalidRepoNoSlash() {
        QVERIFY(!isValidRepo("noslash"));
    }

    void testInvalidRepoEmpty() {
        QVERIFY(!isValidRepo(""));
    }

    void testInvalidRepoDoubleSlash() {
        QVERIFY(!isValidRepo("a/b/c"));
    }

    void testInvalidRepoSpecialChars() {
        QVERIFY(!isValidRepo("owner/repo name"));
        QVERIFY(!isValidRepo("owner/repo;rm"));
        QVERIFY(!isValidRepo("owner/repo'inject"));
    }

    void testInvalidRepoStartsWithHyphen() {
        QVERIFY(!isValidRepo("-owner/repo"));
    }

    void testInvalidRepoTooLong() {
        QVERIFY(!isValidRepo(QString("a").repeated(71) + "/" + QString("b").repeated(71)));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Config parsing (gm_init)
     * ═══════════════════════════════════════════════════════════════ */

    void testInitNoConfig() {
        QTemporaryDir tmp;
        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QVERIFY(s_repos.isEmpty());
        QVERIFY(s_showProfile);
        gm_cleanup();
    }

    void testInitEmptyConfig() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QVERIFY(s_repos.isEmpty());
        QVERIFY(s_showProfile);
        gm_cleanup();
    }

    void testInitWithRepos() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("REPO=anthropics/claude-code\nREPO=torvalds/linux\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 2);
        QCOMPARE(s_repos[0], QString("anthropics/claude-code"));
        QCOMPARE(s_repos[1], QString("torvalds/linux"));
        gm_cleanup();
    }

    void testInitShowProfileDefault() {
        QTemporaryDir tmp;
        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QVERIFY(s_showProfile);
        gm_cleanup();
    }

    void testInitShowProfileFalse() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("SHOW_PROFILE=false\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QVERIFY(!s_showProfile);
        gm_cleanup();
    }

    void testInitShowProfileZero() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("SHOW_PROFILE=0\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QVERIFY(!s_showProfile);
        gm_cleanup();
    }

    void testInitIgnoresComments() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("# This is a comment\n");
        conf.write("REPO=valid/repo\n");
        conf.write("# REPO=commented/out\n");
        conf.write("\n");
        conf.write("REPO=another/repo\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 2);
        QCOMPARE(s_repos[0], QString("valid/repo"));
        QCOMPARE(s_repos[1], QString("another/repo"));
        gm_cleanup();
    }

    void testInitIgnoresInvalidRepos() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("REPO=valid/repo\n");
        conf.write("REPO=noslash\n");
        conf.write("REPO=\n");
        conf.write("REPO=has spaces/bad\n");
        conf.write("REPO=ok/valid-too\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 2);
        QCOMPARE(s_repos[0], QString("valid/repo"));
        QCOMPARE(s_repos[1], QString("ok/valid-too"));
        gm_cleanup();
    }

    void testInitDeduplicatesRepos() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("REPO=torvalds/linux\n");
        conf.write("REPO=torvalds/linux\n");
        conf.write("REPO=other/repo\n");
        conf.write("REPO=torvalds/linux\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 2);
        QCOMPARE(s_repos[0], QString("torvalds/linux"));
        QCOMPARE(s_repos[1], QString("other/repo"));
        gm_cleanup();
    }

    void testInitIgnoresLinesNoEquals() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("garbage line\n");
        conf.write("REPO=valid/repo\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 1);
        gm_cleanup();
    }

    void testInitTrimsWhitespace() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("REPO= torvalds/linux \n");
        conf.write("REPO=  anthropics/claude-code\t\n");
        conf.close();

        QCOMPARE(gm_init(qPrintable(tmp.path())), 0);
        QCOMPARE(s_repos.size(), 2);
        QCOMPARE(s_repos[0], QString("torvalds/linux"));
        QCOMPARE(s_repos[1], QString("anthropics/claude-code"));
        gm_cleanup();
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Query building
     * ═══════════════════════════════════════════════════════════════ */

    void testBuildQueryProfileOnly() {
        QString q = buildQuery({}, true);
        QVERIFY(q.contains("viewer"));
        QVERIFY(!q.contains("r0:"));
    }

    void testBuildQueryReposOnly() {
        QStringList repos = {"torvalds/linux"};
        QString q = buildQuery(repos, false);
        QVERIFY(!q.contains("viewer"));
        QVERIFY(q.contains("r0: repository(owner:\"torvalds\",name:\"linux\")"));
    }

    void testBuildQueryBoth() {
        QStringList repos = {"anthropics/claude-code", "torvalds/linux"};
        QString q = buildQuery(repos, true);
        QVERIFY(q.contains("viewer"));
        QVERIFY(q.contains("r0: repository(owner:\"anthropics\",name:\"claude-code\")"));
        QVERIFY(q.contains("r1: repository(owner:\"torvalds\",name:\"linux\")"));
    }

    void testBuildQueryFields() {
        QStringList repos = {"owner/repo"};
        QString q = buildQuery(repos, false);
        QVERIFY(q.contains("stargazerCount"));
        QVERIFY(q.contains("forkCount"));
        QVERIFY(q.contains("watchers { totalCount }"));
        QVERIFY(q.contains("issues(states:OPEN) { totalCount }"));
        QVERIFY(q.contains("description"));
        QVERIFY(q.contains("nameWithOwner"));
    }

    void testBuildQueryEmpty() {
        QString q = buildQuery({}, false);
        QCOMPARE(q, QString("{ }"));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Profile parsing (cached fixtures)
     * ═══════════════════════════════════════════════════════════════ */

    void testParseProfileFull() {
        QByteArray raw = readFixture("github_graphql_tracked.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject data = QJsonDocument::fromJson(raw)["data"].toObject();
        QJsonObject profile = parseProfile(data["viewer"].toObject());

        QCOMPARE(profile["login"].toString(), QString("testuser"));
        QCOMPARE(profile["name"].toString(), QString("Test User"));
        QCOMPARE(profile["followers"].toInt(), 42);
        QCOMPARE(profile["following"].toInt(), 15);
        QCOMPARE(profile["repos"].toInt(), 30);
        QCOMPARE(profile["stars"].toInt(), 175);  /* 100 + 50 + 25 */
        QCOMPARE(profile["forks"].toInt(), 17);   /* 10 + 5 + 2 */
        QCOMPARE(profile["contributions"].toInt(), 1234);
    }

    void testParseProfileEmpty() {
        QJsonObject profile = parseProfile({});
        QVERIFY(profile.isEmpty());
    }

    void testParseProfileNoRepos() {
        QByteArray raw = readFixture("github_graphql_partial.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject data = QJsonDocument::fromJson(raw)["data"].toObject();
        QJsonObject profile = parseProfile(data["viewer"].toObject());

        QCOMPARE(profile["repos"].toInt(), 10);
        QCOMPARE(profile["stars"].toInt(), 0);  /* empty nodes array */
        QCOMPARE(profile["forks"].toInt(), 0);
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Tracked repos parsing (cached fixtures)
     * ═══════════════════════════════════════════════════════════════ */

    void testParseTrackedFull() {
        QByteArray raw = readFixture("github_graphql_tracked.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject data = QJsonDocument::fromJson(raw)["data"].toObject();
        QStringList repos = {"anthropics/claude-code", "torvalds/linux"};
        QJsonArray tracked = parseTracked(data, repos);

        QCOMPARE(tracked.size(), 2);

        QJsonObject r0 = tracked[0].toObject();
        QCOMPARE(r0["repo"].toString(), QString("anthropics/claude-code"));
        QCOMPARE(r0["stars"].toInt(), 45200);
        QCOMPARE(r0["forks"].toInt(), 1800);
        QCOMPARE(r0["watchers"].toInt(), 523);
        QCOMPARE(r0["issues"].toInt(), 150);
        QCOMPARE(r0["description"].toString(), QString("An agentic coding tool"));

        QJsonObject r1 = tracked[1].toObject();
        QCOMPARE(r1["repo"].toString(), QString("torvalds/linux"));
        QCOMPARE(r1["stars"].toInt(), 198000);
        QCOMPARE(r1["forks"].toInt(), 55800);
        QCOMPARE(r1["watchers"].toInt(), 8100);
        QCOMPARE(r1["issues"].toInt(), 300);
    }

    void testParseTrackedPartial() {
        QByteArray raw = readFixture("github_graphql_partial.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject data = QJsonDocument::fromJson(raw)["data"].toObject();
        QStringList repos = {"real-owner/real-repo", "nonexist/repo"};
        QJsonArray tracked = parseTracked(data, repos);

        /* r1 is null in fixture — should be skipped */
        QCOMPARE(tracked.size(), 1);
        QCOMPARE(tracked[0].toObject()["repo"].toString(),
            QString("real-owner/real-repo"));
        QCOMPARE(tracked[0].toObject()["stars"].toInt(), 500);
    }

    void testParseTrackedEmpty() {
        QJsonObject data;
        QStringList repos = {"a/b"};
        QJsonArray tracked = parseTracked(data, repos);
        QCOMPARE(tracked.size(), 0);
    }

    void testParseTrackedNoRepos() {
        QByteArray raw = readFixture("github_graphql_tracked.json");
        QJsonObject data = QJsonDocument::fromJson(raw)["data"].toObject();
        QJsonArray tracked = parseTracked(data, {});
        QCOMPARE(tracked.size(), 0);
    }

    /* ═══════════════════════════════════════════════════════════════
     *  gm_fetch return null when nothing to do
     * ═══════════════════════════════════════════════════════════════ */

    void testFetchNullWhenBothDisabled() {
        s_showProfile = false;
        s_repos.clear();
        const char *json = gm_fetch();
        QVERIFY2(!json, "gm_fetch should return null when profile=off and no repos");
        gm_cleanup();
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Live integration (network required, QSKIP if offline)
     * ═══════════════════════════════════════════════════════════════ */

    void testLiveFetchProfileOnly() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        QTemporaryDir tmp;
        gm_init(qPrintable(tmp.path()));
        const char *json = gm_fetch();
        QVERIFY2(json, "gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        /* Profile fields present */
        QVERIFY(obj.contains("login"));
        QVERIFY(obj.contains("name"));
        QVERIFY(obj.contains("followers"));
        QVERIFY(obj.contains("following"));
        QVERIFY(obj.contains("repos"));
        QVERIFY(obj.contains("stars"));
        QVERIFY(obj.contains("forks"));
        QVERIFY(obj.contains("contributions"));
        QVERIFY(obj.contains("show_profile"));
        QCOMPARE(obj["show_profile"].toBool(), true);

        /* Tracked should be empty (no REPO lines) */
        QVERIFY(obj.contains("tracked"));
        QCOMPARE(obj["tracked"].toArray().size(), 0);

        QString login = obj["login"].toString();
        QVERIFY2(!login.isEmpty(), "login is empty");
        qDebug() << "GitHub login:" << login;

        gm_cleanup();
    }

    void testLiveFetchWithTrackedRepo() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("REPO=torvalds/linux\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        const char *json = gm_fetch();
        QVERIFY2(json, "gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        /* Profile still present */
        QVERIFY(obj.contains("login"));

        /* Tracked repo present */
        QJsonArray tracked = obj["tracked"].toArray();
        QCOMPARE(tracked.size(), 1);

        QJsonObject repo = tracked[0].toObject();
        QCOMPARE(repo["repo"].toString(), QString("torvalds/linux"));
        QVERIFY2(repo["stars"].toInt() > 0,
            qPrintable(QString("stars=%1").arg(repo["stars"].toInt())));
        QVERIFY2(repo["forks"].toInt() > 0,
            qPrintable(QString("forks=%1").arg(repo["forks"].toInt())));
        QVERIFY(repo["watchers"].toInt() >= 0);
        QVERIFY(repo["issues"].toInt() >= 0);

        gm_cleanup();
    }

    void testLiveNumericFieldsSanity() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        QTemporaryDir tmp;
        gm_init(qPrintable(tmp.path()));
        const char *json = gm_fetch();
        if (!json) QSKIP("gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QVERIFY(obj["repos"].toInt() >= 0);
        QVERIFY(obj["stars"].toInt() >= 0);
        QVERIFY(obj["followers"].toInt() >= 0);
        QVERIFY(obj["contributions"].toInt() >= 0);

        gm_cleanup();
    }

    void testLiveFetchNoProfileWithRepo() {
        if (!ghAvailable) QSKIP("gh CLI not authenticated");

        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/github.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("SHOW_PROFILE=false\nREPO=torvalds/linux\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        const char *json = gm_fetch();
        QVERIFY2(json, "gm_fetch returned null");

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QCOMPARE(obj["show_profile"].toBool(), false);

        /* Profile fields should NOT be present */
        QVERIFY(!obj.contains("login"));
        QVERIFY(!obj.contains("repos"));

        /* Tracked repos should still work */
        QJsonArray tracked = obj["tracked"].toArray();
        QCOMPARE(tracked.size(), 1);
        QCOMPARE(tracked[0].toObject()["repo"].toString(), QString("torvalds/linux"));

        gm_cleanup();
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        gm_cleanup();
    }
};

QTEST_MAIN(TestGithub)
#include "test_github.moc"
