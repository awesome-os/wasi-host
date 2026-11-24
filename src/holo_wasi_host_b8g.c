/*
 * holo_wasi_host_b8g.c — Universal Portable wasm32-wasi Core (holo-wasi-host-b8g.wasm)
 *
 * Canonical ABI Magic: B8G_ABI_MAGIC_V3 (0x33564742u, "B8GV3")
 * CAS Layout: v11
 * Standard: C11 (-std=c11 -Os -s -Wall -Wextra -Werror), zero external dependencies.
 */

#include <stdint.h>
#include <stdio.h>

#define HOLO_WASI_B8G_ABI_MAGIC 0x33564742u
#define HOLO_WASI_B8G_VERSION   2u

#if defined(__wasm__)
#  define HOLO_WASI_EXPORT(name) __attribute__((export_name(name)))
#else
#  define HOLO_WASI_EXPORT(name)
#endif

HOLO_WASI_EXPORT("holo_wasi_b8g_abi_magic")
uint32_t holo_wasi_b8g_abi_magic(void) {
    return HOLO_WASI_B8G_ABI_MAGIC;
}

HOLO_WASI_EXPORT("holo_wasi_b8g_version")
uint32_t holo_wasi_b8g_version(void) {
    return HOLO_WASI_B8G_VERSION;
}

HOLO_WASI_EXPORT("holo_wasi_b8g_route_app")
uint32_t holo_wasi_b8g_route_app(uint32_t app_hash) {
    if (app_hash == 0u) {
        return 0u;
    }
    return (app_hash ^ HOLO_WASI_B8G_ABI_MAGIC) | 1u;
}

int main(void) {
    fputs("{\"component\":\"holo-wasi-host-b8g\",\"abiMagic\":\"0x33564742\",\"casLayout\":\"v11\"}\n", stdout);
    return 0;
}
