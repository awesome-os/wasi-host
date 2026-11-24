/*
 * libwasi_host.c — Core implementation of libwasi-host (B8G_ABI_MAGIC_V3)
 *
 * Strictly C11, zero external dependencies.
 */

#include "libwasi_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

static int ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int ascii_casecmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        int ca = ascii_tolower((unsigned char)*a);
        int cb = ascii_tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return ascii_tolower((unsigned char)*a) - ascii_tolower((unsigned char)*b);
}

static int ascii_ncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int ca = ascii_tolower((unsigned char)a[i]);
        int cb = ascii_tolower((unsigned char)b[i]);
        if (ca != cb) {
            return ca - cb;
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

static int is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static size_t trim_trailing_seps(const char* path, size_t len) {
    while (len > 1 && is_sep(path[len - 1])) {
        /* Keep drive root like "C:\" intact on Windows. */
        if (len == 3 && path[1] == ':') {
            break;
        }
        --len;
    }
    return len;
}

static int file_exists(const char* path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
#endif
}

B8G_WASI_HOST_API uint32_t b8g_wasi_host_abi_magic(void) {
    return B8G_WASI_HOST_ABI_MAGIC_V3;
}

B8G_WASI_HOST_API const char* b8g_wasi_host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

B8G_WASI_HOST_API const char* b8g_wasi_host_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

B8G_WASI_HOST_API int b8g_wasi_host_resolve_cas_root(char* out, size_t cap) {
    if (out == NULL || cap == 0) {
        return B8G_WASI_HOST_ERR_INVALID;
    }

    const char* ghost_dir = getenv("B8G_GHOST_CACHE_DIR");
    if (ghost_dir != NULL && ghost_dir[0] != '\0') {
        size_t base_len = trim_trailing_seps(ghost_dir, strlen(ghost_dir));
        int written;
        if (base_len > 0 && is_sep(ghost_dir[base_len - 1])) {
            written = snprintf(out, cap, "%.*sv11", (int)base_len, ghost_dir);
        } else {
            written = snprintf(out, cap, "%.*s/v11", (int)base_len, ghost_dir);
        }
        if (written < 0 || (size_t)written >= cap) {
            out[0] = '\0';
            return B8G_WASI_HOST_ERR_OVERFLOW;
        }
        return B8G_WASI_HOST_OK;
    }

    const char* home = NULL;
#if defined(_WIN32)
    home = getenv("USERPROFILE");
    if (home == NULL || home[0] == '\0') {
        home = getenv("HOME");
    }
#else
    home = getenv("HOME");
#endif
    if (home == NULL || home[0] == '\0') {
        out[0] = '\0';
        return B8G_WASI_HOST_ERR_ENV;
    }

    size_t home_len = trim_trailing_seps(home, strlen(home));
    int written;
    if (home_len > 0 && is_sep(home[home_len - 1])) {
        written = snprintf(out, cap, "%.*s.b8g/cas/v11", (int)home_len, home);
    } else {
        written = snprintf(out, cap, "%.*s/.b8g/cas/v11", (int)home_len, home);
    }
    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return B8G_WASI_HOST_ERR_OVERFLOW;
    }
    return B8G_WASI_HOST_OK;
}

B8G_WASI_HOST_API int b8g_wasi_host_extract_app_name(const char* argv0, char* out, size_t cap) {
    if (argv0 == NULL || out == NULL || cap == 0) {
        return B8G_WASI_HOST_ERR_INVALID;
    }

    /* Locate basename after last '/' or '\\'. */
    const char* base = argv0;
    for (const char* p = argv0; *p != '\0'; ++p) {
        if (is_sep(*p)) {
            base = p + 1;
        }
    }

    size_t len = strlen(base);
    if (len == 0) {
        out[0] = '\0';
        return B8G_WASI_HOST_OK;
    }

    /* Strip trailing ".exe" (ASCII case-insensitive). */
    if (len >= 4 && ascii_casecmp(base + (len - 4), ".exe") == 0) {
        len -= 4;
    }

    if (len == 0) {
        out[0] = '\0';
        return B8G_WASI_HOST_OK;
    }

    /* Direct invocation as "wasi-host", "holo-wasi-host", or "<prefix>-<os>-<arch>" yields "". */
    if ((len == 9 && ascii_ncasecmp(base, "wasi-host", 9) == 0) ||
        (len > 10 && ascii_ncasecmp(base, "wasi-host-", 10) == 0) ||
        (len == 14 && ascii_ncasecmp(base, "holo-wasi-host", 14) == 0) ||
        (len > 15 && ascii_ncasecmp(base, "holo-wasi-host-", 15) == 0)) {
        out[0] = '\0';
        return B8G_WASI_HOST_OK;
    }

    if (len + 1 > cap) {
        out[0] = '\0';
        return B8G_WASI_HOST_ERR_OVERFLOW;
    }

    memcpy(out, base, len);
    out[len] = '\0';
    return B8G_WASI_HOST_OK;
}

B8G_WASI_HOST_API int b8g_wasi_host_v11_blob_path(
    const char* cas_v11_root,
    const char* sha512_hex,
    int is_exec,
    char* out,
    size_t cap
) {
    if (cas_v11_root == NULL || cas_v11_root[0] == '\0' ||
        sha512_hex == NULL || out == NULL || cap == 0) {
        return B8G_WASI_HOST_ERR_INVALID;
    }

    size_t hex_len = strlen(sha512_hex);
    if (hex_len != B8G_WASI_HOST_SHA512_HEX_LEN) {
        out[0] = '\0';
        return B8G_WASI_HOST_ERR_INVALID;
    }

    char norm_hex[B8G_WASI_HOST_SHA512_HEX_LEN + 1];
    for (size_t i = 0; i < B8G_WASI_HOST_SHA512_HEX_LEN; ++i) {
        if (!is_hex_char(sha512_hex[i])) {
            out[0] = '\0';
            return B8G_WASI_HOST_ERR_INVALID;
        }
        norm_hex[i] = (char)ascii_tolower((unsigned char)sha512_hex[i]);
    }
    norm_hex[B8G_WASI_HOST_SHA512_HEX_LEN] = '\0';

    size_t root_len = trim_trailing_seps(cas_v11_root, strlen(cas_v11_root));
    const char* suffix = is_exec ? "-exec" : "";
    int written;
    if (root_len > 0 && is_sep(cas_v11_root[root_len - 1])) {
        written = snprintf(
            out,
            cap,
            "%.*sfiles/%c%c/%s%s",
            (int)root_len,
            cas_v11_root,
            norm_hex[0],
            norm_hex[1],
            norm_hex,
            suffix
        );
    } else {
        written = snprintf(
            out,
            cap,
            "%.*s/files/%c%c/%s%s",
            (int)root_len,
            cas_v11_root,
            norm_hex[0],
            norm_hex[1],
            norm_hex,
            suffix
        );
    }

    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return B8G_WASI_HOST_ERR_OVERFLOW;
    }
    return B8G_WASI_HOST_OK;
}

B8G_WASI_HOST_API int b8g_wasi_host_validate_wasm_header(const uint8_t* bytes, size_t len) {
    if (bytes == NULL || len < B8G_WASI_HOST_WASM_HEADER_LEN) {
        return 0;
    }
    static const uint8_t kWasmHeader[B8G_WASI_HOST_WASM_HEADER_LEN] = {
        0x00u, 0x61u, 0x73u, 0x6Du, /* \0asm */
        0x01u, 0x00u, 0x00u, 0x00u  /* version 1 */
    };
    return memcmp(bytes, kWasmHeader, sizeof(kWasmHeader)) == 0 ? 1 : 0;
}

B8G_WASI_HOST_API int b8g_wasi_host_stamp_hardlink(const char* target_exe, const char* link_path) {
    if (target_exe == NULL || target_exe[0] == '\0' ||
        link_path == NULL || link_path[0] == '\0') {
        return B8G_WASI_HOST_ERR_INVALID;
    }
#if defined(_WIN32)
    /* Win32 signature: CreateHardLinkA(lpFileName [new], lpExistingFileName [target], NULL). */
    if (!CreateHardLinkA(link_path, target_exe, NULL)) {
        return B8G_WASI_HOST_ERR_IO;
    }
    return B8G_WASI_HOST_OK;
#else
    if (link(target_exe, link_path) != 0) {
        return B8G_WASI_HOST_ERR_IO;
    }
    return B8G_WASI_HOST_OK;
#endif
}

B8G_WASI_HOST_API int b8g_wasi_host_resolve_app_wasm(
    const char* cas_v11_root,
    const char* app_name,
    char* out_path,
    size_t cap
) {
    if (cas_v11_root == NULL || cas_v11_root[0] == '\0' ||
        app_name == NULL || app_name[0] == '\0' ||
        out_path == NULL || cap == 0) {
        return B8G_WASI_HOST_ERR_INVALID;
    }

    /* Reject path traversal characters in app_name. */
    for (const char* p = app_name; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            out_path[0] = '\0';
            return B8G_WASI_HOST_ERR_INVALID;
        }
    }

    size_t root_len = trim_trailing_seps(cas_v11_root, strlen(cas_v11_root));
    char index_path[1024];
    int n = snprintf(
        index_path,
        sizeof(index_path),
        "%.*s/apps/%s.sha512",
        (int)root_len,
        cas_v11_root,
        app_name
    );
    if (n > 0 && (size_t)n < sizeof(index_path)) {
        FILE* fp = fopen(index_path, "rb");
        if (fp != NULL) {
            char buf[256];
            size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[rd] = '\0';

            /* Trim trailing CR/LF/spaces. */
            while (rd > 0 && (buf[rd - 1] == '\n' || buf[rd - 1] == '\r' ||
                              buf[rd - 1] == ' ' || buf[rd - 1] == '\t')) {
                buf[--rd] = '\0';
            }

            int is_exec = 0;
            if (rd == B8G_WASI_HOST_SHA512_HEX_LEN + 5 &&
                strcmp(buf + B8G_WASI_HOST_SHA512_HEX_LEN, "-exec") == 0) {
                buf[B8G_WASI_HOST_SHA512_HEX_LEN] = '\0';
                is_exec = 1;
            }

            int rc = b8g_wasi_host_v11_blob_path(cas_v11_root, buf, is_exec, out_path, cap);
            if (rc == B8G_WASI_HOST_OK && file_exists(out_path)) {
                return B8G_WASI_HOST_OK;
            }
        }
    }

    /* Fallback: direct <cas_v11_root>/apps/<app_name>.wasm */
    int wn = snprintf(
        out_path,
        cap,
        "%.*s/apps/%s.wasm",
        (int)root_len,
        cas_v11_root,
        app_name
    );
    if (wn < 0 || (size_t)wn >= cap) {
        out_path[0] = '\0';
        return B8G_WASI_HOST_ERR_OVERFLOW;
    }
    if (file_exists(out_path)) {
        return B8G_WASI_HOST_OK;
    }

    out_path[0] = '\0';
    return B8G_WASI_HOST_ERR_NOTFOUND;
}
