/*
 * system.so - GM Dashboard system stats plugin
 * Reads /proc/uptime, /proc/loadavg, /proc/meminfo, /dev/shm/argos-thermal
 * No config needed.
 */
#include "../gm-plugin.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QByteArray>
#include <cstdio>
#include <cstring>

#define THERMAL_PATH "/dev/shm/argos-thermal"

static QByteArray resultBuf;

/* ── Plugin ABI ──────────────────────────────────────────────── */

extern "C" {

struct gm_info gm_info(void)
{
	struct gm_info i = {
		GM_API_VERSION,
		"system",
		"SYSTEM",
		"1.0.0",
		"regen-dev",
		90,
		GM_CAP_PROC | GM_CAP_SHM,
		"Proc: system stats from /proc; SHM: thermal data from /dev/shm",
	};
	return i;
}

int gm_init(const char *config_dir)
{
	(void)config_dir;
	return 0;
}

const char *gm_fetch(void)
{
	QJsonObject r;

	/* Uptime */
	QFile uptime("/proc/uptime");
	if (uptime.open(QIODevice::ReadOnly)) {
		double up;
		if (sscanf(uptime.readAll().constData(), "%lf", &up) == 1)
			r["uptime"] = QString("%1d %2h %3m")
				.arg(static_cast<int>(up / 86400))
				.arg((static_cast<int>(up) % 86400) / 3600)
				.arg((static_cast<int>(up) % 3600) / 60);
	}

	/* Load average */
	QFile loadavg("/proc/loadavg");
	if (loadavg.open(QIODevice::ReadOnly)) {
		double l1, l5, l15;
		if (sscanf(loadavg.readAll().constData(), "%lf %lf %lf",
				&l1, &l5, &l15) == 3)
			r["load"] = QString("%1 / %2 / %3")
				.arg(l1, 0, 'f', 2).arg(l5, 0, 'f', 2).arg(l15, 0, 'f', 2);
	}

	/* Memory */
	QFile meminfo("/proc/meminfo");
	if (meminfo.open(QIODevice::ReadOnly)) {
		unsigned long total = 0, avail = 0;
		QByteArray data = meminfo.readAll();
		for (auto &line : data.split('\n')) {
			sscanf(line.constData(), "MemTotal: %lu kB", &total);
			sscanf(line.constData(), "MemAvailable: %lu kB", &avail);
		}
		if (total > 0) {
			r["ram_total_gb"] = total / 1048576.0;
			r["ram_used_gb"] = (total - avail) / 1048576.0;
		}
	}

	/* Thermal from argos-thermal: CPU:N GPU:N DDR:N N0:N ... */
	QFile thermal(THERMAL_PATH);
	if (thermal.open(QIODevice::ReadOnly)) {
		QByteArray line = thermal.readLine();
		int v;
		const char *s = line.constData();
		const char *p;

		if ((p = strstr(s, "CPU:")) && sscanf(p, "CPU:%d", &v) == 1)
			r["cpu_temp"] = v;
		if ((p = strstr(s, "GPU:")) && sscanf(p, "GPU:%d", &v) == 1)
			r["gpu_temp"] = v;
		if ((p = strstr(s, "DDR:")) && sscanf(p, "DDR:%d", &v) == 1)
			r["ddr_temp"] = v;

		QJsonArray nvme;
		for (int i = 0; i < 9; i++) {
			char key[8];
			snprintf(key, sizeof(key), "N%d:", i);
			if ((p = strstr(s, key)) &&
			    sscanf(p + strlen(key), "%d", &v) == 1)
				nvme.append(v);
		}
		if (!nvme.isEmpty())
			r["nvme_temps"] = nvme;
	}

	resultBuf = QJsonDocument(r).toJson(QJsonDocument::Compact);
	return resultBuf.constData();
}

void gm_cleanup(void) { }

} /* extern "C" */
