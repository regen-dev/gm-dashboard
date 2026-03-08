/*
 * gm-config - Encrypted configuration vault for GM Dashboard
 *
 * All plugin configs stored in a single encrypted file using
 * libsodium: Argon2id KDF + XSalsa20-Poly1305 (secretbox).
 *
 * Vault: ~/.config/gm-dashboard/vault.enc
 * Format: "GMV1" + salt[16] + nonce[24] + ciphertext[...]
 * Plaintext: JSON { "section": { "KEY": "VAL" }, ... }
 *
 * Usage:
 *   gm-config init                  Create vault with password
 *   gm-config set <section> K=V     Set a config value
 *   gm-config get <section> [KEY]   Read config value(s)
 *   gm-config del <section> [KEY]   Delete section or key
 *   gm-config list                  List configured sections
 *   gm-config passwd                Change vault password
 *   gm-config unlock                Decrypt configs to tmpfs
 *   gm-config lock                  Wipe tmpfs configs
 *   gm-config migrate               Import plaintext .conf files
 */
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sodium.h>

#define VAULT_MAGIC     "GMV1"
#define VAULT_MAGIC_LEN 4
#define HEADER_LEN      (VAULT_MAGIC_LEN + crypto_pwhash_SALTBYTES + \
                         crypto_secretbox_NONCEBYTES)
#define TMPFS_DIR       "/dev/shm/gm-cfg"
#define MAX_PW_LEN      256
#define MAX_VAULT_SIZE  (1024 * 1024)

/* ── Helpers ─────────────────────────────────────────────────── */

static QString vaultPath()
{
	return QDir::homePath() + "/.config/gm-dashboard/vault.enc";
}

static QString configDir()
{
	return QDir::homePath() + "/.config/gm-dashboard";
}

static void readPassword(const char *prompt, char *buf, size_t len)
{
	/* Non-interactive mode: GM_VAULT_PASSWORD env var (for scripting/testing) */
	const char *envPw = getenv("GM_VAULT_PASSWORD");
	if (envPw) {
		strncpy(buf, envPw, len - 1);
		buf[len - 1] = '\0';
		return;
	}

	// LCOV_EXCL_START — interactive tty path, untestable in CI
	int fd = open("/dev/tty", O_RDWR);
	if (fd < 0) { buf[0] = '\0'; return; }

	struct termios old, noecho;
	tcgetattr(fd, &old);
	noecho = old;
	noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
	tcsetattr(fd, TCSANOW, &noecho);

	(void)!write(fd, prompt, strlen(prompt));

	ssize_t n = read(fd, buf, len - 1);
	if (n > 0) {
		buf[n] = '\0';
		char *nl = strchr(buf, '\n');
		if (nl) *nl = '\0';
	} else {
		buf[0] = '\0';
	}

	tcsetattr(fd, TCSANOW, &old);
	(void)!write(fd, "\n", 1);
	close(fd);
	// LCOV_EXCL_STOP
}

static int deriveKey(const char *pw, const unsigned char *salt,
                     unsigned char key[crypto_secretbox_KEYBYTES])
{
	if (crypto_pwhash(key, crypto_secretbox_KEYBYTES,
			pw, strlen(pw), salt,
			crypto_pwhash_OPSLIMIT_MODERATE,
			crypto_pwhash_MEMLIMIT_MODERATE,
			crypto_pwhash_ALG_ARGON2ID13) != 0) {
		fprintf(stderr, "error: key derivation failed (out of memory?)\n"); // LCOV_EXCL_LINE
		return -1; // LCOV_EXCL_LINE
	}
	return 0;
}

/* ── Vault I/O ───────────────────────────────────────────────── */

static bool vaultExists()
{
	return QFile::exists(vaultPath());
}

/* Decrypt vault → return parsed JSON. Caller owns the document. */
static QJsonObject vaultOpen(const char *password, bool *ok)
{
	*ok = false;
	QFile f(vaultPath());
	if (!f.open(QIODevice::ReadOnly)) { // LCOV_EXCL_START — file I/O failure
		perror(qPrintable(vaultPath()));
		return {};
	} // LCOV_EXCL_STOP

	QByteArray raw = f.readAll();
	if (raw.size() < static_cast<qsizetype>(HEADER_LEN + crypto_secretbox_MACBYTES) ||
	    raw.size() > MAX_VAULT_SIZE) {
		fprintf(stderr, "error: vault corrupt or too large\n");
		return {};
	}

	if (memcmp(raw.constData(), VAULT_MAGIC, VAULT_MAGIC_LEN) != 0) {
		fprintf(stderr, "error: not a valid vault file\n");
		return {};
	}

	auto *salt  = reinterpret_cast<const unsigned char *>(raw.constData() + VAULT_MAGIC_LEN);
	auto *nonce = salt + crypto_pwhash_SALTBYTES;
	auto *ct    = reinterpret_cast<const unsigned char *>(raw.constData() + HEADER_LEN);
	size_t ctLen = raw.size() - HEADER_LEN;

	unsigned char key[crypto_secretbox_KEYBYTES];
	if (deriveKey(password, salt, key) < 0) return {};

	size_t ptLen = ctLen - crypto_secretbox_MACBYTES;
	auto *pt = static_cast<unsigned char *>(malloc(ptLen + 1));
	if (!pt) { sodium_memzero(key, sizeof(key)); return {}; }

	if (crypto_secretbox_open_easy(pt, ct, ctLen, nonce, key) != 0) {
		fprintf(stderr, "error: wrong password or vault corrupted\n");
		sodium_memzero(key, sizeof(key));
		free(pt);
		return {};
	}
	sodium_memzero(key, sizeof(key));

	pt[ptLen] = '\0';
	QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(reinterpret_cast<const char *>(pt), ptLen));
	sodium_memzero(pt, ptLen);
	free(pt);

	if (!doc.isObject()) { // LCOV_EXCL_START — corrupted decrypted data
		fprintf(stderr, "error: vault JSON parse failed\n");
		return {};
	} // LCOV_EXCL_STOP

	*ok = true;
	return doc.object();
}

/* Encrypt JSON and write to vault file. */
static int vaultSave(const QJsonObject &data, const char *password)
{
	QByteArray pt = QJsonDocument(data).toJson(QJsonDocument::Indented);

	unsigned char salt[crypto_pwhash_SALTBYTES];
	unsigned char nonce[crypto_secretbox_NONCEBYTES];
	randombytes_buf(salt, sizeof(salt));
	randombytes_buf(nonce, sizeof(nonce));

	unsigned char key[crypto_secretbox_KEYBYTES];
	if (deriveKey(password, salt, key) < 0) return -1;

	size_t ctLen = pt.size() + crypto_secretbox_MACBYTES;
	auto *ct = static_cast<unsigned char *>(malloc(ctLen));
	if (!ct) { sodium_memzero(key, sizeof(key)); return -1; }

	crypto_secretbox_easy(ct,
		reinterpret_cast<const unsigned char *>(pt.constData()),
		pt.size(), nonce, key);
	sodium_memzero(key, sizeof(key));

	QDir().mkpath(configDir());

	int fd = open(qPrintable(vaultPath()),
		O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { perror(qPrintable(vaultPath())); free(ct); return -1; }

	bool wrote =
		write(fd, VAULT_MAGIC, VAULT_MAGIC_LEN) == VAULT_MAGIC_LEN &&
		write(fd, salt, sizeof(salt)) == sizeof(salt) &&
		write(fd, nonce, sizeof(nonce)) == sizeof(nonce) &&
		write(fd, ct, ctLen) == static_cast<ssize_t>(ctLen);

	close(fd);
	free(ct);

	if (!wrote) { // LCOV_EXCL_START — disk full / I/O failure
		fprintf(stderr, "error: failed to write vault\n");
		return -1;
	} // LCOV_EXCL_STOP
	return 0;
}

/* Open vault, modify via callback, save. */
using VaultCallback = int (*)(QJsonObject &data, void *ctx);

static int vaultModify(VaultCallback cb, void *ctx)
{
	if (!vaultExists()) {
		fprintf(stderr, "error: vault not found. Run 'gm-config init' first.\n");
		return 1;
	}

	char pw[MAX_PW_LEN];
	readPassword("Vault password: ", pw, sizeof(pw));

	bool ok;
	QJsonObject data = vaultOpen(pw, &ok);
	if (!ok) { sodium_memzero(pw, sizeof(pw)); return 1; }

	int ret = cb(data, ctx);
	if (ret == 0)
		ret = vaultSave(data, pw) < 0 ? 1 : 0;

	sodium_memzero(pw, sizeof(pw));
	return ret;
}

/* ── Commands ────────────────────────────────────────────────── */

static int cmdInit()
{
	if (vaultExists()) {
		fprintf(stderr, "Vault already exists. Use 'passwd' to change password.\n");
		return 1;
	}

	char pw1[MAX_PW_LEN], pw2[MAX_PW_LEN];
	readPassword("New vault password: ", pw1, sizeof(pw1));
	readPassword("Confirm password: ", pw2, sizeof(pw2));

	if (strcmp(pw1, pw2) != 0) { // LCOV_EXCL_START — same env var for both prompts
		fprintf(stderr, "error: passwords don't match\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	} // LCOV_EXCL_STOP
	if (strlen(pw1) < 8) {
		fprintf(stderr, "error: password must be at least 8 characters\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	}
	sodium_memzero(pw2, sizeof(pw2));

	QJsonObject data;
	int ret = vaultSave(data, pw1) < 0 ? 1 : 0;
	sodium_memzero(pw1, sizeof(pw1));

	if (ret == 0) printf("Vault created.\n");
	return ret;
}

struct SetCtx { const char *section; const char *key; const char *val; };

static int doSet(QJsonObject &data, void *ctx)
{
	auto *s = static_cast<SetCtx *>(ctx);
	QJsonObject sec = data[s->section].toObject();
	sec[s->key] = QString::fromUtf8(s->val);
	data[s->section] = sec;
	printf("Set [%s] %s\n", s->section, s->key);
	return 0;
}

static int cmdSet(const char *section, const char *kv)
{
	const char *eq = strchr(kv, '=');
	if (!eq || eq == kv) {
		fprintf(stderr, "error: format must be KEY=VALUE\n");
		return 1;
	}
	char key[128];
	size_t klen = eq - kv;
	if (klen >= sizeof(key)) klen = sizeof(key) - 1;
	memcpy(key, kv, klen);
	key[klen] = '\0';

	SetCtx ctx = {section, key, eq + 1};
	return vaultModify(doSet, &ctx);
}

static int cmdGet(const char *section, const char *key)
{
	if (!vaultExists()) {
		fprintf(stderr, "error: vault not found\n");
		return 1;
	}
	char pw[MAX_PW_LEN];
	readPassword("Vault password: ", pw, sizeof(pw));

	bool ok;
	QJsonObject data = vaultOpen(pw, &ok);
	sodium_memzero(pw, sizeof(pw));
	if (!ok) return 1;

	if (!data.contains(section)) {
		fprintf(stderr, "Section '%s' not found\n", section);
		return 1;
	}

	QJsonObject sec = data[section].toObject();
	if (key) {
		if (!sec.contains(key)) {
			fprintf(stderr, "Key '%s' not found in [%s]\n", key, section);
			return 1;
		}
		printf("%s\n", qPrintable(sec[key].toString()));
	} else {
		for (auto it = sec.begin(); it != sec.end(); ++it)
			printf("%s=%s\n", qPrintable(it.key()),
				qPrintable(it.value().toString()));
	}
	return 0;
}

struct DelCtx { const char *section; const char *key; };

static int doDel(QJsonObject &data, void *ctx)
{
	auto *d = static_cast<DelCtx *>(ctx);
	if (d->key) {
		QJsonObject sec = data[d->section].toObject();
		sec.remove(d->key);
		data[d->section] = sec;
		printf("Deleted [%s] %s\n", d->section, d->key);
	} else {
		data.remove(d->section);
		printf("Deleted section [%s]\n", d->section);
	}
	return 0;
}

static int cmdDel(const char *section, const char *key)
{
	DelCtx ctx = {section, key};
	return vaultModify(doDel, &ctx);
}

static int cmdList()
{
	if (!vaultExists()) {
		fprintf(stderr, "error: vault not found\n");
		return 1;
	}
	char pw[MAX_PW_LEN];
	readPassword("Vault password: ", pw, sizeof(pw));

	bool ok;
	QJsonObject data = vaultOpen(pw, &ok);
	sodium_memzero(pw, sizeof(pw));
	if (!ok) return 1;

	for (auto it = data.begin(); it != data.end(); ++it) {
		int n = it.value().toObject().count();
		printf("[%s] (%d key%s)\n", qPrintable(it.key()),
			n, n == 1 ? "" : "s");
	}
	return 0;
}

static int cmdPasswd()
{
	if (!vaultExists()) {
		fprintf(stderr, "error: vault not found\n");
		return 1;
	}
	char oldPw[MAX_PW_LEN];
	readPassword("Current password: ", oldPw, sizeof(oldPw));

	bool ok;
	QJsonObject data = vaultOpen(oldPw, &ok);
	sodium_memzero(oldPw, sizeof(oldPw));
	if (!ok) return 1;

	char pw1[MAX_PW_LEN], pw2[MAX_PW_LEN];
	readPassword("New password: ", pw1, sizeof(pw1));
	readPassword("Confirm new password: ", pw2, sizeof(pw2));

	if (strcmp(pw1, pw2) != 0) { // LCOV_EXCL_START — same env var for both prompts
		fprintf(stderr, "error: passwords don't match\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	}
	if (strlen(pw1) < 8) {
		fprintf(stderr, "error: password must be at least 8 characters\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	} // LCOV_EXCL_STOP
	sodium_memzero(pw2, sizeof(pw2));

	int ret = vaultSave(data, pw1) < 0 ? 1 : 0;
	sodium_memzero(pw1, sizeof(pw1));

	if (ret == 0) printf("Password changed.\n");
	return ret;
}

/* Write decrypted configs to /dev/shm/gm-cfg/ as individual .conf files */
static int cmdUnlock()
{
	if (!vaultExists()) {
		fprintf(stderr, "error: vault not found\n");
		return 1;
	}

	char pw[MAX_PW_LEN];
	readPassword("Vault password: ", pw, sizeof(pw));

	bool ok;
	QJsonObject data = vaultOpen(pw, &ok);
	sodium_memzero(pw, sizeof(pw));
	if (!ok) return 1;

	mkdir(TMPFS_DIR, 0700);
	if (chmod(TMPFS_DIR, 0700) < 0) { // LCOV_EXCL_START — /dev/shm permission failure
		perror(TMPFS_DIR);
		return 1;
	} // LCOV_EXCL_STOP

	for (auto it = data.begin(); it != data.end(); ++it) {
		QString section = it.key();
		QJsonObject sec = it.value().toObject();

		QString path;
		if (section == "global")
			path = QString(TMPFS_DIR) + "/config";
		else
			path = QString("%1/%2.conf").arg(TMPFS_DIR, section);

		int fd = open(qPrintable(path), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) { perror(qPrintable(path)); continue; }

		for (auto si = sec.begin(); si != sec.end(); ++si)
			dprintf(fd, "%s=%s\n", qPrintable(si.key()),
				qPrintable(si.value().toString()));
		close(fd);
	}

	printf("Configs unlocked to %s\n", TMPFS_DIR);
	return 0;
}

static int cmdLock()
{
	QDir d(TMPFS_DIR);
	if (!d.exists()) {
		printf("Already locked (no %s)\n", TMPFS_DIR);
		return 0;
	}

	for (const QString &name : d.entryList(QDir::Files)) {
		QString path = d.filePath(name);

		/* Overwrite with zeros before unlinking */
		QFile f(path);
		qint64 sz = f.size();
		if (sz > 0) {
			int fd = open(qPrintable(path), O_WRONLY);
			if (fd >= 0) {
				auto *z = static_cast<char *>(calloc(1, sz));
				if (z) {
					(void)!write(fd, z, sz);
					free(z);
				}
				close(fd);
			}
		}
		QFile::remove(path);
	}
	rmdir(TMPFS_DIR);
	printf("Configs locked (wiped %s)\n", TMPFS_DIR);
	return 0;
}

static int cmdMigrate()
{
	QString cfgdir = configDir();

	if (vaultExists()) {
		fprintf(stderr, "error: vault already exists. Delete it first to re-migrate.\n");
		return 1;
	}

	QJsonObject data;

	/* Migrate global config */
	QFile gf(cfgdir + "/config");
	if (gf.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QJsonObject sec;
		while (!gf.atEnd()) {
			QString line = QString::fromUtf8(gf.readLine()).trimmed();
			if (line.startsWith('#') || line.isEmpty()) continue;
			int eq = line.indexOf('=');
			if (eq < 0) continue;
			QString key = line.left(eq);
			QString val = line.mid(eq + 1);
			if (!val.isEmpty())
				sec[key] = val;
		}
		if (!sec.isEmpty())
			data["global"] = sec;
	}

	/* Migrate plugin .conf files */
	QStringList plugins = {"weather", "github", "system"};
	for (const QString &pid : plugins) {
		QFile pf(cfgdir + "/" + pid + ".conf");
		if (!pf.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;

		QJsonObject sec;
		while (!pf.atEnd()) {
			QString line = QString::fromUtf8(pf.readLine()).trimmed();
			if (line.startsWith('#') || line.isEmpty()) continue;
			int eq = line.indexOf('=');
			if (eq < 0) continue;
			sec[line.left(eq)] = line.mid(eq + 1);
		}
		if (!sec.isEmpty())
			data[pid] = sec;
	}

	if (data.isEmpty()) {
		fprintf(stderr, "No configs found to migrate.\n");
		return 1;
	}

	printf("Found configs:\n");
	for (auto it = data.begin(); it != data.end(); ++it)
		printf("  [%s] %d key(s)\n", qPrintable(it.key()),
			static_cast<int>(it.value().toObject().count()));

	char pw1[MAX_PW_LEN], pw2[MAX_PW_LEN];
	readPassword("New vault password: ", pw1, sizeof(pw1));
	readPassword("Confirm password: ", pw2, sizeof(pw2));

	if (strcmp(pw1, pw2) != 0) { // LCOV_EXCL_START — same env var for both prompts
		fprintf(stderr, "error: passwords don't match\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	}
	if (strlen(pw1) < 8) {
		fprintf(stderr, "error: password must be at least 8 characters\n");
		sodium_memzero(pw1, sizeof(pw1));
		sodium_memzero(pw2, sizeof(pw2));
		return 1;
	} // LCOV_EXCL_STOP
	sodium_memzero(pw2, sizeof(pw2));

	int ret = vaultSave(data, pw1) < 0 ? 1 : 0;
	sodium_memzero(pw1, sizeof(pw1));

	if (ret == 0) {
		printf("Vault created. You can now remove plaintext configs:\n");
		printf("  rm %s/config %s/*.conf\n",
			qPrintable(cfgdir), qPrintable(cfgdir));
	}
	return ret;
}

/* ── Main ────────────────────────────────────────────────────── */

static void usage()
{
	fprintf(stderr,
		"Usage: gm-config <command> [args]\n\n"
		"Commands:\n"
		"  init                  Create encrypted vault\n"
		"  set <section> K=V    Set a config value\n"
		"  get <section> [KEY]  Read config value(s)\n"
		"  del <section> [KEY]  Delete section or key\n"
		"  list                 List configured sections\n"
		"  passwd               Change vault password\n"
		"  unlock               Decrypt configs to tmpfs\n"
		"  lock                 Wipe tmpfs configs\n"
		"  migrate              Import existing plaintext configs\n"
		"\nSections: global, weather, github, system, ...\n"
		"Example: gm-config set weather CITY=Goiania\n");
}

#ifndef GM_TESTING
int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 1; }

	if (sodium_init() < 0) { // LCOV_EXCL_START — system failure
		fprintf(stderr, "error: libsodium init failed\n");
		return 1;
	} // LCOV_EXCL_STOP

	const char *cmd = argv[1];

	if (strcmp(cmd, "init") == 0)
		return cmdInit();
	if (strcmp(cmd, "set") == 0) {
		if (argc < 4) { fprintf(stderr, "Usage: gm-config set <section> KEY=VALUE\n"); return 1; }
		return cmdSet(argv[2], argv[3]);
	}
	if (strcmp(cmd, "get") == 0) {
		if (argc < 3) { fprintf(stderr, "Usage: gm-config get <section> [KEY]\n"); return 1; }
		return cmdGet(argv[2], argc > 3 ? argv[3] : nullptr);
	}
	if (strcmp(cmd, "del") == 0) {
		if (argc < 3) { fprintf(stderr, "Usage: gm-config del <section> [KEY]\n"); return 1; }
		return cmdDel(argv[2], argc > 3 ? argv[3] : nullptr);
	}
	if (strcmp(cmd, "list") == 0)
		return cmdList();
	if (strcmp(cmd, "passwd") == 0)
		return cmdPasswd(); // LCOV_EXCL_LINE
	if (strcmp(cmd, "unlock") == 0)
		return cmdUnlock();
	if (strcmp(cmd, "lock") == 0)
		return cmdLock();
	if (strcmp(cmd, "migrate") == 0)
		return cmdMigrate(); // LCOV_EXCL_LINE

	fprintf(stderr, "Unknown command: %s\n", cmd);
	usage();
	return 1;
}
#endif /* GM_TESTING */
