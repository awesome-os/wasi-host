/*
 * wasi_host.c — Universal WASI Host Launcher CLI (wasi-host-<os>-<arch>)
 *
 * Collapses N apps x 5 OS native launchers into 1 tiny native host + N portable .wasm modules.
 * Hardlinking wasi-host as <app>[.exe] consumes 0 extra bytes on disk.
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
#  include <unistd.h>
#endif

static void print_json_escaped(const char* s) {
    putchar('"');
    if (s != NULL) {
        for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; ++p) {
            switch (*p) {
                case '"':  fputs("\\\"", stdout); break;
                case '\\': fputs("\\\\", stdout); break;
                case '\b': fputs("\\b", stdout);  break;
                case '\f': fputs("\\f", stdout);  break;
                case '\n': fputs("\\n", stdout);  break;
                case '\r': fputs("\\r", stdout);  break;
                case '\t': fputs("\\t", stdout);  break;
                default:
                    if (*p < 0x20u) {
                        printf("\\u%04x", (unsigned)*p);
                    } else {
                        putchar((int)*p);
                    }
                    break;
            }
        }
    }
    putchar('"');
}

static int resolve_self_exe(const char* argv0, char* out, size_t cap) {
    if (out == NULL || cap == 0) {
        return B8G_WASI_HOST_ERR_INVALID;
    }
#if defined(_WIN32)
    (void)argv0;
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (len == 0 || len >= (DWORD)cap) {
        return B8G_WASI_HOST_ERR_IO;
    }
    out[len] = '\0';
    return B8G_WASI_HOST_OK;
#else
    ssize_t len = readlink("/proc/self/exe", out, cap - 1);
    if (len > 0 && (size_t)len < cap) {
        out[len] = '\0';
        return B8G_WASI_HOST_OK;
    }
    if (argv0 == NULL || strlen(argv0) + 1 > cap) {
        return B8G_WASI_HOST_ERR_IO;
    }
    strcpy(out, argv0);
    return B8G_WASI_HOST_OK;
#endif
}

static void print_info_json(const char* app_name, const char* cas_root) {
    printf("{\"abiMagic\":\"0x%08X\",\"appName\":", (unsigned)b8g_wasi_host_abi_magic());
    print_json_escaped(app_name);
    fputs(",\"casV11Root\":", stdout);
    print_json_escaped(cas_root);
    fputs(",\"os\":", stdout);
    print_json_escaped(b8g_wasi_host_os());
    fputs(",\"arch\":", stdout);
    print_json_escaped(b8g_wasi_host_arch());
    fputs("}\n", stdout);
}

static void print_usage(void) {
    fputs(
        "Usage:\n"
        "  wasi-host --wasi-host-info\n"
        "  wasi-host --stamp <app-name> <dest-dir>\n"
        "  wasi-host <app-name> [args...]\n"
        "  <app-name>[.exe] [args...]   (0-byte hardlink to wasi-host)\n",
        stderr
    );
}

static int handle_stamp(const char* argv0, const char* app_name, const char* dest_dir) {
    if (app_name == NULL || app_name[0] == '\0' || dest_dir == NULL || dest_dir[0] == '\0') {
        fputs("wasi-host: --stamp requires <app-name> <dest-dir>\n", stderr);
        return 1;
    }

    char self_path[1024];
    if (resolve_self_exe(argv0, self_path, sizeof(self_path)) != B8G_WASI_HOST_OK) {
        fputs("wasi-host: failed to resolve current executable path\n", stderr);
        return 1;
    }

    size_t dlen = strlen(dest_dir);
    while (dlen > 1 && (dest_dir[dlen - 1] == '/' || dest_dir[dlen - 1] == '\\')) {
        if (dlen == 3 && dest_dir[1] == ':') {
            break;
        }
        --dlen;
    }

    char link_path[1024];
#if defined(_WIN32)
    const char* ext = ".exe";
    const char sep = '\\';
#else
    const char* ext = "";
    const char sep = '/';
#endif
    int n;
    if (dlen > 0 && (dest_dir[dlen - 1] == '/' || dest_dir[dlen - 1] == '\\')) {
        n = snprintf(link_path, sizeof(link_path), "%.*s%s%s", (int)dlen, dest_dir, app_name, ext);
    } else {
        n = snprintf(link_path, sizeof(link_path), "%.*s%c%s%s", (int)dlen, dest_dir, sep, app_name, ext);
    }
    if (n < 0 || (size_t)n >= sizeof(link_path)) {
        fputs("wasi-host: destination path too long\n", stderr);
        return 1;
    }

    if (b8g_wasi_host_stamp_hardlink(self_path, link_path) != B8G_WASI_HOST_OK) {
        fprintf(stderr, "wasi-host: failed to hardlink '%s' -> '%s'\n", self_path, link_path);
        return 1;
    }

    printf("{\"stamped\":true,\"appName\":");
    print_json_escaped(app_name);
    fputs(",\"linkPath\":", stdout);
    print_json_escaped(link_path);
    fputs(",\"extraBytes\":0}\n", stdout);
    return 0;
}

/*
 * 3-Tier Lookup & Dispatch Plan:
 *   Tier 1: v11 CAS lookup (<cas_v11_root>/apps/<app>.sha512 -> v11/files/<first2>/<sha512>[-exec],
 *           or direct <cas_v11_root>/apps/<app>.wasm)
 *   Tier 2: Install-if-missing hook (honors B8G_WASI_INSTALL_HOOK if configured; otherwise reports
 *           structured install-if-missing miss)
 *   Tier 3: .wasm binary header validation (\0asm\1\0\0\0) & B8G_ABI_MAGIC_V3 dispatch readiness
 */
static int run_app_3tier(const char* cas_root, const char* app_name, int app_argc, char** app_argv) {
    (void)app_argc;
    (void)app_argv;

    char wasm_path[1024];
    int rc = b8g_wasi_host_resolve_app_wasm(cas_root, app_name, wasm_path, sizeof(wasm_path));

    /* Tier 2: Install-if-missing hook */
    if (rc == B8G_WASI_HOST_ERR_NOTFOUND) {
        const char* hook = getenv("B8G_WASI_INSTALL_HOOK");
        if (hook != NULL && hook[0] != '\0') {
            /*
             * Hook protocol: callers may point B8G_WASI_INSTALL_HOOK to an installer
             * that populates <cas_v11_root>/apps/<app_name>.sha512 or .wasm.
             * Re-check CAS after reporting hook trigger.
             */
            fprintf(stderr, "wasi-host: tier2 install-if-missing hook '%s' requested for '%s'\n",
                    hook, app_name);
            rc = b8g_wasi_host_resolve_app_wasm(cas_root, app_name, wasm_path, sizeof(wasm_path));
        }
    }

    if (rc != B8G_WASI_HOST_OK) {
        fprintf(
            stderr,
            "wasi-host: app '%s' not found in v11 CAS '%s' (tier1 miss, tier2 not installed)\n",
            app_name,
            cas_root
        );
        return 2;
    }

    /* Tier 3: .wasm header validation & dispatch */
    FILE* fp = fopen(wasm_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "wasi-host: unable to open wasm module '%s'\n", wasm_path);
        return 3;
    }

    uint8_t header[B8G_WASI_HOST_WASM_HEADER_LEN];
    size_t nread = fread(header, 1, sizeof(header), fp);
    fclose(fp);

    if (!b8g_wasi_host_validate_wasm_header(header, nread)) {
        fprintf(stderr, "wasi-host: invalid wasm header in '%s'\n", wasm_path);
        return 4;
    }

    printf("{\"abiMagic\":\"0x%08X\",\"status\":\"ready\",\"appName\":",
           (unsigned)b8g_wasi_host_abi_magic());
    print_json_escaped(app_name);
    fputs(",\"wasmPath\":", stdout);
    print_json_escaped(wasm_path);
    fputs(",\"wasmValid\":true}\n", stdout);
    return 0;
}

int main(int argc, char** argv) {
    char app_name[256];
    char cas_root[1024];

    const char* argv0 = (argc > 0 && argv[0] != NULL) ? argv[0] : "wasi-host";
    if (b8g_wasi_host_extract_app_name(argv0, app_name, sizeof(app_name)) != B8G_WASI_HOST_OK) {
        fputs("wasi-host: failed to parse argv[0]\n", stderr);
        return 1;
    }

    if (b8g_wasi_host_resolve_cas_root(cas_root, sizeof(cas_root)) != B8G_WASI_HOST_OK) {
        fputs("wasi-host: failed to resolve v11 CAS root\n", stderr);
        return 1;
    }

    /* Handle host control flags regardless of whether invoked directly or via hardlink. */
    if (argc >= 2 && strcmp(argv[1], "--wasi-host-info") == 0) {
        print_info_json(app_name, cas_root);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--stamp") == 0) {
        if (argc < 4) {
            fputs("wasi-host: --stamp requires <app-name> <dest-dir>\n", stderr);
            return 1;
        }
        return handle_stamp(argv0, argv[2], argv[3]);
    }

    if (app_name[0] == '\0') {
        /* Direct invocation: wasi-host <app-name> [args...] */
        if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return (argc >= 2) ? 0 : 1;
        }
        if (argc >= 3 && strcmp(argv[2], "--wasi-host-info") == 0) {
            print_info_json(argv[1], cas_root);
            return 0;
        }
        return run_app_3tier(cas_root, argv[1], argc - 2, argv + 2);
    }

    /* Hardlinked invocation: <app-name>[.exe] [args...] */
    return run_app_3tier(cas_root, app_name, argc - 1, argv + 1);
}
