// SPDX-License-Identifier: GPL-3.0-only
// The CLAP entry point for clap-wrapper's "clap-first" build: clap-wrapper compiles this file once for each format (CLAP, VST3, AUv2)
// and links it to the plugin's static library, which has the three functions below (plugin/clap/cultbox_clap.c).
#include <clap/clap.h>

extern "C" {
bool cultbox_entry_init(const char *path);
void cultbox_entry_deinit(void);
const void *cultbox_entry_get_factory(const char *id);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
CLAP_EXPORT const struct clap_plugin_entry clap_entry = {CLAP_VERSION, cultbox_entry_init, cultbox_entry_deinit, cultbox_entry_get_factory};
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
