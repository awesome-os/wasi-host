> [!NOTE]
> Consolidated into the unified Holo v11 monorepo at https://github.com/awesome-os/awesome-os (all existing v11 CAS release assets remain downloadable).

# `holo-wasi-host` (`awesome-os/wasi-host`) — Universal WASI Host Launcher, `libwasi-host` & `holo-wasi-host-b8g.wasm`

> **ABI Magic:** `B8G_WASI_HOST_ABI_MAGIC_V3` (`0x33564742`, `"B8GV3"`)  
> **CAS Layout:** `v11` (`holo-compiler-lockfile.json`)  
> **Toolchain:** Native C11 (`-std=c11 -Os -s -Wall -Wextra -Werror`) via `holo-compiler` / `.\bin\zig.cmd cc` (zero WSL, zero external dependencies).

---

## Architecture

Collapses the `N apps x 5 OS` native launcher matrix down to:

- **4 tiny native hosts** (`holo-wasi-host-<os>-<arch>` + backwards-compatible `wasi-host-<os>-<arch>` aliases + `libwasi-host`)
- **1 universal portable `wasm32-wasi` core** (`holo-wasi-host-b8g.wasm`, `2,662 B`) exporting `holo_wasi_b8g_abi_magic`, `holo_wasi_b8g_version`, and `holo_wasi_b8g_route_app`
- **`N x 1` portable `.wasm` modules** stored in the v11 content-addressed store (`<cas_v11_root>/files/<first2>/<sha512>[-exec]`)
- **0-byte CLI stamping** via NTFS `CreateHardLinkA` / POSIX `link()` so stamping `<app>[.exe]` consumes **0 extra bytes on disk** while `argv[0]` automatically identifies the target app.

## Files

| File | Purpose |
| :--- | :--- |
| [`include/libwasi_host.h`](include/libwasi_host.h) | Public C11 ABI header (`B8G_WASI_HOST_ABI_MAGIC_V3 = 0x33564742u`) |
| [`src/libwasi_host.c`](src/libwasi_host.c) | Core CAS root resolution, `argv[0]` app-name extraction (`holo-wasi-host*` / `wasi-host*`), v11 blob pathing, `.wasm` header validation, and 0-byte hardlink stamping |
| [`src/wasi_host.c`](src/wasi_host.c) | Thin universal launcher CLI (`--wasi-host-info`, `--stamp <app> <dest-dir>`, and 3-tier CAS lookup + validation) |
| [`src/holo_wasi_host_b8g.c`](src/holo_wasi_host_b8g.c) | Universal portable `wasm32-wasi` C11 core (`holo-wasi-host-b8g.wasm`) exporting `holo_wasi_b8g_abi_magic`, `holo_wasi_b8g_version`, `holo_wasi_b8g_route_app`, and JSON WASI command entrypoint |
| [`src/wasi_host_test.c`](src/wasi_host_test.c) | Unit & live integration test suite (including NTFS file-index / POSIX inode hardlink verification) |
| [`holo-compiler-lockfile.json`](holo-compiler-lockfile.json) | Pinned `holo-compiler-lockfile-v1` manifest (`0x33564742`, `v11` CAS SHA-512 blobs) |

## 3-Tier Lookup Plan

1. **Tier 1 (`v11` CAS lookup):** Resolves `<cas_v11_root>` (`$B8G_GHOST_CACHE_DIR/v11` or `<HOME>/.b8g/cas/v11`) and maps `<cas_v11_root>/apps/<app>.sha512` to `<cas_v11_root>/files/<first2>/<sha512>[-exec]` (falling back to `<cas_v11_root>/apps/<app>.wasm`).
2. **Tier 2 (install-if-missing hook):** On CAS miss, checks `B8G_WASI_INSTALL_HOOK` or emits a structured install-if-missing diagnostic.
3. **Tier 3 (`.wasm` validation & dispatch):** Verifies the 8-byte WebAssembly header (`\0asm\1\0\0\0`) before B8G ABI v3 dispatch.

## Release Artifacts & Measured Stripped Sizes (`-std=c11 -Os -s`)

| Artifact | Target | Measured Size |
| :--- | :--- | ---: |
| `holo-wasi-host-b8g.wasm` | `wasm32-wasi` (portable core) | `2,662 B` (`2.6 KB`) |
| `libwasi-host-linux-x64.so` | `x86_64-linux-musl` (shared) | `6,520 B` (`6.4 KB`) |
| `libwasi-host-windows-x64.dll` | `x86_64-windows-gnu` (DLL) | `18,432 B` (`18.0 KB`) |
| `holo-wasi-host-windows-x64.exe` (`wasi-host-windows-x64.exe`) | `x86_64-windows-gnu` (PE64) | `23,552 B` (`23.0 KB`) |
| `holo-wasi-host-linux-x64` (`wasi-host-linux-x64`) | `x86_64-linux-musl` (ELF64) | `34,416 B` (`33.6 KB`) |
| `holo-wasi-host-linux-arm64` (`wasi-host-linux-arm64`) | `aarch64-linux-musl` (ELF64) | `41,048 B` (`40.1 KB`) |
| `holo-wasi-host-darwin-arm64` (`wasi-host-darwin-arm64`) | `aarch64-macos` (Mach-O) | `51,784 B` (`50.6 KB`) |

All releases also ship `libwasi-host-{linux-arm64.so,darwin-arm64.dylib}`, `libwasi_host.h`, `SHA256SUMS.txt`, and `SHA512SUMS.txt`.

## Build & Test (Windows 11 PowerShell, Zero WSL)

```powershell
$tmp = Join-Path $env:TEMP "wasi-host-build"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

# Universal portable wasm32-wasi core
.\bin\zig.cmd cc -target wasm32-wasi -Os -s -std=c11 -Wall -Wextra -Werror `
  src/holo_wasi_host_b8g.c -o "$tmp\holo-wasi-host-b8g.wasm"

# Native Win64 launcher + test runner
.\bin\zig.cmd cc -std=c11 -Os -s -Wall -Wextra -Werror -Iinclude `
  src/libwasi_host.c src/wasi_host.c `
  -o "$tmp\holo-wasi-host-windows-x64.exe"

.\bin\zig.cmd cc -std=c11 -Os -s -Wall -Wextra -Werror -Iinclude `
  src/libwasi_host.c src/wasi_host_test.c `
  -o "$tmp\wasi_host_test.exe"

& "$tmp\wasi_host_test.exe" "$tmp\holo-wasi-host-windows-x64.exe"
```

## Quick Install & Run (Windows / Linux / macOS)

```sh
# Install / hydrate from v11 CAS via holo-pm
holo-pm install repository://github.com/awesome-os/wasi-host@main

# Or project & run directly via holo-project (0-byte hardlink)
holo-project "project://cpp/wasi-host?from=repository://github.com/awesome-os/wasi-host@main"
```
