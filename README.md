# t3-conv

`t3-conv` is a Windows command-line tool for converting Tianzheng DWG files to T3 DWG files through a no-UI `TBatSave` workflow.

## Portable Package

Download the ready-to-run Windows x64 package from [`release/t3-conv.zip`](./release/t3-conv.zip).

The package is portable, but it is not a standalone Tianzheng or AutoCAD replacement. The target machine still needs Tianzheng TArch T20 V9 and a supported AutoCAD installation.

Compared with dialog-driven CAD operation, this tool avoids repeated menu clicks, confirmation dialogs, per-file manual waiting, and unnecessary host restarts. In local smoke tests, direct-worker single-file conversion typically completed in seconds to tens of seconds after the host was ready; batch runs are usually several times faster than manual popup workflows because files can run unattended and reuse the Tianzheng CAD host. Actual speed depends on DWG size, fonts, external references, machine performance, and whether the host is already warm.

The current production path is intentionally narrow:

```text
t3conv.exe
  -> load t3conv.ini
  -> find Tianzheng T20 V9 and AutoCAD
  -> start or reuse a background Tianzheng CAD host
  -> wait until Tianzheng's ARX modules, fonts, and no-dialog settings are ready
  -> convert one DWG through Tianzheng's T3 save logic
  -> write the T3 DWG and a compact conversion log
  -> return diagnostics instead of opening modal UI when conversion fails
```

In practice, `t3conv.exe` starts CAD only to create a real Tianzheng runtime environment. Once the host is ready, conversion is driven by the verified TBatSave direct worker, not by clicking menus or sending modal UI commands.

The tool does not use `accoreconsole`, legacy Python prototypes, ARX test scaffolds, job/result file protocols, or UI command fallbacks.

## Features

- Converts a single DWG to a T3 DWG with a default `_t3.dwg` suffix.
- Converts a folder by running isolated per-file `t3conv.exe` child processes.
- Reuses a ready Tianzheng CAD host when possible.
- Detects and recovers hung Tianzheng `acad.exe` processes before reuse.
- Keeps runtime state under `var` instead of copying project scripts into the Tianzheng installation.
- Syncs project `.shx` / `.ttf` fonts into AutoCAD `Fonts` safely without overwriting conflicting files.
- Produces compact logs, in-log batch summaries, and optional debug diagnostics.

## Requirements

- Windows x64.
- Tianzheng TArch T20 V9 installed locally.
- AutoCAD 2020-2026 installed locally, with an available `Fonts` directory.
- CMake 3.20+ and a C++17-capable MSVC toolchain when building from source.

The portable package and this repository do not include Tianzheng or AutoCAD files.

## Configuration

The only configuration file is [`t3conv.ini`](./t3conv.ini). It can stay fully commented when auto-detection works:

```ini
[paths]
; tangent_root=C:\path\to\TArchT20V9
; autocad_root=C:\Program Files\Autodesk\AutoCAD 2020

[fonts]
; fontalt=HZTXT.SHX
```

Resolution order:

- `paths.tangent_root`: INI value, then `T3CONV_TANGENT_ROOT`, then auto-detection.
- `paths.autocad_root`: INI value, then `T3CONV_AUTOCAD_ROOT`, then auto-detection from AutoCAD 2020-2026.
- `fonts.fontalt`: INI value, then `T3CONV_FONTALT`, then `HZTXT.SHX`.
- Project fonts are always read from [`fonts`](./fonts).

If Tianzheng T20 V9, `TGStart.exe`, AutoCAD, or AutoCAD `Fonts` cannot be found, `t3conv.exe` exits before starting a conversion.

## Usage

Use absolute paths for source, target, output, and log arguments.

Convert one DWG with the minimum arguments:

```powershell
.\t3conv.exe -s "C:\path\source.dwg"
```

The source can also be passed as the first positional source path:

```powershell
.\t3conv.exe "C:\path\source.dwg"
```

Default output:

```text
C:\path\source_t3.dwg
```

Convert one DWG with an explicit target:

```powershell
.\t3conv.exe -s "C:\path\source.dwg" -o "C:\path\source_t3.dwg"
```

Convert a folder:

```powershell
.\t3conv.exe -s "C:\path\dwgs" -o "C:\path\t3-output"
```

When `-s` points to a directory, `t3conv.exe` automatically runs as a batch parent. There is no `--batch` flag.

Runtime output is compact by default. Use `--dry-run` to print the launch plan, or `-d` / `--debug` to print internal diagnostics.

Single-file and folder conversions write `t3conv.log` next to `t3conv.exe` / the portable workspace unless `--log <path>` is provided:

- `t3conv.log`: compact per-file log. Non-debug entries include the copyable command, source, output, final status, `started_at` local timestamp, duration, output size, and error summary when failed.
- `log\t3conv.log.1`, `log\t3conv.log.2`, ...: rolled log files. When `t3conv.log` exceeds 20 MB, new entries go into the `log` folder; after 20 rolled files are full, the old rolled logs are cleared and numbering restarts.
- `batch_summary`: folder conversion appends total, success, failure, total seconds, and average seconds at the end of `t3conv.log`.

Add `-d` / `--debug` to include the launch plan, child stdout/stderr, and internal diagnostics.

Batch conversion reuses the same Tianzheng CAD host between files. It restarts the host only for timeouts, launch failures, crashes, or explicit host-side failures; normal per-file conversion errors are logged and the next DWG continues.

Single-file conversion uses a temporary `_t3conv_work` staging directory under the workspace so the direct worker never writes directly over the requested target. The directory is removed automatically after each conversion attempt.

Host control is optional and normally unnecessary. Single-file and folder conversions automatically reuse or launch the Tianzheng CAD host when needed; use the host options only for troubleshooting or manual pre-warm/stop workflows.

## CLI Options

| Option | Description |
| --- | --- |
| `-s <path>`, `--source <path>` | Source DWG or source directory. Must be absolute. It can also be supplied as the first positional source path. |
| `-o <path>`, `--target <path>` | Target DWG for single-file conversion, or output directory when the source is a directory. Must be absolute. |
| `--output-dir <dir>` | Output directory for derived files. Must be absolute. |
| `--log <path>` | Explicit log path. Must be absolute. |
| `-d`, `--debug` | Print the launch plan and expand stdout/stderr/internal diagnostics. |
| `--timeout-seconds <n>` | Host launch, single-file conversion, and direct worker timeout. Default: `120` seconds. |
| `--retries <n>` | Batch retry count. Default: `1`. |
| `--no-overwrite` | Preserve an existing target file. |
| `--dry-run` | Print the launch plan without starting CAD or modifying `tangent.mnl`. |
| `--json` | Print a JSON-style launch plan. |
| `--tbatsave-bindmode <n>` | Reserved reverse-engineering / diagnostic option. The current direct worker path does not treat it as a stable conversion parameter. |
| `--tbatsave-bindref <n>` | Reserved reverse-engineering / diagnostic option. The current direct worker path does not treat it as a stable conversion parameter. |
| `--host-status` | Check whether the background host is ready. |
| `--host-stop` | Request the background host loop to stop. |
| `--host-start` | Optional pre-warm command that launches or reuses a background Tianzheng CAD host loop. |

## Build

```powershell
cmake -S . -B ..\_build\t3-conv-release
cmake --build ..\_build\t3-conv-release --config Release
```

The executable is generated at:

```text
..\_build\t3-conv-release\src\t3conv\Release\t3conv.exe
```

## Package

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\package.ps1
```

By default, the script builds the Release binary and creates:

```text
<project-parent>\dist\t3-conv.zip
```

The zip contains only runtime files:

- `t3conv.exe`
- `t3conv.ini`
- `runtime\tgstart_host\*.lsp`
- `fonts\`
- `docs\README-packaged.md`

It intentionally excludes `src`, `tests`, `var`, build intermediates, Tianzheng files, and AutoCAD files.

## Project Layout

| Path | Purpose |
| --- | --- |
| [`src/common`](./src/common) | Shared types, error codes, and path helpers. |
| [`src/t3conv`](./src/t3conv) | The production CLI implementation. |
| [`runtime/tgstart_host`](./runtime/tgstart_host) | LSP templates rendered into `var/runtime` at startup. |
| [`fonts`](./fonts) | Optional project fonts synced into AutoCAD `Fonts`. |
| [`docs`](./docs) | Developer docs and the native no-UI protocol notes. |
| [`tools/package.ps1`](./tools/package.ps1) | Windows packaging script. |
| `var/host` | Generated host markers and direct worker status files. |
| `var/runtime` | Generated local runtime LSP files and `fontmap.fmp`. |
| `var/logs/trigger.log` | LSP-side diagnostic log. |
| `var/bootstrap.json` | Generated startup state snapshot. |

Generated `var` files can be deleted; they are recreated on demand.

## Documentation

- [t3conv developer guide](./docs/t3conv-developer-guide.md) explains this project's configuration, CLI, runtime layout, host reuse, startup repair, conversion, batch, logs, troubleshooting, and packaging behavior.
- [TBatSave direct worker protocol](./docs/tbatsave-direct-worker-protocol.md) describes the reverse-engineered direct worker contract for developers implementing the same protocol in another language.

## Current Boundaries

- The verified production target is single-file T3 output using the TBatSave direct worker.
- Folder conversion is an outer batch runner that invokes the single-file path one DWG at a time.
- The direct worker failure path returns an error with diagnostics; it does not fall back to modal `TBatSave`, `TBatSaveFolder`, or `PLDC` commands.
- Source, target, output, and log paths must be absolute.
- AutoCAD education-version warnings are only suppressed as far as host variables allow; this tool does not guarantee removal of education-version marks.
- Xrefs, proxy objects, missing fonts, damaged DWGs, and confirmation dialogs are handled with host no-UI variables, font mapping, direct worker checks, and per-file diagnostics, not by interactive UI automation.

## Disclaimer

This repository is intended solely for technical research, interoperability analysis, automation testing, protocol research, reverse engineering, and compatibility learning purposes. It must not be used for commercial cracking, license bypassing, unauthorized distribution, commercial misuse, or any illegal activities.

This project does not include, provide, distribute, bundle, sell, or modify any third-party commercial software, licenses, or proprietary components. Users are responsible for legally obtaining and installing all required software and valid licenses independently.

All related software, names, trademarks, copyrights, and other intellectual property referenced in this repository belong to their respective owners, including but not limited to Tianzheng, AutoCAD, Autodesk, and their affiliated rights holders.

The purpose of this project is limited to researching software interoperability, file format compatibility, automation workflows, and related technical implementations. It is not intended to crack, bypass, replace, or circumvent any commercial software or licensing mechanisms.

If any copyright holder or rights owner believes that certain content in this repository is inappropriate or infringing, please contact the repository maintainer via Issue or email. Relevant content will be reviewed and promptly modified or removed upon verification.
