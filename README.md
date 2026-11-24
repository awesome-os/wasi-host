# `sections/wasi-host` — Universal WASI Host Launcher & `libwasi-host`

> **ABI Magic:** `B8G_WASI_HOST_ABI_MAGIC_V3` (`0x33564742`, `"B8GV3"`)  
> **Toolchain:** Native C11 (`-std=c11 -Os -s -Wall -Wextra -Werror`) via `holo-compiler` / `.\bin\zig.cmd cc` (zero WSL, zero external dependencies).

---

## Architecture

Collapses the `N apps x 5 OS` native launcher matrix down to:

- **4 tiny native hosts** (`wasi-host-<os>-<arch>` + `libwasi-host`)
- **`N x 1` portable `.wasm` modules** stored in the v11 content-addressed store (`<cas_v11_root>/files/<first2>/<sha512>[-exec]`)
- **0-byte CLI stamping** via NTFS `CreateHardLinkA` / POSIX `link()` so stamping `<app>[.exe]` consumes **0 extra bytes on disk** while `argv[0]` automatically identifies the target app.

## Files

| File | Purpose |
| :--- | :--- |
| [`include/libwasi_host.h`](include/libwasi_host.h) | Public C11 ABI header (`B8G_WASI_HOST_ABI_MAGIC_V3 = 0x33564742u`) |
| [`src/libwasi_host.c`](src/libwasi_host.c) | Core CAS root resolution, `argv[0]` app-name extraction, v11 blob pathing, `.wasm` header validation, and 0-byte hardlink stamping |
| [`src/wasi_host.c`](src/wasi_host.c) | Thin universal launcher CLI (`--wasi-host-info`, `--stamp <app> <dest-dir>`, and 3-tier CAS lookup + validation) |
| [`src/wasi_host_test.c`](src/wasi_host_test.c) | Unit & live integration test suite (including NTFS file-index / POSIX inode hardlink verification) |

## 3-Tier Lookup Plan

1. **Tier 1 (`v11` CAS lookup):** Resolves `<cas_v11_root>` (`$B8G_GHOST_CACHE_DIR/v11` or `<HOME>/.b8g/cas/v11`) and maps `<cas_v11_root>/apps/<app>.sha512` to `<cas_v11_root>/files/<first2>/<sha512>[-exec]` (falling back to `<cas_v11_root>/apps/<app>.wasm`).
2. **Tier 2 (install-if-missing hook):** On CAS miss, checks `B8G_WASI_INSTALL_HOOK` or emits a structured install-if-missing diagnostic.
3. **Tier 3 (`.wasm` validation & dispatch):** Verifies the 8-byte WebAssembly header (`\0asm\1\0\0\0`) before B8G ABI v3 dispatch.

## Measured Stripped Binary Sizes (`-std=c11 -Os -s`)

| Artifact | Target | Exact Size |
| :--- | :--- | ---: |
| `libwasi-host-linux-x64.so` | `x86_64-linux-musl` (shared) | `6,520 B` (`6.4 KB`) |
| `libwasi-host-windows-x64.dll` | `x86_64-windows-gnu` (DLL) | `18,432 B` (`18.0 KB`) |
| `wasi-host.exe` | `x86_64-windows-gnu` (PE64) | `23,552 B` (`23.0 KB`) |
| `wasi-host-linux-x64` | `x86_64-linux-musl` (ELF64) | `34,416 B` (`33.6 KB`) |

**Verification:** `wasi_host_test.exe` passes **57 / 57** assertions (`0` failures), including real NTFS 0-byte hardlink stamping (`nNumberOfLinks >= 2`, identical volume serial & file index), live `wasi-host.exe --wasi-host-info`, live `--stamp`, and 3-tier CAS `.wasm` header validation.

## Build & Test (Windows 11 PowerShell, Zero WSL)

```powershell
$tmp = Join-Path $env:TEMP "wasi-host-build"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

# Native Win64 launcher + test runner
.\bin\zig.cmd cc -std=c11 -Os -s -Wall -Wextra -Werror -Isections/wasi-host/include `
  sections/wasi-host/src/libwasi_host.c sections/wasi-host/src/wasi_host.c `
  -o "$tmp\wasi-host.exe"

.\bin\zig.cmd cc -std=c11 -Os -s -Wall -Wextra -Werror -Isections/wasi-host/include `
  sections/wasi-host/src/libwasi_host.c sections/wasi-host/src/wasi_host_test.c `
  -o "$tmp\wasi_host_test.exe"

& "$tmp\wasi_host_test.exe" "$tmp\wasi-host.exe"

# Cross-compile stripped Linux x64 musl shared library & launcher
.\bin\zig.cmd cc -target x86_64-linux-musl -std=c11 -Os -s -fPIC -shared -Wall -Wextra -Werror `
  -Isections/wasi-host/include sections/wasi-host/src/libwasi_host.c `
  -o "$tmp\libwasi-host-linux-x64.so"

.\bin\zig.cmd cc -target x86_64-linux-musl -std=c11 -Os -s -Wall -Wextra -Werror `
  -Isections/wasi-host/include sections/wasi-host/src/libwasi_host.c sections/wasi-host/src/wasi_host.c `
  -o "$tmp\wasi-host-linux-x64"
```
