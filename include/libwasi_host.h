/*
 * libwasi_host.h — Awesome-OS Universal WASI Host Launcher & Host-Bindings ABI
 *
 * Canonical ABI Magic: B8G_WASI_HOST_ABI_MAGIC_V3 (0x33564742u, "B8GV3")
 * Standard: C11 (-std=c11 -Os -Wall -Wextra -Werror), zero external dependencies.
 */

#ifndef B8G_LIBWASI_HOST_H
#define B8G_LIBWASI_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Export visibility for shared library (.dll / .so / .dylib) and static builds. */
#if defined(_WIN32)
#  if defined(B8G_WASI_HOST_BUILD_DLL)
#    define B8G_WASI_HOST_API __declspec(dllexport)
#  elif defined(B8G_WASI_HOST_USE_DLL)
#    define B8G_WASI_HOST_API __declspec(dllimport)
#  else
#    define B8G_WASI_HOST_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define B8G_WASI_HOST_API __attribute__((visibility("default")))
#else
#  define B8G_WASI_HOST_API
#endif

/** Canonical B8G ABI v3 magic ("B8GV3" in little-endian: 0x33564742u). */
#define B8G_WASI_HOST_ABI_MAGIC_V3 0x33564742u

/** SHA-512 hex digest length in ASCII characters. */
#define B8G_WASI_HOST_SHA512_HEX_LEN 128u

/** Standard WebAssembly binary header size (\0asm + 4-byte version). */
#define B8G_WASI_HOST_WASM_HEADER_LEN 8u

/** Return codes for libwasi_host functions. */
typedef enum b8g_wasi_host_status {
    B8G_WASI_HOST_OK           =  0,
    B8G_WASI_HOST_ERR_INVALID  = -1,
    B8G_WASI_HOST_ERR_ENV      = -2,
    B8G_WASI_HOST_ERR_OVERFLOW = -3,
    B8G_WASI_HOST_ERR_IO       = -4,
    B8G_WASI_HOST_ERR_NOTFOUND = -5
} b8g_wasi_host_status_t;

/**
 * Returns the canonical B8G ABI v3 magic constant (0x33564742u).
 */
B8G_WASI_HOST_API uint32_t b8g_wasi_host_abi_magic(void);

/**
 * Returns the compile-time target OS string ("windows", "linux", "darwin", "unknown").
 */
B8G_WASI_HOST_API const char* b8g_wasi_host_os(void);

/**
 * Returns the compile-time target architecture string ("x64", "arm64", "x86", "unknown").
 */
B8G_WASI_HOST_API const char* b8g_wasi_host_arch(void);

/**
 * Resolves the v11 CAS root directory:
 *   - If B8G_GHOST_CACHE_DIR is set and non-empty: <B8G_GHOST_CACHE_DIR>/v11
 *   - Otherwise: <HOME>/.b8g/cas/v11 (using USERPROFILE or HOME on Win32, HOME on POSIX)
 *
 * Returns B8G_WASI_HOST_OK (0) on success, or a negative b8g_wasi_host_status_t code.
 */
B8G_WASI_HOST_API int b8g_wasi_host_resolve_cas_root(char* out, size_t cap);

/**
 * Extracts the logical app name from argv[0]:
 *   - Strips directory prefixes (both '/' and '\\').
 *   - Strips a trailing ".exe" suffix (ASCII case-insensitive).
 *   - If the resulting stem is "wasi-host" or begins with "wasi-host-"
 *     (ASCII case-insensitive), writes "" (empty string) to out.
 *
 * Example: "D:\\pnpm\\bin\\godot.exe" -> "godot"
 *          "wasi-host.exe"            -> ""
 *
 * Returns B8G_WASI_HOST_OK (0) on success, or a negative b8g_wasi_host_status_t code.
 */
B8G_WASI_HOST_API int b8g_wasi_host_extract_app_name(const char* argv0, char* out, size_t cap);

/**
 * Formats a v11 content-addressed blob path:
 *   <cas_v11_root>/files/<first2>/<sha512>[-exec]
 *
 * Validates that sha512_hex is exactly 128 ASCII hexadecimal characters.
 * Hex characters in the emitted path are normalized to lowercase.
 *
 * Returns B8G_WASI_HOST_OK (0) on success, or a negative b8g_wasi_host_status_t code.
 */
B8G_WASI_HOST_API int b8g_wasi_host_v11_blob_path(
    const char* cas_v11_root,
    const char* sha512_hex,
    int is_exec,
    char* out,
    size_t cap
);

/**
 * Validates the 8-byte WebAssembly binary header:
 *   magic   = 0x00, 0x61, 0x73, 0x6D ("\0asm")
 *   version = 0x01, 0x00, 0x00, 0x00 (version 1)
 *
 * Returns 1 if valid, 0 otherwise.
 */
B8G_WASI_HOST_API int b8g_wasi_host_validate_wasm_header(const uint8_t* bytes, size_t len);

/**
 * Creates a zero-extra-byte hardlink at link_path pointing to target_exe:
 *   - Win32: CreateHardLinkA(link_path, target_exe, NULL)
 *   - POSIX: link(target_exe, link_path)
 *
 * Returns B8G_WASI_HOST_OK (0) on success, or a negative b8g_wasi_host_status_t code.
 */
B8G_WASI_HOST_API int b8g_wasi_host_stamp_hardlink(const char* target_exe, const char* link_path);

/**
 * Resolves an app's .wasm module path inside cas_v11_root (Tier-1 CAS lookup):
 *   1. Checks <cas_v11_root>/apps/<app_name>.sha512 (128-char hex, optional "-exec" suffix)
 *      and maps it via b8g_wasi_host_v11_blob_path(), verifying the blob exists.
 *   2. Falls back to direct <cas_v11_root>/apps/<app_name>.wasm if present.
 *
 * Returns B8G_WASI_HOST_OK (0) if found, B8G_WASI_HOST_ERR_NOTFOUND if absent,
 * or another negative status code on error.
 */
B8G_WASI_HOST_API int b8g_wasi_host_resolve_app_wasm(
    const char* cas_v11_root,
    const char* app_name,
    char* out_path,
    size_t cap
);

#ifdef __cplusplus
}
#endif

#endif /* B8G_LIBWASI_HOST_H */
