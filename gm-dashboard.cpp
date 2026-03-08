/*
 * gm-dashboard - Good Morning Dashboard (plugin architecture)
 *
 * Loads .so plugins from ~/.local/lib/gm-dashboard/
 * Assembles a sci-fi HTML dashboard from base template + plugin cards.
 * Ed25519 signature verification + Landlock + seccomp sandbox per plugin.
 *
 * Usage: gm-dashboard [--no-browser|--json|--help]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <linux/limits.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <sodium.h>
#include <seccomp.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

#include <algorithm>
#include <vector>

#include "gm-plugin.h"

/* Flush gcov data before _exit() in forked children */
extern "C" void __gcov_dump(void) __attribute__((weak));

#define MAX_BUF          (512 * 1024)
#define OUTPUT_PATH      "/dev/shm/gm-dashboard.html"
#define FETCH_TIMEOUT_MS 30000
#define TMPFS_CFG_DIR    "/dev/shm/gm-cfg"

/* ── Landlock syscall wrappers ───────────────────────────────── */

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

/* ── Plugin handle ───────────────────────────────────────────── */

struct Plugin {
	void           *handle;
	gm_info         info;
	gm_init_fn      init;
	gm_fetch_fn     fetch;
	gm_cleanup_fn   cleanup;
	QByteArray      json;
	QString         cardHtml;
	char            soPath[1024];
};

/* ── Utility ─────────────────────────────────────────────────── */

static QByteArray readBinary(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) return {};
	return f.readAll();
}

/*
 * Verify Ed25519 signature of a .so file.
 * Returns:  1 = valid,  0 = INVALID (tampered),  -1 = no .sig (unsigned)
 */
static int verifyPluginSig(const char *soPath, const unsigned char *pk)
{
	QByteArray soData = readBinary(QString::fromLocal8Bit(soPath));
	if (soData.isEmpty()) return 0;

	QByteArray sig = readBinary(QString::fromLocal8Bit(soPath) + ".sig");
	if (sig.size() != crypto_sign_BYTES)
		return -1;

	bool ok = crypto_sign_verify_detached(
		reinterpret_cast<const unsigned char *>(sig.constData()),
		reinterpret_cast<const unsigned char *>(soData.constData()),
		soData.size(), pk) == 0;
	return ok ? 1 : 0;
}

/* ── Config ──────────────────────────────────────────────────── */

static QString loadName(const QString &configDir)
{
	QFile f(configDir + "/config");
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return {};

	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine()).trimmed();
		if (line.startsWith('#') || line.isEmpty()) continue;
		int eq = line.indexOf('=');
		if (eq < 0) continue;
		if (line.left(eq) == "NAME")
			return line.mid(eq + 1);
	}
	return {};
}

/*
 * Resolve config directory:
 * 1. /dev/shm/gm-cfg/ (vault unlocked via gm-config unlock)
 * 2. ~/.config/gm-dashboard/ (plaintext fallback, with warning)
 */
static QString resolveConfigDir()
{
	QDir tmpfs(TMPFS_CFG_DIR);
	if (tmpfs.exists()) {
		fprintf(stderr, "[gm] Config: encrypted vault (unlocked)\n");
		return TMPFS_CFG_DIR;
	}

	QString home = QDir::homePath();
	QString vaultPath = home + "/.config/gm-dashboard/vault.enc";
	if (QFile::exists(vaultPath)) {
		fprintf(stderr,
			"[gm] WARNING: vault.enc exists but is locked.\n"
			"[gm] Run 'gm-config unlock' first, or configs will be empty.\n"
			"[gm] Falling back to plaintext configs.\n");
	}

	return home + "/.config/gm-dashboard";
}

/* ── Sandbox: Landlock ───────────────────────────────────────── */

static int applyLandlock(unsigned caps, const char *pluginDir,
                         const char *configDir)
{
	int abi = syscall(__NR_landlock_create_ruleset, nullptr, 0,
		LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 1) return -1;

	/* Build handled access mask — all access types we restrict */
	__u64 fsAccess =
		LANDLOCK_ACCESS_FS_EXECUTE |
		LANDLOCK_ACCESS_FS_WRITE_FILE |
		LANDLOCK_ACCESS_FS_READ_FILE |
		LANDLOCK_ACCESS_FS_READ_DIR |
		LANDLOCK_ACCESS_FS_REMOVE_DIR |
		LANDLOCK_ACCESS_FS_REMOVE_FILE |
		LANDLOCK_ACCESS_FS_MAKE_CHAR |
		LANDLOCK_ACCESS_FS_MAKE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_REG |
		LANDLOCK_ACCESS_FS_MAKE_SOCK |
		LANDLOCK_ACCESS_FS_MAKE_FIFO |
		LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		LANDLOCK_ACCESS_FS_MAKE_SYM;

	if (abi >= 2) fsAccess |= LANDLOCK_ACCESS_FS_REFER;
	if (abi >= 3) fsAccess |= LANDLOCK_ACCESS_FS_TRUNCATE;
	if (abi >= 5) fsAccess |= LANDLOCK_ACCESS_FS_IOCTL_DEV;

	/* Network: restrict if plugin doesn't need it */
	__u64 netAccess = 0;
	if (abi >= 4 && !(caps & GM_CAP_NETWORK))
		netAccess = LANDLOCK_ACCESS_NET_BIND_TCP |
		            LANDLOCK_ACCESS_NET_CONNECT_TCP;

	struct landlock_ruleset_attr attr = {};
	attr.handled_access_fs  = fsAccess;
	attr.handled_access_net = netAccess;

	int rulesetFd = syscall(__NR_landlock_create_ruleset,
		&attr, sizeof(attr), 0);
	if (rulesetFd < 0) return -1;

	/* Helper to add a path rule */
	auto addPath = [&](__u64 allowed, const char *path) {
		int fd = open(path, O_PATH | O_CLOEXEC);
		if (fd >= 0) {
			struct landlock_path_beneath_attr r = {};
			r.allowed_access = allowed & fsAccess;
			r.parent_fd = fd;
			syscall(__NR_landlock_add_rule, rulesetFd,
				LANDLOCK_RULE_PATH_BENEATH, &r, 0);
			close(fd);
		}
	};

	__u64 ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
	__u64 roExec = ro | LANDLOCK_ACCESS_FS_EXECUTE;

	/* Shared libraries — all plugins need these */
	addPath(roExec, "/usr/lib");
	addPath(roExec, "/lib");
	addPath(roExec, "/lib64");
	addPath(roExec, "/usr/lib64");

	/* Plugin dir (read + execute for dlopen) */
	addPath(roExec, pluginDir);

	/* Config dir (plugins read their .conf files) */
	addPath(ro, configDir);

	/* Also allow tmpfs config dir if vault is unlocked */
	if (strcmp(configDir, TMPFS_CFG_DIR) != 0)
		addPath(ro, TMPFS_CFG_DIR);

	/* Essential system files */
	addPath(LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE,
		"/dev/null");
	addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/dev/urandom");
	addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/ld.so.cache");
	addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/localtime");
	addPath(ro, "/usr/share/zoneinfo");

	/* Per-capability paths */
	if (caps & GM_CAP_NETWORK) {
		addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/resolv.conf");
		addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/hosts");
		addPath(ro, "/etc/ssl");
		addPath(ro, "/etc/ca-certificates");
		addPath(ro, "/usr/share/ca-certificates");
		addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/nsswitch.conf");
		addPath(LANDLOCK_ACCESS_FS_READ_FILE, "/etc/gai.conf");
		addPath(roExec, "/lib/x86_64-linux-gnu");
	}

	if (caps & GM_CAP_PROC)
		addPath(ro, "/proc");

	if (caps & GM_CAP_SHM)
		addPath(ro, "/dev/shm");

	if (caps & GM_CAP_EXEC) {
		addPath(roExec, "/usr/bin");
		addPath(roExec, "/bin");
		addPath(ro, "/usr/share");
		QByteArray hcfg = (QDir::homePath() + "/.config").toLocal8Bit();
		addPath(ro, hcfg.constData());
	}

#ifndef GM_TESTING
	if (syscall(__NR_landlock_restrict_self, rulesetFd, 0) < 0) { // LCOV_EXCL_LINE
		close(rulesetFd); // LCOV_EXCL_LINE
		return -1; // LCOV_EXCL_LINE
	}
#endif
	close(rulesetFd);
	return 0;
}

/* ── Sandbox: seccomp ────────────────────────────────────────── */

static int applySeccomp(unsigned caps)
{
	scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
	if (!ctx) return -1;

	#define BLOCK(s) seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, \
		SCMP_SYS(s), 0)

	/* Always blocked: dangerous kernel interfaces */
	BLOCK(ptrace);
	BLOCK(process_vm_readv);
	BLOCK(process_vm_writev);
	BLOCK(mount);
	BLOCK(umount2);
	BLOCK(pivot_root);
	BLOCK(chroot);
	BLOCK(reboot);
	BLOCK(kexec_load);
	BLOCK(kexec_file_load);
	BLOCK(init_module);
	BLOCK(finit_module);
	BLOCK(delete_module);
	BLOCK(acct);
	BLOCK(swapon);
	BLOCK(swapoff);
	BLOCK(settimeofday);
	BLOCK(clock_settime);
	BLOCK(adjtimex);
	BLOCK(keyctl);
	BLOCK(add_key);
	BLOCK(request_key);
	BLOCK(unshare);
	BLOCK(setns);

	/* Privilege escalation */
	BLOCK(setuid);
	BLOCK(setgid);
	BLOCK(setreuid);
	BLOCK(setregid);
	BLOCK(setresuid);
	BLOCK(setresgid);
	BLOCK(setgroups);

	/* Filesystem modification (belt+suspenders with Landlock) */
	BLOCK(unlink);
	BLOCK(unlinkat);
	BLOCK(rmdir);
	BLOCK(rename);
	BLOCK(renameat2);
	BLOCK(chmod);
	BLOCK(fchmod);
	BLOCK(fchmodat);
	BLOCK(chown);
	BLOCK(fchown);
	BLOCK(fchownat);
	BLOCK(lchown);
	BLOCK(link);
	BLOCK(linkat);
	BLOCK(symlink);
	BLOCK(symlinkat);
	BLOCK(mkdir);
	BLOCK(mkdirat);
	BLOCK(mknod);
	BLOCK(mknodat);

	/* Conditional: exec */
	if (!(caps & GM_CAP_EXEC)) {
		BLOCK(execve);
		BLOCK(execveat);
	}

	/* Conditional: network */
	if (!(caps & GM_CAP_NETWORK)) {
		BLOCK(socket);
		BLOCK(socketpair);
		BLOCK(bind);
		BLOCK(listen);
		BLOCK(accept);
		BLOCK(accept4);
		BLOCK(connect);
		BLOCK(sendto);
		BLOCK(recvfrom);
		BLOCK(sendmsg);
		BLOCK(recvmsg);
		BLOCK(shutdown);
		BLOCK(setsockopt);
		BLOCK(getsockopt);
		BLOCK(getpeername);
		BLOCK(getsockname);
	}

	#undef BLOCK

#ifndef GM_TESTING
	int ret = seccomp_load(ctx); // LCOV_EXCL_LINE
#else
	int ret = 0;
#endif
	seccomp_release(ctx);
	return ret;
}

/* ── Sandboxed plugin fetch ──────────────────────────────────── */

/*
 * Fork a child, apply Landlock + seccomp, run plugin init+fetch.
 * Returns JSON bytes, or empty on error.
 */
static QByteArray sandboxedFetch(Plugin &p, const QByteArray &pluginDir,
                                 const QByteArray &configDir)
{
	int pipefd[2];
	if (pipe(pipefd) < 0) return {};

	pid_t pid = fork();
	if (pid < 0) { // LCOV_EXCL_START — fork failure
		close(pipefd[0]);
		close(pipefd[1]);
		return {};
	} // LCOV_EXCL_STOP

	if (pid == 0) { // LCOV_EXCL_START — forked child, Landlock blocks gcov writes
		/* ── Child: sandbox + init + fetch ─────────────── */
		close(pipefd[0]);

		prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

		if (applyLandlock(p.info.caps_required, pluginDir.constData(),
				configDir.constData()) == 0)
			fprintf(stderr, "[gm-sandbox] %s: Landlock OK\n",
				p.info.id);
		else
			fprintf(stderr, "[gm-sandbox] %s: Landlock failed\n",
				p.info.id);

		if (applySeccomp(p.info.caps_required) == 0)
			fprintf(stderr, "[gm-sandbox] %s: seccomp OK\n",
				p.info.id);
		else
			fprintf(stderr, "[gm-sandbox] %s: seccomp failed\n",
				p.info.id);

		/* Plugin init + fetch inside sandbox */
		if (p.init(configDir.constData()) != 0) {
			close(pipefd[1]);
			if (__gcov_dump) __gcov_dump();
		_exit(1);
		}

		const char *json = p.fetch();
		if (json) {
			uint32_t len = strlen(json);
			ssize_t w = write(pipefd[1], &len, sizeof(len));
			if (w > 0) w = write(pipefd[1], json, len);
			if (w < 0) _exit(1);
		}

		p.cleanup();
		close(pipefd[1]);
		if (__gcov_dump) __gcov_dump();
		_exit(0);
	} // LCOV_EXCL_STOP

	/* ── Parent: read JSON from pipe ───────────────── */
	close(pipefd[1]);

	QByteArray result;
	struct pollfd pfd = {pipefd[0], POLLIN, 0};

	if (poll(&pfd, 1, FETCH_TIMEOUT_MS) > 0) {
		uint32_t len = 0;
		if (read(pipefd[0], &len, sizeof(len)) == static_cast<ssize_t>(sizeof(len)) &&
		    len > 0 && len < static_cast<uint32_t>(MAX_BUF)) {
			char *buf = static_cast<char *>(malloc(len + 1));
			if (buf) {
				size_t got = 0;
				while (got < len) {
					ssize_t r = read(pipefd[0], buf + got,
						len - got);
					if (r <= 0) break;
					got += r;
				}
				if (got == len)
					result = QByteArray(buf, len);
				free(buf);
			}
		}
	} else { // LCOV_EXCL_START — timeout path
		fprintf(stderr, "[gm-sandbox] %s: timeout (%ds), killing\n",
			p.info.id, FETCH_TIMEOUT_MS / 1000);
		kill(pid, SIGKILL);
	} // LCOV_EXCL_STOP

	close(pipefd[0]);

	int status;
	waitpid(pid, &status, 0);

	if (WIFSIGNALED(status)) { // LCOV_EXCL_START — signal death path
		int sig = WTERMSIG(status);
		fprintf(stderr, "[gm-sandbox] %s: killed by signal %d%s\n",
			p.info.id, sig,
			sig == 31 ? " (seccomp violation)" : "");
		return {};
	} // LCOV_EXCL_STOP

	return result;
}

/* ── Plugin loader ───────────────────────────────────────────── */

static void loadPlugins(std::vector<Plugin> &plugins,
                        const unsigned char *pk)
{
	QString home = QDir::homePath();
	QString dir = home + "/.local/lib/gm-dashboard";

	QDir d(dir);
	if (!d.exists()) {
		fprintf(stderr, "[gm] No plugin dir: %s\n", qPrintable(dir));
		return;
	}

	for (const QString &name : d.entryList({"*.so"}, QDir::Files)) {
		QByteArray path = d.filePath(name).toLocal8Bit();

		/* Verify signature BEFORE dlopen (constructors run at dlopen) */
		if (pk) {
			int sig = verifyPluginSig(path.constData(), pk);
			if (sig == 0) {
				fprintf(stderr, "[gm] %s: INVALID signature — REJECTED\n",
					qPrintable(name));
				continue;
			}
			if (sig == -1)
				fprintf(stderr, "[gm] %s: unsigned (no .sig)\n",
					qPrintable(name));
			else
				fprintf(stderr, "[gm] %s: signature verified\n",
					qPrintable(name));
		}

		void *h = dlopen(path.constData(), RTLD_LAZY);
		if (!h) {
			fprintf(stderr, "[gm] dlopen %s: %s\n",
				qPrintable(name), dlerror());
			continue;
		}

		auto ifn   = reinterpret_cast<gm_info_fn>(dlsym(h, "gm_info"));
		auto initf = reinterpret_cast<gm_init_fn>(dlsym(h, "gm_init"));
		auto fetchf = reinterpret_cast<gm_fetch_fn>(dlsym(h, "gm_fetch"));
		auto cleanf = reinterpret_cast<gm_cleanup_fn>(dlsym(h, "gm_cleanup"));

		if (!ifn || !initf || !fetchf || !cleanf) {
			fprintf(stderr, "[gm] %s: missing ABI symbols\n",
				qPrintable(name));
			dlclose(h);
			continue;
		}

		Plugin p = {};
		p.handle  = h;
		p.info    = ifn();
		p.init    = initf;
		p.fetch   = fetchf;
		p.cleanup = cleanf;

		if (p.info.api_version != GM_API_VERSION) {
			fprintf(stderr, "[gm] %s: API v%d != v%d\n",
				qPrintable(name), p.info.api_version, GM_API_VERSION);
			dlclose(h);
			continue;
		}

		snprintf(p.soPath, sizeof(p.soPath), "%s", path.constData());

		/* Load card HTML fragment */
		QFile htmlFile(dir + "/" + QLatin1String(p.info.id) + ".html");
		if (htmlFile.open(QIODevice::ReadOnly | QIODevice::Text))
			p.cardHtml = QString::fromUtf8(htmlFile.readAll());

		fprintf(stderr, "[gm] Plugin: %s v%s (order %d, caps 0x%02x)\n",
			p.info.id, p.info.version, p.info.order,
			p.info.caps_required);
		plugins.push_back(std::move(p));
	}
}

/* ── Greeting ────────────────────────────────────────────────── */

static const char *getGreeting(int hour)
{
	if (hour >= 5  && hour < 12) return "GOOD MORNING";
	if (hour >= 12 && hour < 18) return "GOOD AFTERNOON";
	if (hour >= 18 && hour < 23) return "GOOD EVENING";
	return "GOOD NIGHT";
}

/* ── Viewer launch ───────────────────────────────────────────── */

static void openViewer(const char *htmlPath, const QByteArray &configDir)
{
	pid_t pid = fork();
	if (pid != 0) return;
	// LCOV_EXCL_START — forked child, exec replaces process
	setsid();

	QByteArray viewer = (QDir::homePath() + "/.local/bin/gm-viewer").toLocal8Bit();
	execlp(viewer.constData(), "gm-viewer", htmlPath,
		configDir.constData(), static_cast<char *>(nullptr));
	perror("gm-viewer");
	_exit(1);
}
// LCOV_EXCL_STOP

/* ── Main ────────────────────────────────────────────────────── */

#ifndef GM_TESTING
int main(int argc, char **argv)
{
	bool noBrowser = false, jsonOnly = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-browser") == 0) noBrowser = true;
		else if (strcmp(argv[i], "--json") == 0) jsonOnly = true;
		else if (strcmp(argv[i], "--help") == 0) {
			puts("Usage: gm-dashboard [--no-browser|--json|--help]");
			return 0;
		}
	}

	/* Resolve config dir (vault tmpfs or plaintext fallback) */
	QString cfgDir = resolveConfigDir();
	QByteArray cfgDirBytes = cfgDir.toLocal8Bit();

	QString name = loadName(cfgDir);

	if (sodium_init() < 0) { // LCOV_EXCL_START — system failure
		fprintf(stderr, "[gm] libsodium init failed\n");
		return 1;
	} // LCOV_EXCL_STOP
	curl_global_init(CURL_GLOBAL_DEFAULT);

	/* Load marketplace public key */
	QByteArray pkData = readBinary(
		QDir::homePath() + "/.config/gm-dashboard/marketplace.pub");
	const unsigned char *marketplacePk = nullptr;
	if (pkData.size() == crypto_sign_PUBLICKEYBYTES) {
		marketplacePk = reinterpret_cast<const unsigned char *>(pkData.constData());
		fprintf(stderr, "[gm] Marketplace key loaded\n");
	} else {
		fprintf(stderr, "[gm] No marketplace key — sig checks disabled\n"); // LCOV_EXCL_LINE
	}

	/* Load plugins (metadata only — init+fetch run sandboxed) */
	std::vector<Plugin> plugins;
	loadPlugins(plugins, marketplacePk);
	std::sort(plugins.begin(), plugins.end(),
		[](const Plugin &a, const Plugin &b) {
			return a.info.order < b.info.order;
		});

	/* Fetch from each plugin in sandboxed child process */
	QByteArray plugDir = (QDir::homePath() + "/.local/lib/gm-dashboard").toLocal8Bit();

	for (auto &p : plugins) {
		fprintf(stderr, "[gm] Fetch: %s (sandboxed)...\n", p.info.id);
		p.json = sandboxedFetch(p, plugDir, cfgDirBytes);
		if (p.json.isEmpty())
			fprintf(stderr, "[gm] %s returned NULL\n", p.info.id); // LCOV_EXCL_LINE
	}

	/* Build root JSON */
	QJsonObject root;
	time_t now = time(nullptr);
	struct tm *tm = localtime(&now);

	root["greeting"] = getGreeting(tm->tm_hour);

	char dbuf[128];
	strftime(dbuf, sizeof(dbuf), "%A, %B %d, %Y", tm);
	root["date"] = QString(dbuf).toUpper();

	char tzbuf[32];
	strftime(tzbuf, sizeof(tzbuf), "%Z", tm);
	root["timezone"] = tzbuf;

	/* Name: config > plugin "name" field > nothing */
	if (name.isEmpty()) {
		for (auto &p : plugins) {
			if (p.json.isEmpty()) continue;
			QJsonDocument pd = QJsonDocument::fromJson(p.json);
			if (pd.isObject() && pd.object().contains("name")) {
				name = pd["name"].toString();
				break;
			}
		}
	}
	if (!name.isEmpty())
		root["name"] = name;

	/* Plugins data */
	QJsonObject pobj;
	for (auto &p : plugins) {
		if (p.json.isEmpty()) continue;
		QJsonDocument pd = QJsonDocument::fromJson(p.json);
		if (pd.isObject())
			pobj[p.info.id] = pd.object();
	}
	root["plugins"] = pobj;

	QByteArray jsonStr = QJsonDocument(root).toJson(QJsonDocument::Compact);

	if (jsonOnly) {
		puts(jsonStr.constData());
		goto done;
	}

	{
		/* ── HTML assembly ───────────────────────────────────────── */
		QFile baseFile(QDir::homePath() +
			"/.local/share/gm-dashboard/base.html");
		if (!baseFile.open(QIODevice::ReadOnly | QIODevice::Text)) { // LCOV_EXCL_START
			fprintf(stderr, "[gm] Cannot read base.html\n");
			goto done;
		} // LCOV_EXCL_STOP
		QString html = QString::fromUtf8(baseFile.readAll());

		/* Generate card wrapper divs */
		QString cards;
		for (auto &p : plugins) {
			cards += QString(
				"<div class=\"card\" data-plugin=\"%1\" "
				"style=\"order:%2\">\n"
				"  <span class=\"c tl\"></span>"
				"<span class=\"c tr\"></span>"
				"<span class=\"c bl\"></span>"
				"<span class=\"c br\"></span>\n"
				"  <div class=\"card-title\">"
				"<span>%3</span>"
				"<span class=\"gear\" onclick=\"toggleCfg('%1')\" "
				"title=\"Configure\">&#9881;</span>"
				"</div>\n"
				"  <div class=\"cfg-panel\" id=\"cfg-%1\" "
				"style=\"display:none\">"
				"<span class=\"cfg-path\">"
				"~/.config/gm-dashboard/%1.conf</span>"
				"</div>\n"
				"  <div id=\"p-%1\"></div>\n"
				"</div>\n")
				.arg(p.info.id)
				.arg(p.info.order)
				.arg(p.info.name);
		}

		/* Collect plugin HTML scripts */
		QString scripts;
		for (auto &p : plugins) {
			if (!p.cardHtml.isEmpty())
				scripts += p.cardHtml;
		}

		/* Replace placeholders */
		html.replace("__CARDS__", cards);
		html.replace("__PLUGIN_SCRIPTS__", scripts);
		html.replace("__GM_DATA__", QString::fromUtf8(jsonStr));

		fprintf(stderr, "[gm] Writing %s\n", OUTPUT_PATH);
		QFile out(OUTPUT_PATH);
		if (out.open(QIODevice::WriteOnly | QIODevice::Text))
			out.write(html.toUtf8());

		if (!noBrowser) { // LCOV_EXCL_START — launches GUI viewer
			fprintf(stderr, "[gm] Opening viewer...\n");
			openViewer(OUTPUT_PATH, cfgDirBytes);
		} // LCOV_EXCL_STOP
	}

done:
	for (auto &p : plugins)
		dlclose(p.handle);
	curl_global_cleanup();
	fprintf(stderr, "[gm] Done.\n");
	return 0;
}
#endif /* GM_TESTING */
