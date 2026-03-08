/*
 * gm-plugin.h - GM Dashboard Plugin ABI (v2)
 *
 * Each plugin is a shared library (.so) installed in:
 *   ~/.local/lib/gm-dashboard/<id>.so
 *
 * Alongside an HTML card template:
 *   ~/.local/lib/gm-dashboard/<id>.html
 *
 * And an optional config file:
 *   ~/.config/gm-dashboard/<id>.conf
 *
 * Plugins MUST be signed (Ed25519) by the marketplace operator.
 * Signature file: <id>.so.sig (raw 64-byte detached signature).
 *
 * The host process calls curl_global_init() before loading plugins.
 * Plugins that use libcurl must NOT call curl_global_init() themselves.
 */
#ifndef GM_PLUGIN_H
#define GM_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#define GM_API_VERSION 2

/* Capability flags — each plugin declares what it needs */
#define GM_CAP_NETWORK    0x01  /* Outbound network (DNS + HTTPS)           */
#define GM_CAP_PROC       0x02  /* Read /proc filesystem                    */
#define GM_CAP_SHM        0x04  /* Read /dev/shm                            */
#define GM_CAP_EXEC       0x08  /* Execute external programs (popen/execve) */
#define GM_CAP_HOME_READ  0x10  /* Read files under $HOME                   */

struct gm_info {
	int         api_version;      /* Must be GM_API_VERSION                */
	const char *id;               /* Unique slug: "weather", "github"      */
	const char *name;             /* Display name: "WEATHER", "GITHUB"     */
	const char *version;          /* SemVer string: "1.0.0"                */
	const char *author;           /* Author / maintainer                   */
	int         order;            /* Display order (lower = further left)   */
	unsigned    caps_required;    /* Bitmask of GM_CAP_* needed            */
	const char *caps_description; /* Human-readable cap explanations       */
};

/*
 * Required exports — every .so MUST define all four:
 *
 * struct gm_info gm_info(void)
 *   Return plugin metadata. Called once after dlopen.
 *
 * int gm_init(const char *config_dir)
 *   Initialize plugin. config_dir = ~/.config/gm-dashboard/
 *   Read your own config from <config_dir>/<id>.conf
 *   Return 0 on success, -1 on error (plugin will be skipped).
 *
 * const char *gm_fetch(void)
 *   Fetch data. Return a JSON object string (owned by plugin,
 *   valid until gm_cleanup). Return NULL on error.
 *
 * void gm_cleanup(void)
 *   Free resources. Called once before dlclose.
 */
typedef struct gm_info (*gm_info_fn)(void);
typedef int            (*gm_init_fn)(const char *config_dir);
typedef const char    *(*gm_fetch_fn)(void);
typedef void           (*gm_cleanup_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* GM_PLUGIN_H */
