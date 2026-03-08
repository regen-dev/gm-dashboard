/* Stub .so — exports gm_* symbols but with wrong API version.
 * Used to test loadPlugins "API version mismatch" error path. */
#include "../gm-plugin.h"

extern "C" {

struct gm_info gm_info(void) {
    return {
        .api_version = 999, /* wrong version */
        .id = "stub_badver",
        .name = "Bad Version Stub",
        .version = "0.0.0",
        .order = 1,
        .caps_required = 0,
        .caps_description = "none"
    };
}

int gm_init(const char *) { return 0; }
const char *gm_fetch(void) { return "{}"; }
void gm_cleanup(void) {}

}
