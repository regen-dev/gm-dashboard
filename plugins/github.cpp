/*
 * github.so - GM Dashboard GitHub plugin v2
 * Fetches profile stats + tracked repo stats via gh CLI (GraphQL).
 *
 * Config: ~/.config/gm-dashboard/github.conf
 *   SHOW_PROFILE=true|false  (default: true)
 *   REPO=owner/name          (one per line, multiple allowed)
 */
#include "../gm-plugin.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <cstdio>

static QByteArray resultBuf;
static QStringList s_repos;
static bool s_showProfile = true;

/* ── Repo name validation ────────────────────────────────────── */

static bool isValidRepo(const QString &repo)
{
	/* GitHub owner: [a-zA-Z0-9-] (starts with alphanum)
	 * GitHub name:  [a-zA-Z0-9._-] */
	static QRegularExpression rx(
		QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9-]*/[a-zA-Z0-9._][a-zA-Z0-9._-]*$"));
	return repo.length() <= 140 && rx.match(repo).hasMatch();
}

/* ── GraphQL query builder ───────────────────────────────────── */

static QString buildQuery(const QStringList &repos, bool includeProfile)
{
	QString q = QStringLiteral("{ ");

	if (includeProfile) {
		q += QStringLiteral(
			"viewer { login name "
			"followers { totalCount } following { totalCount } "
			"repositories(first: 100, ownerAffiliations: OWNER) { "
			"totalCount nodes { stargazerCount forkCount } } "
			"contributionsCollection { contributionCalendar "
			"{ totalContributions } } } ");
	}

	for (int i = 0; i < repos.size(); i++) {
		int slash = repos[i].indexOf('/');
		QString owner = repos[i].left(slash);
		QString name = repos[i].mid(slash + 1);
		q += QString("r%1: repository(owner:\"%2\",name:\"%3\") { "
			"nameWithOwner stargazerCount forkCount "
			"watchers { totalCount } "
			"issues(states:OPEN) { totalCount } "
			"description } ")
			.arg(i).arg(owner).arg(name);
	}

	q += QStringLiteral("}");
	return q;
}

/* ── Response parsers ────────────────────────────────────────── */

static QJsonObject parseProfile(const QJsonObject &viewer)
{
	if (viewer.isEmpty()) return {};

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

	return r;
}

static QJsonArray parseTracked(const QJsonObject &data, const QStringList &repos)
{
	QJsonArray arr;
	for (int i = 0; i < repos.size(); i++) {
		QString key = QString("r%1").arg(i);
		if (!data.contains(key) || data[key].isNull()) continue;

		QJsonObject repo = data[key].toObject();
		QJsonObject entry;
		entry["repo"] = repo["nameWithOwner"];
		entry["stars"] = repo["stargazerCount"];
		entry["forks"] = repo["forkCount"];
		entry["watchers"] = repo["watchers"].toObject()["totalCount"];
		entry["issues"] = repo["issues"].toObject()["totalCount"];
		entry["description"] = repo["description"];
		arr.append(entry);
	}
	return arr;
}

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
		"2.0.0",
		"regen-dev",
		20,
		GM_CAP_EXEC | GM_CAP_NETWORK,
		"Exec: gh CLI; Network: GitHub API queries",
	};
	return i;
}

int gm_init(const char *config_dir)
{
	s_repos.clear();
	s_showProfile = true;
	resultBuf.clear();

	QFile f(QString("%1/github.conf").arg(config_dir));
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!f.atEnd()) {
			QString line = QString::fromUtf8(f.readLine()).trimmed();
			if (line.startsWith('#') || line.isEmpty()) continue;
			int eq = line.indexOf('=');
			if (eq < 0) continue;
			QString key = line.left(eq);
			QString val = line.mid(eq + 1).trimmed();
			if (key == "REPO" && isValidRepo(val)) {
				if (!s_repos.contains(val))
					s_repos.append(val);
			} else if (key == "SHOW_PROFILE") {
				s_showProfile = (val != "false" && val != "0");
			}
		}
	}

	return 0;
}

const char *gm_fetch(void)
{
	if (!s_showProfile && s_repos.isEmpty())
		return nullptr;

	QString query = buildQuery(s_repos, s_showProfile);
	QString cmd = QString("gh api graphql -f query='%1' 2>/dev/null").arg(query);
	QByteArray raw = runCmd(qPrintable(cmd));
	if (raw.isEmpty()) return nullptr;

	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isObject()) return nullptr;

	QJsonObject data = doc["data"].toObject();
	if (data.isEmpty()) return nullptr;

	QJsonObject result;

	/* Profile stats (backward compatible flat fields) */
	if (s_showProfile) {
		QJsonObject profile = parseProfile(data["viewer"].toObject());
		for (auto it = profile.begin(); it != profile.end(); ++it)
			result[it.key()] = it.value();
		result["show_profile"] = true;
	} else {
		result["show_profile"] = false;
	}

	/* Tracked repos */
	result["tracked"] = parseTracked(data, s_repos);

	resultBuf = QJsonDocument(result).toJson(QJsonDocument::Compact);
	return resultBuf.constData();
}

void gm_cleanup(void)
{
	s_repos.clear();
	resultBuf.clear();
}

} /* extern "C" */
