/*
 * wasi_host_test.c — Unit & live integration tests for libwasi-host and wasi-host
 *
 * Tests:
 *   - B8G_WASI_HOST_ABI_MAGIC_V3 (0x33564742u)
 *   - b8g_wasi_host_resolve_cas_root (B8G_GHOST_CACHE_DIR + HOME fallback)
 *   - b8g_wasi_host_extract_app_name (Win32 paths, POSIX paths, .exe, wasi-host)
 *   - b8g_wasi_host_v11_blob_path (regular + -exec + validation)
 *   - b8g_wasi_host_validate_wasm_header (valid, bad magic, bad version, truncated, NULL)
 *   - b8g_wasi_host_stamp_hardlink (real 0-byte NTFS/POSIX hardlink + file-index/inode proof)
 *   - Live wasi-host.exe --wasi-host-info, --stamp, and 3-tier CAS .wasm validation
 */

#include "libwasi_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <direct.h>
#  include <windows.h>
#  define MKDIR_P(p) _mkdir(p)
#  define POPEN _popen
#  define PCLOSE _pclose
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define MKDIR_P(p) mkdir((p), 0755)
#  define POPEN popen
#  define PCLOSE pclose
#endif

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                      \
    do {                                                                            \
        if (cond) {                                                                 \
            ++g_passed;                                                             \
        } else {                                                                    \
            ++g_failed;                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
        }                                                                           \
    } while (0)

#define ASSERT_STR_EQ(actual, expected, msg)                                        \
    do {                                                                            \
        if ((actual) != NULL && strcmp((actual), (expected)) == 0) {                \
            ++g_passed;                                                             \
        } else {                                                                    \
            ++g_failed;                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s (got '%s', expected '%s')\n",         \
                    __FILE__, __LINE__, (msg),                                      \
                    (actual) ? (actual) : "(null)", (expected));                    \
        }                                                                           \
    } while (0)

static void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value);
    if (value != NULL) {
        _putenv_s(name, value);
    } else {
        _putenv_s(name, "");
    }
#else
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

static int run_cmd_capture(const char* cmd, char* out, size_t cap) {
#if defined(_WIN32)
    /* cmd.exe /c strips the first and last quote when a command starts and ends with '"'. */
    char wrapped[2048];
    snprintf(wrapped, sizeof(wrapped), "\"%s\"", cmd);
    FILE* pipe = POPEN(wrapped, "r");
#else
    FILE* pipe = POPEN(cmd, "r");
#endif
    if (pipe == NULL) {
        return -1;
    }
    size_t total = 0;
    while (total + 1 < cap) {
        size_t n = fread(out + total, 1, cap - 1 - total, pipe);
        if (n == 0) {
            break;
        }
        total += n;
    }
    out[total] = '\0';
    return PCLOSE(pipe);
}

static int verify_same_hardlink_inode(const char* path_a, const char* path_b, unsigned long* out_links) {
#if defined(_WIN32)
    HANDLE ha = CreateFileA(path_a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hb = CreateFileA(path_b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (ha == INVALID_HANDLE_VALUE || hb == INVALID_HANDLE_VALUE) {
        if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
        if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION ia, ib;
    BOOL oka = GetFileInformationByHandle(ha, &ia);
    BOOL okb = GetFileInformationByHandle(hb, &ib);
    CloseHandle(ha);
    CloseHandle(hb);
    if (!oka || !okb) {
        return 0;
    }
    if (out_links != NULL) {
        *out_links = (unsigned long)ib.nNumberOfLinks;
    }
    return (ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber) &&
           (ia.nFileIndexHigh == ib.nFileIndexHigh) &&
           (ia.nFileIndexLow == ib.nFileIndexLow) &&
           (ib.nNumberOfLinks >= 2);
#else
    struct stat sa, sb;
    if (stat(path_a, &sa) != 0 || stat(path_b, &sb) != 0) {
        return 0;
    }
    if (out_links != NULL) {
        *out_links = (unsigned long)sb.st_nlink;
    }
    return (sa.st_dev == sb.st_dev) && (sa.st_ino == sb.st_ino) && (sb.st_nlink >= 2);
#endif
}

static void test_abi_magic(void) {
    ASSERT_TRUE(b8g_wasi_host_abi_magic() == 0x33564742u, "abi_magic == 0x33564742u");
    ASSERT_TRUE(b8g_wasi_host_abi_magic() == B8G_WASI_HOST_ABI_MAGIC_V3, "abi_magic == macro");
    ASSERT_TRUE(strcmp(b8g_wasi_host_os(), "unknown") != 0, "os string resolved");
    ASSERT_TRUE(strcmp(b8g_wasi_host_arch(), "unknown") != 0, "arch string resolved");
}

static void test_extract_app_name(void) {
    char buf[128];

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("D:\\pnpm\\bin\\godot.exe", buf, sizeof(buf)) == 0,
                "extract Win32 path");
    ASSERT_STR_EQ(buf, "godot", "godot.exe -> godot");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("C:\\Tools\\FFMPEG.EXE", buf, sizeof(buf)) == 0,
                "extract uppercase .EXE");
    ASSERT_STR_EQ(buf, "FFMPEG", "FFMPEG.EXE -> FFMPEG");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("/usr/local/bin/ripgrep", buf, sizeof(buf)) == 0,
                "extract POSIX path");
    ASSERT_STR_EQ(buf, "ripgrep", "POSIX ripgrep -> ripgrep");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("wasi-host", buf, sizeof(buf)) == 0,
                "extract bare wasi-host");
    ASSERT_STR_EQ(buf, "", "wasi-host -> empty");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("E:\\bin\\wasi-host.exe", buf, sizeof(buf)) == 0,
                "extract wasi-host.exe");
    ASSERT_STR_EQ(buf, "", "wasi-host.exe -> empty");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("/opt/b8g/wasi-host-linux-x64", buf, sizeof(buf)) == 0,
                "extract wasi-host-linux-x64");
    ASSERT_STR_EQ(buf, "", "wasi-host-linux-x64 -> empty");

    ASSERT_TRUE(b8g_wasi_host_extract_app_name("D:\\bin\\godot.exe", buf, 3) == B8G_WASI_HOST_ERR_OVERFLOW,
                "extract overflow check");
}

static void test_resolve_cas_root(void) {
    char buf[512];

    set_env_var("B8G_GHOST_CACHE_DIR", "D:/ghost-cache");
    ASSERT_TRUE(b8g_wasi_host_resolve_cas_root(buf, sizeof(buf)) == 0, "resolve with B8G_GHOST_CACHE_DIR");
    ASSERT_STR_EQ(buf, "D:/ghost-cache/v11", "B8G_GHOST_CACHE_DIR -> <dir>/v11");

    set_env_var("B8G_GHOST_CACHE_DIR", "D:/ghost-cache/");
    ASSERT_TRUE(b8g_wasi_host_resolve_cas_root(buf, sizeof(buf)) == 0, "resolve with trailing slash");
    ASSERT_STR_EQ(buf, "D:/ghost-cache/v11", "trailing slash trimmed");

    set_env_var("B8G_GHOST_CACHE_DIR", NULL);
    ASSERT_TRUE(b8g_wasi_host_resolve_cas_root(buf, sizeof(buf)) == 0, "resolve HOME fallback");
    ASSERT_TRUE(strstr(buf, "/.b8g/cas/v11") != NULL, "HOME fallback ends with /.b8g/cas/v11");

    ASSERT_TRUE(b8g_wasi_host_resolve_cas_root(buf, 4) == B8G_WASI_HOST_ERR_OVERFLOW,
                "cas_root overflow check");
}

static void test_v11_blob_path(void) {
    /* 128-char SHA-512 hex string (mixed case to verify lowercase normalization). */
    const char* sha512 =
        "ABcd0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char* expected_reg =
        "D:/cas/v11/files/ab/"
        "abcd0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char* expected_exec =
        "D:/cas/v11/files/ab/"
        "abcd0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef-exec";

    char out[512];
    ASSERT_TRUE(b8g_wasi_host_v11_blob_path("D:/cas/v11", sha512, 0, out, sizeof(out)) == 0,
                "v11 regular blob path");
    ASSERT_STR_EQ(out, expected_reg, "v11 regular path exact match");

    ASSERT_TRUE(b8g_wasi_host_v11_blob_path("D:/cas/v11/", sha512, 1, out, sizeof(out)) == 0,
                "v11 exec blob path");
    ASSERT_STR_EQ(out, expected_exec, "v11 exec path exact match");

    ASSERT_TRUE(b8g_wasi_host_v11_blob_path("D:/cas/v11", "deadbeef", 0, out, sizeof(out)) == B8G_WASI_HOST_ERR_INVALID,
                "reject short sha512");

    char bad_hex[129];
    memset(bad_hex, 'g', 128);
    bad_hex[128] = '\0';
    ASSERT_TRUE(b8g_wasi_host_v11_blob_path("D:/cas/v11", bad_hex, 0, out, sizeof(out)) == B8G_WASI_HOST_ERR_INVALID,
                "reject non-hex sha512");
}

static void test_validate_wasm_header(void) {
    const uint8_t valid[8]       = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    const uint8_t valid_long[12] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60};
    const uint8_t bad_magic[8]   = {0x7F, 0x45, 0x4C, 0x46, 0x01, 0x00, 0x00, 0x00};
    const uint8_t bad_ver[8]     = {0x00, 0x61, 0x73, 0x6D, 0x02, 0x00, 0x00, 0x00};

    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(valid, sizeof(valid)) == 1, "valid 8-byte wasm");
    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(valid_long, sizeof(valid_long)) == 1, "valid 12-byte wasm");
    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(bad_magic, sizeof(bad_magic)) == 0, "reject ELF magic");
    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(bad_ver, sizeof(bad_ver)) == 0, "reject wasm v2");
    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(valid, 7) == 0, "reject truncated header");
    ASSERT_TRUE(b8g_wasi_host_validate_wasm_header(NULL, 8) == 0, "reject NULL");
}

static void test_hardlink_and_live_cli(const char* host_exe_path) {
    /* Create a temporary test directory alongside the host executable. */
    char tmp_dir[512];
#if defined(_WIN32)
    const char* base_tmp = getenv("TEMP");
    if (base_tmp == NULL || base_tmp[0] == '\0') {
        base_tmp = ".";
    }
    snprintf(tmp_dir, sizeof(tmp_dir), "%s\\b8g_wasi_host_test_%lu", base_tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/b8g_wasi_host_test_%lu", (unsigned long)getpid());
#endif
    MKDIR_P(tmp_dir);

    /* 1. Direct libwasi_host hardlink test + file-index / inode verification. */
    char src_file[640];
    char dst_link[640];
    snprintf(src_file, sizeof(src_file), "%s/origin.bin", tmp_dir);
    snprintf(dst_link, sizeof(dst_link), "%s/hardlink.bin", tmp_dir);

    FILE* fp = fopen(src_file, "wb");
    ASSERT_TRUE(fp != NULL, "create origin.bin");
    if (fp != NULL) {
        const char payload[] = "B8GV3-HARDLINK-PROOF";
        fwrite(payload, 1, sizeof(payload) - 1, fp);
        fclose(fp);
    }

    ASSERT_TRUE(b8g_wasi_host_stamp_hardlink(src_file, dst_link) == B8G_WASI_HOST_OK,
                "stamp_hardlink origin -> hardlink");

    unsigned long link_count = 0;
    ASSERT_TRUE(verify_same_hardlink_inode(src_file, dst_link, &link_count),
                "hardlink shares exact same NTFS file-index / POSIX inode with nLinks >= 2");
    ASSERT_TRUE(link_count >= 2, "hardlink link_count >= 2");

    remove(dst_link);
    remove(src_file);

    if (host_exe_path == NULL || host_exe_path[0] == '\0') {
        return;
    }

    /* 2. Live execution: wasi-host --wasi-host-info */
    char cache_dir[640];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", tmp_dir);
    MKDIR_P(cache_dir);
    set_env_var("B8G_GHOST_CACHE_DIR", cache_dir);

    char cmd[1024];
    char out[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" --wasi-host-info", host_exe_path);
    int rc = run_cmd_capture(cmd, out, sizeof(out));
    ASSERT_TRUE(rc == 0, "live wasi-host --wasi-host-info exits 0");
    ASSERT_TRUE(strstr(out, "\"abiMagic\":\"0x33564742\"") != NULL, "info JSON has abiMagic 0x33564742");
    ASSERT_TRUE(strstr(out, "\"appName\":\"\"") != NULL, "info JSON has empty appName for direct wasi-host");
    ASSERT_TRUE(strstr(out, "\"casV11Root\":") != NULL, "info JSON has casV11Root");
    ASSERT_TRUE(strstr(out, "\"os\":") != NULL, "info JSON has os");
    ASSERT_TRUE(strstr(out, "\"arch\":") != NULL, "info JSON has arch");

    /* 3. Live execution: wasi-host --stamp demo-app <tmp_dir> */
    snprintf(cmd, sizeof(cmd), "\"%s\" --stamp demo-app \"%s\"", host_exe_path, tmp_dir);
    rc = run_cmd_capture(cmd, out, sizeof(out));
    ASSERT_TRUE(rc == 0, "live wasi-host --stamp demo-app exits 0");
    ASSERT_TRUE(strstr(out, "\"extraBytes\":0") != NULL, "stamp JSON reports extraBytes:0");

    char stamped_exe[640];
#if defined(_WIN32)
    snprintf(stamped_exe, sizeof(stamped_exe), "%s\\demo-app.exe", tmp_dir);
#else
    snprintf(stamped_exe, sizeof(stamped_exe), "%s/demo-app", tmp_dir);
#endif
    unsigned long stamped_links = 0;
    ASSERT_TRUE(verify_same_hardlink_inode(host_exe_path, stamped_exe, &stamped_links),
                "stamped demo-app.exe is a 0-byte hardlink to wasi-host.exe");

    /* 4. Live execution of stamped demo-app.exe --wasi-host-info */
    snprintf(cmd, sizeof(cmd), "\"%s\" --wasi-host-info", stamped_exe);
    rc = run_cmd_capture(cmd, out, sizeof(out));
    ASSERT_TRUE(rc == 0, "live demo-app --wasi-host-info exits 0");
    ASSERT_TRUE(strstr(out, "\"appName\":\"demo-app\"") != NULL, "stamped binary detects appName=demo-app");

    /* 5. Live 3-tier CAS lookup + .wasm header validation */
    char v11_dir[700];
    char apps_dir[760];
    char files_dir[760];
    char shard_dir[800];
    snprintf(v11_dir, sizeof(v11_dir), "%s/v11", cache_dir);
    snprintf(apps_dir, sizeof(apps_dir), "%s/apps", v11_dir);
    snprintf(files_dir, sizeof(files_dir), "%s/files", v11_dir);
    snprintf(shard_dir, sizeof(shard_dir), "%s/de", files_dir);
    MKDIR_P(v11_dir);
    MKDIR_P(apps_dir);
    MKDIR_P(files_dir);
    MKDIR_P(shard_dir);

    const char* demo_sha =
        "deadbeef00112233445566778899aabbccddeeff00112233445566778899aabb"
        "ccddeeff00112233445566778899aabbccddeeff00112233445566778899aabb";
    char blob_file[960];
    char idx_file[840];
    ASSERT_TRUE(b8g_wasi_host_v11_blob_path(v11_dir, demo_sha, 0, blob_file, sizeof(blob_file)) == 0,
                "format demo-app v11 blob path");
    snprintf(idx_file, sizeof(idx_file), "%s/demo-app.sha512", apps_dir);

    FILE* bf = fopen(blob_file, "wb");
    ASSERT_TRUE(bf != NULL, "create valid wasm blob");
    if (bf != NULL) {
        const uint8_t valid_wasm[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        fwrite(valid_wasm, 1, sizeof(valid_wasm), bf);
        fclose(bf);
    }
    FILE* idxf = fopen(idx_file, "wb");
    ASSERT_TRUE(idxf != NULL, "create demo-app.sha512 index");
    if (idxf != NULL) {
        fputs(demo_sha, idxf);
        fclose(idxf);
    }

    snprintf(cmd, sizeof(cmd), "\"%s\"", stamped_exe);
    rc = run_cmd_capture(cmd, out, sizeof(out));
    ASSERT_TRUE(rc == 0, "live demo-app 3-tier CAS lookup + wasm validation exits 0");
    ASSERT_TRUE(strstr(out, "\"wasmValid\":true") != NULL, "demo-app reports wasmValid:true");

    /* Corrupt the blob and confirm Tier-3 rejects it. */
    bf = fopen(blob_file, "wb");
    if (bf != NULL) {
        const uint8_t corrupt[8] = {0x7F, 0x45, 0x4C, 0x46, 0x00, 0x00, 0x00, 0x00};
        fwrite(corrupt, 1, sizeof(corrupt), bf);
        fclose(bf);
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" 2>&1", stamped_exe);
    rc = run_cmd_capture(cmd, out, sizeof(out));
    ASSERT_TRUE(rc != 0, "corrupt wasm blob rejected with non-zero exit");

    /* Cleanup temp artifacts. */
    remove(blob_file);
    remove(idx_file);
    remove(stamped_exe);
#if defined(_WIN32)
    _rmdir(shard_dir);
    _rmdir(files_dir);
    _rmdir(apps_dir);
    _rmdir(v11_dir);
    _rmdir(cache_dir);
    _rmdir(tmp_dir);
#else
    rmdir(shard_dir);
    rmdir(files_dir);
    rmdir(apps_dir);
    rmdir(v11_dir);
    rmdir(cache_dir);
    rmdir(tmp_dir);
#endif
}

int main(int argc, char** argv) {
    const char* host_exe = (argc >= 2) ? argv[1] : NULL;

    test_abi_magic();
    test_extract_app_name();
    test_resolve_cas_root();
    test_v11_blob_path();
    test_validate_wasm_header();
    test_hardlink_and_live_cli(host_exe);

    printf("wasi_host_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
