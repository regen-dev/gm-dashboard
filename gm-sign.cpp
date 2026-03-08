/*
 * gm-sign - GM Dashboard Plugin Signing Tool
 *
 * Ed25519 code signing via libsodium.
 * The marketplace operator uses this to sign .so plugin files.
 *
 * Usage:
 *   gm-sign keygen          Generate Ed25519 keypair
 *   gm-sign sign <file.so>  Sign a plugin (creates <file.so>.sig)
 *   gm-sign verify <file.so> Verify a plugin signature
 */
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <cstdio>
#include <cstring>
#include <sodium.h>

static QString keyDir()
{
	return QDir::homePath() + "/.config/gm-dashboard";
}

static QByteArray readBinary(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) return {};
	return f.readAll();
}

/* ── keygen ─────────────────────────────────────────────────── */

static int cmdKeygen()
{
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	unsigned char sk[crypto_sign_SECRETKEYBYTES];
	crypto_sign_keypair(pk, sk);

	QString dir = keyDir();
	QDir().mkpath(dir);

	/* Save secret key (0600) */
	QString secPath = dir + "/marketplace.sec";
	QFile sf(secPath);
	if (!sf.open(QIODevice::WriteOnly)) { // LCOV_EXCL_START — filesystem error
		fprintf(stderr, "Cannot write: %s\n", qPrintable(secPath));
		sodium_memzero(sk, sizeof(sk));
		return 1;
	} // LCOV_EXCL_STOP
	sf.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	sf.write(reinterpret_cast<const char *>(sk), sizeof(sk));
	sf.close();
	fprintf(stderr, "Secret key: %s\n", qPrintable(secPath));

	/* Save public key */
	QString pubPath = dir + "/marketplace.pub";
	QFile pf(pubPath);
	if (!pf.open(QIODevice::WriteOnly)) { // LCOV_EXCL_START — filesystem error
		fprintf(stderr, "Cannot write: %s\n", qPrintable(pubPath));
		sodium_memzero(sk, sizeof(sk));
		return 1;
	} // LCOV_EXCL_STOP
	pf.write(reinterpret_cast<const char *>(pk), sizeof(pk));
	pf.close();
	fprintf(stderr, "Public key: %s\n", qPrintable(pubPath));

	fprintf(stderr, "\nPublic key (hex): ");
	for (size_t i = 0; i < sizeof(pk); i++)
		fprintf(stderr, "%02x", pk[i]);
	fprintf(stderr, "\n");

	sodium_memzero(sk, sizeof(sk));
	return 0;
}

/* ── sign ───────────────────────────────────────────────────── */

static int cmdSign(const QString &soPath)
{
	QByteArray sk = readBinary(keyDir() + "/marketplace.sec");
	if (sk.size() != crypto_sign_SECRETKEYBYTES) { // LCOV_EXCL_START — no key
		fprintf(stderr, "Cannot read secret key. Run: gm-sign keygen\n");
		return 1;
	} // LCOV_EXCL_STOP

	QByteArray soData = readBinary(soPath);
	if (soData.isEmpty()) { // LCOV_EXCL_START — missing .so
		fprintf(stderr, "Cannot read: %s\n", qPrintable(soPath));
		sodium_memzero(sk.data(), sk.size());
		return 1;
	} // LCOV_EXCL_STOP

	unsigned char sig[crypto_sign_BYTES];
	crypto_sign_detached(sig, nullptr,
		reinterpret_cast<const unsigned char *>(soData.constData()),
		soData.size(),
		reinterpret_cast<const unsigned char *>(sk.constData()));

	sodium_memzero(sk.data(), sk.size());

	QString sigPath = soPath + ".sig";
	QFile f(sigPath);
	if (!f.open(QIODevice::WriteOnly)) { // LCOV_EXCL_START — filesystem error
		fprintf(stderr, "Cannot write: %s\n", qPrintable(sigPath));
		return 1;
	} // LCOV_EXCL_STOP
	f.write(reinterpret_cast<const char *>(sig), sizeof(sig));
	f.close();

	fprintf(stderr, "Signed: %s -> %s (%lld bytes)\n",
		qPrintable(soPath), qPrintable(sigPath), soData.size());
	return 0;
}

/* ── verify ─────────────────────────────────────────────────── */

static int cmdVerify(const QString &soPath)
{
	QByteArray pk = readBinary(keyDir() + "/marketplace.pub");
	if (pk.size() != crypto_sign_PUBLICKEYBYTES) { // LCOV_EXCL_START — no key
		fprintf(stderr, "Cannot read public key\n");
		return 1;
	} // LCOV_EXCL_STOP

	QByteArray soData = readBinary(soPath);
	if (soData.isEmpty()) { // LCOV_EXCL_START — missing .so
		fprintf(stderr, "Cannot read: %s\n", qPrintable(soPath));
		return 1;
	} // LCOV_EXCL_STOP

	QByteArray sig = readBinary(soPath + ".sig");
	if (sig.size() != crypto_sign_BYTES) { // LCOV_EXCL_START — no .sig
		fprintf(stderr, "No valid signature: %s.sig\n", qPrintable(soPath));
		return 1;
	} // LCOV_EXCL_STOP

	bool ok = crypto_sign_verify_detached(
		reinterpret_cast<const unsigned char *>(sig.constData()),
		reinterpret_cast<const unsigned char *>(soData.constData()),
		soData.size(),
		reinterpret_cast<const unsigned char *>(pk.constData())) == 0;

	fprintf(stderr, "%s: %s\n", qPrintable(soPath), ok ? "VALID" : "INVALID");
	return ok ? 0 : 1;
}

/* ── main ───────────────────────────────────────────────────── */

#ifndef GM_TESTING
int main(int argc, char **argv)
{
	if (sodium_init() < 0) { // LCOV_EXCL_START — system failure
		fprintf(stderr, "libsodium init failed\n");
		return 1;
	} // LCOV_EXCL_STOP

	if (argc < 2) goto usage; // LCOV_EXCL_LINE

	if (strcmp(argv[1], "keygen") == 0)
		return cmdKeygen();
	if (strcmp(argv[1], "sign") == 0 && argc >= 3)
		return cmdSign(QString::fromLocal8Bit(argv[2]));
	if (strcmp(argv[1], "verify") == 0 && argc >= 3)
		return cmdVerify(QString::fromLocal8Bit(argv[2]));

usage: // LCOV_EXCL_LINE
	fprintf(stderr,
		"gm-sign - GM Dashboard plugin signing tool\n\n"
		"Usage:\n"
		"  gm-sign keygen           Generate Ed25519 keypair\n"
		"  gm-sign sign <file.so>   Sign a plugin\n"
		"  gm-sign verify <file.so> Verify a plugin signature\n\n"
		"Keys stored in: ~/.config/gm-dashboard/marketplace.{sec,pub}\n");
	return 1;
}
#endif /* GM_TESTING */
