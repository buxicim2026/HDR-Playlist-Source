# Building

The plugin is plain C11 + CMake and does **not** compile OBS. It needs only
the libobs *headers* plus a linker input, matching how the OBS SDK is used by
many third-party plugins.

## Prerequisites

| Platform | Tools |
| --- | --- |
| Windows x64 | Visual Studio Build Tools (C++ workload: `cl`/`lib`/`dumpbin`), CMake ≥ 3.16, Git |
| Linux x64 | gcc/clang, CMake ≥ 3.16, Git |
| macOS | Xcode Command Line Tools (`xcode-select --install`), CMake, Git |

## Windows

Run inside a *Developer PowerShell for VS* (so `dumpbin`/`lib`/`cl` are on
PATH):

```powershell
.\scripts\package-plugin.ps1 -ObsVersion 31.0.0
# Output: release\HDR-Playlist-Source-obs-win-x64-<version>.zip
```

What it does:

1. shallow-clones `obsproject/obs-studio` at `-ObsVersion` for headers
2. finds `obs.dll` (installed OBS or auto-downloaded official zip)
3. generates an import library `obs.lib` via `dumpbin /exports` + `lib`
4. runs CMake + MSVC build
5. assembles `bin/64bit/HDR-Playlist-Source.dll` + `data/locale`, zips it

## Linux

```bash
bash scripts/package-plugin.sh
# Output: release/HDR-Playlist-Source-obs-linux-x64-<version>.tar.gz
```

The script creates a stub `libobs.so` with `soname=libobs.so.0` for linking;
runtime symbols resolve against installed OBS.

## macOS

```bash
bash scripts/package-plugin.sh
# Output: release/HDR-Playlist-Source-obs-macos-arm64-<version>.tar.gz
```

The module links with `-undefined dynamic_lookup` and is wrapped into a
`HDR-Playlist-Source.plugin` bundle.

## Manual CMake (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIBOBS_INCLUDE_DIR=/path/to/obs-studio/libobs
# plus one of:
#   -DOBS_IMPORT_LIB=<obs.lib>   (Windows)
#   -DOBS_STUB_LIB=<libobs.so>   (Linux)
cmake --build build
```
