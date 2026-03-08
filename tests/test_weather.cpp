/*
 * test_weather — QTest for the weather plugin v2 (Open-Meteo)
 *
 * Two test categories:
 * 1. Parsing/logic tests: include weather.cpp with GM_TESTING, use cached
 *    fixture files for deterministic results. No network calls.
 * 2. ABI/integration tests: dlopen the installed .so, one live test with
 *    QSKIP if network unavailable.
 */
#define GM_TESTING
#include "../plugins/weather.cpp"

#include <QTest>
#include <QTemporaryDir>

class TestWeather : public QObject {
    Q_OBJECT

private:
    /* Path to test fixtures directory (derived from source file location) */
    QString fixturesDir;

    QByteArray readFixture(const QString &name) {
        QFile f(fixturesDir + "/" + name);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.readAll();
    }

private slots:

    void initTestCase() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        /* Fixtures live next to this test file */
        fixturesDir = QString(__FILE__).replace("test_weather.cpp", "fixtures");
        QVERIFY2(QDir(fixturesDir).exists(),
            qPrintable("Fixtures dir not found: " + fixturesDir));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  ABI compliance (metadata from gm_info)
     * ═══════════════════════════════════════════════════════════════ */

    void testApiVersion() {
        struct gm_info info = gm_info();
        QCOMPARE(info.api_version, GM_API_VERSION);
    }

    void testPluginId() {
        QCOMPARE(QString(gm_info().id), QString("weather"));
    }

    void testPluginName() {
        QCOMPARE(QString(gm_info().name), QString("WEATHER"));
    }

    void testPluginVersion() {
        QCOMPARE(QString(gm_info().version), QString("2.0.0"));
    }

    void testPluginOrder() {
        QCOMPARE(gm_info().order, 10);
    }

    void testCapsNetwork() {
        QVERIFY(gm_info().caps_required & GM_CAP_NETWORK);
    }

    void testCapsNoExec() {
        QVERIFY(!(gm_info().caps_required & GM_CAP_EXEC));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  WMO weather code mapping
     * ═══════════════════════════════════════════════════════════════ */

    void testWmoCode0() {
        QCOMPARE(QString(wmoDesc(0)), QString("Clear Sky"));
        QCOMPARE(QString(wmoIcon(0)), QString("sun"));
    }

    void testWmoCode3() {
        QCOMPARE(QString(wmoDesc(3)), QString("Overcast"));
        QCOMPARE(QString(wmoIcon(3)), QString("cloud"));
    }

    void testWmoCode63() {
        QCOMPARE(QString(wmoDesc(63)), QString("Moderate Rain"));
        QCOMPARE(QString(wmoIcon(63)), QString("rain"));
    }

    void testWmoCode95() {
        QCOMPARE(QString(wmoDesc(95)), QString("Thunderstorm"));
        QCOMPARE(QString(wmoIcon(95)), QString("thunderstorm"));
    }

    void testWmoCodeUnknown() {
        QCOMPARE(QString(wmoDesc(999)), QString("Unknown"));
        QCOMPARE(QString(wmoIcon(999)), QString("question"));
    }

    void testWmoAllKnownCodes() {
        /* Every entry in the table must NOT return "Unknown" */
        int knownCodes[] = { 0,1,2,3,45,48,51,53,55,56,57,61,63,65,66,67,
                             71,73,75,77,80,81,82,85,86,95,96,99 };
        for (int code : knownCodes) {
            QVERIFY2(QString(wmoDesc(code)) != "Unknown",
                qPrintable(QString("Code %1 returned Unknown").arg(code)));
        }
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Wind direction label
     * ═══════════════════════════════════════════════════════════════ */

    void testWindDirN()  { QCOMPARE(QString(windDirLabel(0)),   QString("N"));  }
    void testWindDirNE() { QCOMPARE(QString(windDirLabel(45)),  QString("NE")); }
    void testWindDirE()  { QCOMPARE(QString(windDirLabel(90)),  QString("E"));  }
    void testWindDirSE() { QCOMPARE(QString(windDirLabel(135)), QString("SE")); }
    void testWindDirS()  { QCOMPARE(QString(windDirLabel(180)), QString("S"));  }
    void testWindDirSW() { QCOMPARE(QString(windDirLabel(225)), QString("SW")); }
    void testWindDirW()  { QCOMPARE(QString(windDirLabel(270)), QString("W"));  }
    void testWindDirNW() { QCOMPARE(QString(windDirLabel(315)), QString("NW")); }

    void testWindDirBoundary() {
        /* 22 degrees is still N (22+22=44, /45=0) */
        QCOMPARE(QString(windDirLabel(22)), QString("N"));
        /* 23 degrees is NE (23+22=45, /45=1) */
        QCOMPARE(QString(windDirLabel(23)), QString("NE"));
        /* 359 is N (359+22=381%360=21, /45=0) */
        QCOMPARE(QString(windDirLabel(359)), QString("N"));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Open-Meteo response parsing (cached fixtures)
     * ═══════════════════════════════════════════════════════════════ */

    void testParseOpenMeteoFull() {
        QByteArray raw = readFixture("open_meteo_current.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject r = parseOpenMeteo(raw, "Brasilia");
        QVERIFY(!r.isEmpty());

        QCOMPARE(r["city"].toString(), QString("Brasilia"));
        QVERIFY(r.contains("temp_c"));
        QVERIFY(r.contains("feels_like"));
        QVERIFY(r.contains("humidity"));
        QVERIFY(r.contains("wind_kmh"));
        QVERIFY(r.contains("wind_dir"));
        QVERIFY(r.contains("wind_dir_label"));
        QVERIFY(r.contains("pressure_hpa"));
        QVERIFY(r.contains("uv_index"));
        QVERIFY(r.contains("visibility_m"));
        QVERIFY(r.contains("weather_code"));
        QVERIFY(r.contains("desc"));
        QVERIFY(r.contains("icon"));
        QVERIFY(r.contains("is_day"));
        QVERIFY(r.contains("temp_max"));
        QVERIFY(r.contains("temp_min"));
        QVERIFY(r.contains("sunrise"));
        QVERIFY(r.contains("sunset"));

        /* Verify actual values from fixture (Brasilia 2026-03-08) */
        QCOMPARE(r["temp_c"].toInt(), 22);
        QCOMPARE(r["humidity"].toInt(), 79);
        QCOMPARE(r["feels_like"].toInt(), 24);
        QCOMPARE(r["weather_code"].toInt(), 3);
        QCOMPARE(r["desc"].toString(), QString("Overcast"));
        QCOMPARE(r["icon"].toString(), QString("cloud"));
        QCOMPARE(r["is_day"].toBool(), true);
        QCOMPARE(r["sunrise"].toString(), QString("06:14"));
        QCOMPARE(r["sunset"].toString(), QString("18:31"));
        QCOMPARE(r["temp_max"].toInt(), 28);
        QCOMPARE(r["temp_min"].toInt(), 19);

        /* 7-day forecast array */
        QVERIFY(r.contains("forecast"));
        QJsonArray fc = r["forecast"].toArray();
        QCOMPARE(fc.size(), 7);

        /* First day */
        QJsonObject day0 = fc[0].toObject();
        QVERIFY(day0.contains("date"));
        QVERIFY(day0.contains("weather_code"));
        QVERIFY(day0.contains("icon"));
        QVERIFY(day0.contains("desc"));
        QVERIFY(day0.contains("temp_max"));
        QVERIFY(day0.contains("temp_min"));
        QVERIFY(day0.contains("precip_prob"));
        QCOMPARE(day0["temp_max"].toInt(), 28);
        QCOMPARE(day0["temp_min"].toInt(), 19);

        /* Hourly sparkline data */
        QVERIFY(r.contains("hourly_yesterday"));
        QVERIFY(r.contains("hourly_today"));
        QJsonArray hYesterday = r["hourly_yesterday"].toArray();
        QJsonArray hToday = r["hourly_today"].toArray();
        QCOMPARE(hYesterday.size(), 24);
        QCOMPARE(hToday.size(), 24);
        /* First hour of today should match fixture: 19.4 */
        QVERIFY(qAbs(hToday[0].toDouble() - 19.4) < 0.1);
        /* Peak yesterday around 27.8 (hour 13) */
        QVERIFY(hYesterday[13].toDouble() > 27.0);
    }

    void testParseOpenMeteoMissingFields() {
        /* Fixture with some current fields absent */
        QByteArray raw = readFixture("open_meteo_missing.json");
        QVERIFY(!raw.isEmpty());

        QJsonObject r = parseOpenMeteo(raw, "Test");
        QVERIFY(!r.isEmpty());

        /* Required fields still present with defaults */
        QVERIFY(r.contains("temp_c"));
        QVERIFY(r.contains("weather_code"));
        /* Missing fields default to 0 */
        QCOMPARE(r["wind_kmh"].toInt(), 0);
        QCOMPARE(r["visibility_m"].toInt(), 0);
        /* No hourly data in missing fixture */
        QCOMPARE(r["hourly_yesterday"].toArray().size(), 0);
        QCOMPARE(r["hourly_today"].toArray().size(), 0);
    }

    void testParseOpenMeteoMalformed() {
        QJsonObject r = parseOpenMeteo("not json at all", "Test");
        QVERIFY(r.isEmpty());
    }

    void testParseOpenMeteoEmpty() {
        QJsonObject r = parseOpenMeteo("", "Test");
        QVERIFY(r.isEmpty());
    }

    void testParseOpenMeteoNoCurrent() {
        /* Valid JSON but no "current" key */
        QJsonObject r = parseOpenMeteo("{\"latitude\":0}", "Test");
        QVERIFY(r.isEmpty());
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Nominatim response parsing (cached fixture)
     * ═══════════════════════════════════════════════════════════════ */

    void testParseNominatimBrasilia() {
        QByteArray raw = readFixture("nominatim_brasilia.json");
        QVERIFY(!raw.isEmpty());

        double lat, lon;
        QVERIFY(parseNominatim(raw, lat, lon));

        /* Brasilia: ~-15.79, ~-47.88 */
        QVERIFY2(lat < -15.0 && lat > -16.5,
            qPrintable(QString("Lat %1 out of range").arg(lat)));
        QVERIFY2(lon < -47.0 && lon > -49.0,
            qPrintable(QString("Lon %1 out of range").arg(lon)));
    }

    void testParseNominatimEmpty() {
        double lat, lon;
        QVERIFY(!parseNominatim("[]", lat, lon));
    }

    void testParseNominatimMalformed() {
        double lat, lon;
        QVERIFY(!parseNominatim("garbage", lat, lon));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  IP geolocation response parsing (cached fixture)
     * ═══════════════════════════════════════════════════════════════ */

    void testParseIpGeoSuccess() {
        QByteArray raw = readFixture("ip_geo.json");
        QVERIFY(!raw.isEmpty());

        double lat, lon;
        QString city;
        QVERIFY(parseIpGeo(raw, lat, lon, city));
        QCOMPARE(city, QString("Brasilia"));
        QVERIFY(lat < 0);  /* Southern hemisphere */
    }

    void testParseIpGeoFail() {
        QByteArray raw = readFixture("ip_geo_fail.json");
        QVERIFY(!raw.isEmpty());

        double lat, lon;
        QString city;
        QVERIFY(!parseIpGeo(raw, lat, lon, city));
    }

    void testParseIpGeoMalformed() {
        double lat, lon;
        QString city;
        QVERIFY(!parseIpGeo("not json", lat, lon, city));
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Config parsing (gm_init)
     * ═══════════════════════════════════════════════════════════════ */

    void testConfigLatLon() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("LAT=-15.84\nLON=-48.03\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        QVERIFY(s_configured);
        QVERIFY(qAbs(s_lat - (-15.84)) < 0.01);
        QVERIFY(qAbs(s_lon - (-48.03)) < 0.01);
        QCOMPARE(s_city, QString("Custom Location"));
        gm_cleanup();
    }

    void testConfigLatLonWithCity() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("LAT=-15.84\nLON=-48.03\nCITY=Brasilia\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        QVERIFY(s_configured);
        QCOMPARE(s_city, QString("Brasilia"));
        gm_cleanup();
    }

    void testConfigCityOnly() {
        /* CITY without LAT/LON — needs Nominatim (will fail offline) */
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("CITY=TestCity\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        /* City is set regardless of geocoding result */
        QCOMPARE(s_city, QString("TestCity"));
        gm_cleanup();
    }

    void testConfigEmpty() {
        QTemporaryDir tmp;
        gm_init(qPrintable(tmp.path()));
        /* No config file — auto-detect path (may or may not work) */
        gm_cleanup();
    }

    void testConfigCommentIgnored() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("# CITY=WrongCity\nCITY=RightCity\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        QCOMPARE(s_city, QString("RightCity"));
        gm_cleanup();
    }

    void testConfigLatOnly() {
        /* LAT without LON — incomplete, should not set configured via coords */
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("LAT=-15.84\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        /* Not configured via coords (no LON), no CITY either */
        QVERIFY(!s_configured);
        gm_cleanup();
    }

    void testConfigInvalidLatLon() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("LAT=999\nLON=999\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        /* Out of range — not configured via coords */
        QVERIFY(!s_configured);
        gm_cleanup();
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Not configured state (gm_fetch)
     * ═══════════════════════════════════════════════════════════════ */

    void testNotConfiguredReturnsJson() {
        s_configured = false;
        const char *json = gm_fetch();
        QVERIFY(json);

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QCOMPARE(obj["not_configured"].toBool(), true);
        gm_cleanup();
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Live integration (network required, QSKIP if offline)
     * ═══════════════════════════════════════════════════════════════ */

    void testLiveGeocodeBrasilia() {
        /* Tests geocodeCity via real Nominatim call.
         * Nominatim may return different "Brasilia" results, so we accept
         * any coordinates in Brazil (-1 to -34 lat, -30 to -74 lon). */
        double lat, lon;
        bool ok = geocodeCity("Brasilia", lat, lon);
        if (!ok) QSKIP("Network unavailable — Nominatim unreachable");

        QVERIFY2(lat < 0 && lat > -35,
            qPrintable(QString("Lat %1 not in Brazil").arg(lat)));
        QVERIFY2(lon < -30 && lon > -75,
            qPrintable(QString("Lon %1 not in Brazil").arg(lon)));
    }

    void testLiveGeocodeCityInit() {
        /* Tests the CITY path in gm_init (geocodeCity + s_configured) */
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("CITY=Brasilia\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        if (!s_configured) QSKIP("Network unavailable — geocoding failed");

        QCOMPARE(s_city, QString("Brasilia"));
        QVERIFY(s_lat < 0 && s_lat > -35);

        const char *json = gm_fetch();
        if (!json) QSKIP("Network unavailable — Open-Meteo unreachable");

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QVERIFY(obj.contains("temp_c"));
        QCOMPARE(obj["city"].toString(), QString("Brasilia"));

        gm_cleanup();
    }

    void testLiveFetchBrasilia() {
        QTemporaryDir tmp;
        QFile conf(tmp.path() + "/weather.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("LAT=-15.84\nLON=-48.03\nCITY=Brasilia\n");
        conf.close();

        gm_init(qPrintable(tmp.path()));
        QVERIFY(s_configured);

        const char *json = gm_fetch();
        if (!json) QSKIP("Network unavailable — Open-Meteo unreachable");

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        /* All fields present */
        QVERIFY(obj.contains("temp_c"));
        QVERIFY(obj.contains("feels_like"));
        QVERIFY(obj.contains("humidity"));
        QVERIFY(obj.contains("wind_kmh"));
        QVERIFY(obj.contains("wind_dir_label"));
        QVERIFY(obj.contains("pressure_hpa"));
        QVERIFY(obj.contains("uv_index"));
        QVERIFY(obj.contains("desc"));
        QVERIFY(obj.contains("icon"));
        QVERIFY(obj.contains("sunrise"));
        QVERIFY(obj.contains("sunset"));
        QVERIFY(obj.contains("temp_max"));
        QVERIFY(obj.contains("temp_min"));

        /* Sanity ranges for Brasilia */
        int temp = obj["temp_c"].toInt();
        QVERIFY2(temp > -10 && temp < 55,
            qPrintable(QString("Temp %1 out of range").arg(temp)));

        int humidity = obj["humidity"].toInt();
        QVERIFY2(humidity >= 0 && humidity <= 100,
            qPrintable(QString("Humidity %1 out of range").arg(humidity)));

        QCOMPARE(obj["city"].toString(), QString("Brasilia"));

        gm_cleanup();
    }

    /* ═══════════════════════════════════════════════════════════════
     *  Cleanup
     * ═══════════════════════════════════════════════════════════════ */

    void cleanupTestCase() {
        gm_cleanup();
        curl_global_cleanup();
    }
};

QTEST_MAIN(TestWeather)
#include "test_weather.moc"
