/*
 * github.so - GM Dashboard GitHub plugin
 * Fetches profile stats via gh CLI (GraphQL)
 * No config needed — uses gh's keyring auth.
 */
#include "../gm-plugin.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <cstdio>

static QByteArray resultBuf;

/* ── popen helper ────────────────────────────────────────────── */

static QByteArray runCmd(const char *cmd)
{
	FILE *fp = popen(cmd, "r");
	if (!fp) return {};
	QByteArray data;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		data.append(buf, static_cast<qsizetype>(n));
	pclose(fp);
	return data;
}

/* ── Plugin ABI ──────────────────────────────────────────────── */

extern "C" {

struct gm_info gm_info(void)
{
	struct gm_info i = {
		GM_API_VERSION,
		"github",
		"GITHUB",
		"1.0.0",
		"regen-dev",
		20,
		GM_CAP_EXEC | GM_CAP_NETWORK,
		"Exec: gh CLI; Network: GitHub API queries",
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
	QByteArray raw = runCmd(
		"gh api graphql -f query='"
		"{ viewer { login name "
		"followers { totalCount } following { totalCount } "
		"repositories(first: 100, ownerAffiliations: OWNER) { "
		"totalCount nodes { stargazerCount forkCount } } "
		"contributionsCollection { contributionCalendar "
		"{ totalContributions } } "
		"} }' 2>/dev/null"
	);
	if (raw.isEmpty()) return nullptr;

	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isObject()) return nullptr;

	QJsonObject viewer = doc["data"].toObject()["viewer"].toObject();
	if (viewer.isEmpty()) return nullptr;

	QJsonObject r;
	r["login"] = viewer["login"];
	r["name"] = viewer["name"];
	r["followers"] = viewer["followers"].toObject()["totalCount"];
	r["following"] = viewer["following"].toObject()["totalCount"];

	QJsonObject repos = viewer["repositories"].toObject();
	r["repos"] = repos["totalCount"];

	int stars = 0, forks = 0;
	for (auto node : repos["nodes"].toArray()) {
		QJsonObject nd = node.toObject();
		stars += nd["stargazerCount"].toInt();
		forks += nd["forkCount"].toInt();
	}
	r["stars"] = stars;
	r["forks"] = forks;

	r["contributions"] = viewer["contributionsCollection"].toObject()
		["contributionCalendar"].toObject()["totalContributions"];

	resultBuf = QJsonDocument(r).toJson(QJsonDocument::Compact);
	return resultBuf.constData();
}

void gm_cleanup(void) { }

} /* extern "C" */
