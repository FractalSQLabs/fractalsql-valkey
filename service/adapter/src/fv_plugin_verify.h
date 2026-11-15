/* SPDX-License-Identifier: Apache-2.0 */
/* fv_plugin_verify.h -- authenticity gate for the reasoning plugin.
 *
 * The reasoning plugin is a native shared object the daemon maps into its
 * own process: loading it is arbitrary code execution in the daemon.
 * The engine checks the plugin's path and its ABI, but not where the
 * file came from, so this gate applies the same trust rule the
 * enterprise core already uses before its own library is mapped:
 *
 *   1. FSQL_REASONING_REQUIRE_SIGNATURE=0 in the environment is the
 *      operator's opt-out: a plugin with no .sig loads, and one warning
 *      goes to stderr. A .sig that is present is always checked, and a
 *      wrong one is always refused.
 *   2. Otherwise (the default) a detached Ed25519 signature -- a sibling <path>.sig
 *      file of exactly 64 raw bytes -- must verify against the
 *      FractalSQLabs reasoning-plugin signing public key (a different
 *      key from the one that signs the enterprise core).
 *
 * Anything else is refused: the plugin is not loaded, and the tier that
 * wanted it reports the refusal.
 *
 * fv_plugin_gate() runs before the load and fv_plugin_gate_after_load()
 * runs after it, so a file swapped between the check and the load is
 * caught rather than quietly mapped.
 */
#ifndef FV_PLUGIN_VERIFY_H
#define FV_PLUGIN_VERIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verifies the plugin at `path`. Returns 0 when it may be loaded, -1
 * when it must not be -- with the reason written to `err` (which is
 * always NUL-terminated when err_cap > 0). */
int fv_plugin_gate(const char *path, char *err, size_t err_cap);

/* Re-reads the plugin after the load and refuses when the file is no
 * longer the one that was verified. Returns 0 when the file is
 * unchanged, -1 otherwise -- with the reason written to `err`. */
int fv_plugin_gate_after_load(const char *path, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* FV_PLUGIN_VERIFY_H */
