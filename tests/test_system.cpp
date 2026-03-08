/*
 * test_system — QTest for the system stats plugin
 *
 * Verifies:
 * 1. Plugin ABI compliance (symbols, metadata, caps)
 * 2. Fetch returns valid JSON
 * 3. Uptime field present and sane
 * 4. Load average present and formatted
 * 5. RAM fields present with sane values
 * 6. Thermal data (if /dev/shm/argos-thermal exists)
 */
#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <dlfcn.h>
#include "../gm-plugin.h"

class TestSystem : public QObject {
    Q_OBJECT

private:
    void *handle = nullptr;
    gm_info_fn  infoFn  = nullptr;
    gm_init_fn  initFn  = nullptr;
    gm_fetch_fn fetchFn = nullptr;
    gm_cleanup_fn cleanFn = nullptr;

    QString pluginPath() {
        return QDir::homePath() + "/.local/lib/gm-dashboard/system.so";
    }

private slots:

    void initTestCase() {
        QVERIFY2(QFile::exists(pluginPath()),
            "system.so not installed — run 'make install' first");

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
        QCOMPARE(QString(info.id), QString("system"));
    }

    void testPluginName() {
        struct gm_info info = infoFn();
        QCOMPARE(QString(info.name), QString("SYSTEM"));
    }

    void testPluginOrder() {
        struct gm_info info = infoFn();
        QCOMPARE(info.order, 90);
    }

    void testCapsProc() {
        struct gm_info info = infoFn();
        QVERIFY(info.caps_required & GM_CAP_PROC);
    }

    void testCapsShm() {
        struct gm_info info = infoFn();
        QVERIFY(info.caps_required & GM_CAP_SHM);
    }

    void testCapsNoNetwork() {
        struct gm_info info = infoFn();
        QVERIFY2(!(info.caps_required & GM_CAP_NETWORK),
            "System plugin should not need network");
    }

    void testCapsNoExec() {
        struct gm_info info = infoFn();
        QVERIFY2(!(info.caps_required & GM_CAP_EXEC),
            "System plugin should not need exec");
    }

    /* ── Init ── */

    void testInitSucceeds() {
        QCOMPARE(initFn("/tmp"), 0);
    }

    /* ── Fetch: JSON structure ── */

    void testFetchReturnsJson() {
        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY2(json, "gm_fetch returned null");

        QJsonDocument doc = QJsonDocument::fromJson(json);
        QVERIFY2(doc.isObject(), "Response is not a JSON object");
        cleanFn();
    }

    void testUptimePresent() {
        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY(json);

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QVERIFY2(obj.contains("uptime"), "Missing uptime field");

        QString uptime = obj["uptime"].toString();
        QVERIFY2(uptime.contains("d") && uptime.contains("h") && uptime.contains("m"),
            qPrintable(QString("Uptime format wrong: %1").arg(uptime)));
        cleanFn();
    }

    void testLoadPresent() {
        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY(json);

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QVERIFY2(obj.contains("load"), "Missing load field");

        QString load = obj["load"].toString();
        QVERIFY2(load.contains("/"),
            qPrintable(QString("Load format wrong: %1").arg(load)));
        cleanFn();
    }

    void testRamFields() {
        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY(json);

        QJsonObject obj = QJsonDocument::fromJson(json).object();
        QVERIFY2(obj.contains("ram_total_gb"), "Missing ram_total_gb");
        QVERIFY2(obj.contains("ram_used_gb"), "Missing ram_used_gb");

        double total = obj["ram_total_gb"].toDouble();
        double used  = obj["ram_used_gb"].toDouble();

        QVERIFY2(total > 0,
            qPrintable(QString("RAM total %1 GB invalid").arg(total)));
        QVERIFY2(used >= 0 && used <= total,
            qPrintable(QString("RAM used %1 > total %2").arg(used).arg(total)));

        /* Sanity check — any real machine has at least 1 GB */
        QVERIFY2(total >= 1, qPrintable(QString("RAM total too low: %1 GB").arg(total)));
        cleanFn();
    }

    /* ── Thermal data ── */

    void testThermalData() {
        bool hasThermal = QFile::exists("/dev/shm/argos-thermal");
        if (!hasThermal)
            QSKIP("No /dev/shm/argos-thermal — skipping thermal test");

        initFn("/tmp");
        const char *json = fetchFn();
        QVERIFY(json);

        QJsonObject obj = QJsonDocument::fromJson(json).object();

        /* CPU temp should be present */
        QVERIFY2(obj.contains("cpu_temp"), "Missing cpu_temp");
        int cpuTemp = obj["cpu_temp"].toInt();
        QVERIFY2(cpuTemp > 10 && cpuTemp < 110,
            qPrintable(QString("CPU temp %1°C out of range").arg(cpuTemp)));

        /* GPU temp should be present */
        QVERIFY2(obj.contains("gpu_temp"), "Missing gpu_temp");
        int gpuTemp = obj["gpu_temp"].toInt();
        QVERIFY2(gpuTemp > 0 && gpuTemp < 110,
            qPrintable(QString("GPU temp %1°C out of range").arg(gpuTemp)));

        /* DDR temp */
        if (obj.contains("ddr_temp")) {
            int ddrTemp = obj["ddr_temp"].toInt();
            QVERIFY2(ddrTemp > 10 && ddrTemp < 110,
                qPrintable(QString("DDR temp %1°C out of range").arg(ddrTemp)));
        }

        /* NVMe temps */
        if (obj.contains("nvme_temps")) {
            QJsonArray nvme = obj["nvme_temps"].toArray();
            QVERIFY2(!nvme.isEmpty(), "nvme_temps is empty array");
            for (int i = 0; i < nvme.size(); i++) {
                int t = nvme[i].toInt();
                QVERIFY2(t > 0 && t < 100,
                    qPrintable(QString("NVMe[%1] temp %2°C out of range").arg(i).arg(t)));
            }
        }

        cleanFn();
    }

    /* ── Cleanup ── */

    void cleanupTestCase() {
        if (handle) dlclose(handle);
    }
};

QTEST_MAIN(TestSystem)
#include "test_system.moc"
