# t3conv 开发指南

## 目标

本文档是 `t3-conv` 的项目级开发文档，覆盖配置、CLI、运行态文件布局、宿主启动、单文件转换、目录批量、日志、打包、测试和排障。

跨语言 direct worker 协议细节保留在 [TBatSave direct worker 协议](./tbatsave-direct-worker-protocol.md)。本文档只描述当前 C++ 项目如何把这些协议组织成可运行的 `t3conv.exe`。

当前生产链路只有一条：

```text
t3conv.exe
  -> 读取 t3conv.ini
  -> 自动检测或从配置 / 环境变量定位 Tianzheng 和 AutoCAD
  -> 启动或复用后台 Tianzheng CAD 宿主
  -> 等待天正 ARX、字体映射和无弹窗变量准备完成
  -> 调用已验证的 TBatSave direct worker 转换单个 DWG
  -> 成功时生成 <source_stem>_t3.dwg 并写入简洁日志
  -> 失败时返回错误和诊断，不回退到交互式 UI 命令
```

启动 CAD 的目的不是模拟用户操作，而是让 Tianzheng 插件、字体路径、保存变量和 direct worker 所需的运行环境真实加载完成。宿主就绪后，转换逻辑直接在该环境中执行；失败时返回诊断，避免进入会弹窗的命令链路。

这条链路不模拟 UI 操作，也不依赖旧脚本入口、job/result 文件协议或会弹窗的命令回退。

## 为什么不使用 AccoreConsole

项目曾验证过两条 `AccoreConsole.exe` 路线，结论都是不作为当前生产方案使用。

### `AccoreConsole + 完整 Tianzheng T20 V9`

测试方式是在 AutoCAD 2020 的 `AccoreConsole.exe` 中手动加载完整天正目录下的核心 `tch_*.arx`，包括 `tch_kernal.arx`、`tch_initstart.arx`、`tch_UIFrame.arx`、`tch_kerncmd.arx` 和 `tch_toolcmd.arx` 等。结果是单个加载、按依赖顺序加载、通过 `/p TArch20V9 /t acad.dwt` 启动配置加载，均无法得到可用的天正运行态；`ARXLOAD` 返回失败，profile/template 启动后也没有自动加载 `tch_kernal.arx`。

根因不是脚本参数问题，而是完整天正 ARX 深度依赖桌面版 AutoCAD 运行环境。天正的 `tch_*.arx` 不只是 DWG 数据解析模块，还包含命令注册、右键菜单、动态输入、自定义实体显示和交互逻辑。依赖和符号分析显示，相关模块依赖 `ACAD.exe`、`acui23.dll`、`adui23.dll`、MFC、`USER32.dll`、`GDI32.dll`、`AcPal.dll`，并引用编辑器、命令栈、窗口、Palette、状态栏等桌面 UI 相关接口。

`AccoreConsole.exe` 是无头 Core Engine，没有完整桌面消息循环、窗口句柄（`hWnd`）和 UI 宿主。完整天正 ARX 在初始化阶段如果访问这些桌面环境能力，就可能直接加载失败、提前返回错误，或者拒绝进入可用运行态。

另一个限制是启动链不同。完整 Tianzheng 依赖桌面 AutoCAD 的 profile、template、菜单、MNL/LSP 和 ARX 初始化流程组合完成加载；`AccoreConsole.exe` 不能等价复刻桌面版 `acad.exe` 的 `MENULOAD`、菜单和 UI 初始化链。即使改用 `(arxload "...")` 或脚本命令手动加载，也仍会撞到上面的 UI 运行时依赖。

### `AccoreConsole + T20 PlugInV9.0`

测试对象为 `T20 PlugInV9.0`，AutoCAD 2020 对应 `sys23x64`。该插件比完整天正更小，只包含 `tch_kernal.arx`、`tch_utility.arx`、`tch_pipebase.arx`、`tch_pipewire.arx`、`tch_initstart.arx` 以及 COM DLL 等核心组件；但逐个在 `AccoreConsole.exe` 中加载这些 ARX，结果仍然全部 `ARXLOAD 失败`。

对 `T20 PlugInV9.0\sys23x64` 进行依赖检查后可以看到，这些插件 ARX 仍然依赖桌面版 AutoCAD 组件：

```text
ACAD.exe
acui23.dll
adui23.dll
mfc140u.dll
USER32.dll
GDI32.dll
AcPal.dll
```

因此，本项目测试到的 `T20 PlugInV9.0` 更适合作为普通 AutoCAD 桌面环境中的天正显示 / 插件兼容层，而不是可在 `AccoreConsole.exe` 中运行的纯无头 Object Enabler。用户手动通过 `MENULOAD` 加载菜单也属于桌面 CAD 插件路径，不能等价迁移到 Core Console。

### 当前取舍

基于以上验证，当前项目只支持 `TGStart.exe -> acad.exe` 的桌面宿主链路。这个方案不是为了模拟用户点击，而是给 Tianzheng ARX 提供它实际需要的桌面 AutoCAD 运行环境，然后在该环境中调用已验证的 direct worker。后续性能优化优先考虑宿主复用、超时控制、日志压缩和实验性的多宿主池，而不是继续把完整天正或 T20 PlugIn 迁移到 `AccoreConsole.exe`。

## 配置

配置文件固定为：

```text
<workspace_root>\t3conv.ini
```

默认可以不启用任何配置项；程序会自动检测 Tianzheng T20 V9 和 AutoCAD 2021 / 2020 根目录。项目字体目录固定为 `<workspace_root>\fonts`，不需要配置。

```ini
[paths]
; tangent_root=C:\path\to\TArchT20V9
; autocad_root=C:\Program Files\Autodesk\AutoCAD 2020

[fonts]
; fontalt=HZTXT.SHX
```

解析规则：

- `tangent_root` 指向 Tianzheng T20 V9 根目录，优先使用 INI，其次 `T3CONV_TANGENT_ROOT`，最后自动检测。
- Tianzheng 自动检测会优先检查各固定盘 `X:\Tangent\...` 下的常见目录，再检查 workspace 相邻目录或盘符根目录，最后检查 Program Files；候选目录必须同时包含 `TGStart.exe` 和 `SYS`。
- `autocad_root` 指向 AutoCAD 根目录，优先使用 INI，其次 `T3CONV_AUTOCAD_ROOT`，最后按 AutoCAD 2021 到 AutoCAD 2020 的顺序自动检测。
- 当前 direct worker 已明确支持的 AutoCAD / Tianzheng ARX 目录映射为：AutoCAD 2020 -> `sys23x64`，AutoCAD 2021 -> `sys24x64`。程序会校验映射目录下存在 `tch_kernal.arx`。
- `<workspace_root>\fonts` 是固定项目字体目录，不能通过 INI 修改；目录不存在或没有字体文件时自动跳过。
- `fontalt` 用于设置 AutoCAD `FONTALT`，优先使用 INI，其次 `T3CONV_FONTALT`，最后使用 `HZTXT.SHX`。
- 如果未检测到 Tianzheng T20 V9、`TGStart.exe`、AutoCAD 根目录或 AutoCAD `Fonts` 目录，程序会报错退出，不继续启动 CAD。

## CLI

所有输入、输出和日志路径都要求绝对路径。

最少参数转换单个 DWG：

```powershell
.\t3conv.exe -s "C:\absolute\source.dwg"
```

显式指定目标文件：

```powershell
.\t3conv.exe -s "C:\absolute\source.dwg" -o "C:\absolute\source_t3.dwg"
```

默认输出：

```text
<source_dir>\<source_stem>_t3.dwg
```

批量转换目录：

```powershell
.\t3conv.exe -s "C:\absolute\source-folder" -o "C:\absolute\t3-output"
```

宿主控制是可选能力。普通转换会按需自动复用或启动宿主，只有排障、预热或手动停止时才需要直接使用相关 host 参数。

## 参数

| 参数 | 说明 |
| --- | --- |
| `-s <path>`, `--source <path>` | 源 DWG 或源目录，必须是绝对路径；也可作为第一个位置参数传入。 |
| `-o <path>`, `--target <path>` | 单文件转换时表示目标 DWG；源路径是目录时表示批量输出目录。必须是绝对路径。 |
| `-d`, `--debug` | 批量模式下展开子进程 stdout/stderr 和内部诊断。 |
| `--output-dir <dir>` | 指定输出目录，必须是绝对路径。 |
| `--log <path>` | 指定日志路径，必须是绝对路径。 |
| `--timeout-seconds <n>` | 单文件转换、宿主启动和 direct worker 等待超时，默认 `120` 秒。 |
| `--retries <n>` | 批量模式下失败重试次数，默认 `1`。 |
| `--no-overwrite` | 目标已存在时不覆盖。 |
| `--dry-run` | 只输出执行计划，不启动 CAD，不写 `tangent.mnl`。 |
| `--json` | 输出 JSON 风格执行计划。 |
| `--tbatsave-bindmode <n>` | 逆向 / 诊断保留参数。当前 direct worker 生产链路不把它作为稳定转换参数。 |
| `--tbatsave-bindref <n>` | 逆向 / 诊断保留参数。当前 direct worker 生产链路不把它作为稳定转换参数。 |

未指定 `--timeout-seconds` 时，默认超时为 2 分钟。该值会进入执行计划：

```text
timeout_seconds=120
```

目录批量模式下，父进程会把同一个超时时间传给每个 `t3conv.exe -s <dwg> -o <target>` 子进程。

## 路径推导

`ConfigLoader` 从 `t3conv.ini` 所在目录推导全部运行路径。

| 名称 | 推导规则 |
| --- | --- |
| `workspace_root` | `t3conv.ini` 所在目录 |
| `tgstart_exe` | `<tangent_root>\TGStart.exe` |
| `tangent_mnl` | `<tangent_root>\SYS\tangent.mnl` |
| `tangent_sys_dir` | AutoCAD 2020 -> `sys23x64`；AutoCAD 2021 -> `sys24x64` |
| `runtime_root` | `<workspace_root>\runtime\tgstart_host` |
| `var_root` | `<workspace_root>\var` |
| `runtime_dir` | `<workspace_root>\var\runtime` |
| `host_dir` | `<workspace_root>\var\host` |
| `logs_dir` | `<workspace_root>\var\logs` |
| `bootstrap_state_path` | `<workspace_root>\var\bootstrap.json` |
| `bridge_runtime_path` | `<workspace_root>\var\runtime\tangent_mnl_bridge.runtime.lsp` |
| `host_runtime_path` | `<workspace_root>\var\runtime\tbatsave_experimental_trigger.runtime.lsp` |
| `worker_status_path` | `<workspace_root>\var\host\worker_status.txt` |
| `font_map_path` | `<workspace_root>\var\runtime\fontmap.fmp` |
| `font_dir` | 固定为 `<workspace_root>\fonts` |
| `autocad_fonts_dir` | `<autocad_root>\Fonts` |

生产源码和 LSP 模板不得写死本机路径。渲染后的 `var\runtime\*.runtime.lsp` 可以包含绝对路径，因为它们是启动时生成的本机运行态文件。

## 运行态文件布局

源码模板位于：

- [`runtime/tgstart_host/tbatsave_experimental_trigger.lsp`](../runtime/tgstart_host/tbatsave_experimental_trigger.lsp)
- [`runtime/tgstart_host/tangent_mnl_bridge.lsp`](../runtime/tgstart_host/tangent_mnl_bridge.lsp)

启动时渲染到：

```text
<workspace_root>\var\runtime\tangent_mnl_bridge.runtime.lsp
<workspace_root>\var\runtime\tbatsave_experimental_trigger.runtime.lsp
<workspace_root>\var\runtime\fontmap.fmp
```

宿主状态文件位于：

```text
<workspace_root>\var\host\host_ready.txt
<workspace_root>\var\host\host_bootstrap.txt
<workspace_root>\var\host\worker_status.txt
```

日志和启动快照位于：

```text
<workspace_root>\var\logs\trigger.log
<workspace_root>\var\bootstrap.json
```

当前生产路径不使用 LSP job/result 文件。host runtime LSP 只负责环境准备和写入 `host_ready.txt`；direct worker 的内部状态写入 `worker_status.txt`，仅用于 debug 或故障排查。

这些 `.txt` 都是运行态标记或诊断文件，不需要用户手工创建：

| 文件 | 作用 |
| --- | --- |
| `host_bootstrap.txt` | C++ 写入启动触发标记，`tangent.mnl` bridge 看到后加载 runtime LSP。 |
| `host_ready.txt` | LSP 环境准备完成后写入，C++ 用它判断宿主是否 ready。 |
| `worker_status.txt` | direct worker 写入成功 / 失败、步骤、`save_result` 等诊断。 |

除上表列出的 3 个文件外，当前生产链路没有其他 `.txt` 状态标记需要保留。

`var` 下所有文件都是本机运行态产物，可以删除，程序会按需重新生成。

## 启动自修复

真实执行时，`t3conv.exe` 会处理以下事项：

1. 创建 `var\host`、`var\logs`、`var\runtime`。
2. 渲染 `runtime\tgstart_host` 下的 LSP 模板到 `var\runtime`。
3. 生成 `var\runtime\fontmap.fmp`。
4. 同步项目字体到 AutoCAD `Fonts` 目录。
5. 规范化 `<tangent_root>\SYS\tangent.mnl`，确保存在唯一最小 bridge load。
6. 写入 `<workspace_root>\var\bootstrap.json`，记录当前初始化状态。

Tianzheng 目录允许的最小改动只有：

```lisp
(load "<workspace_root>/var/runtime/tangent_mnl_bridge.runtime.lsp" nil)
```

`tangent.mnl` 不直接加载项目模板，也不保存多条旧 bridge 入口。不要手工把项目脚本复制到 Tianzheng 目录，启动时缺失就由 `t3conv.exe` 自动修复。

启动 bridge 有单会话加载守卫：

- `host_bootstrap.txt` 存在时加载 runtime。
- 如果只看到已有 `host_ready.txt`，同一个 CAD 会话内不会重复加载 runtime。
- 这样可以避免启动阶段重复触发窗口或动作。

## 字体逻辑

项目字体入口为：

```text
<workspace_root>\fonts
```

启动自检时会处理 `.shx` 和 `.ttf`：

- AutoCAD `Fonts` 目录中不存在同名字体：复制进去。
- 同名且文件大小相同：认为已处理，跳过。
- 同名但文件大小不同：不覆盖 AutoCAD 原文件，复制成 `t3conv_<原名>_<size>_<hash>.<ext>`。
- 已存在同名项目专属副本且大小相同：跳过。
- `fonts` 目录不存在或为空：记录诊断，不中断转换。

运行时会设置：

```text
FONTMAP=<workspace_root>\var\runtime\fontmap.fmp
FONTALT=<fonts.fontalt>
ACADPREFIX=<autocad_root>\Fonts
FILEDIA=0
CMDDIA=0
CMDECHO=0
EXPERT=5
PROXYNOTICE=0
PROXYSHOW=0
PROXYWEBSEARCH=0
XREFNOTIFY=0
XLOADCTL=0
XREFCTL=0
ISAVEBAK=0
DownGradeSaveFlag=T
g_bSaveProxyGraph=1
```

这样新增项目字体可以自动生效，同时避免覆盖 AutoCAD 自带字体。

其中 `CMDECHO=0` 和 `CMDDIA/FILEDIA=0` 用于减少命令行回显和对话框；`ISAVEBAK=0` 避免生成 `.bak`；`DownGradeSaveFlag=T` 与 `g_bSaveProxyGraph=1` 是 Tianzheng / AutoCAD 兼容保存相关变量，保持和 runtime LSP 模板一致。

## 转换链路

`t3conv.exe` 会优先在已加载 `tch_kernal.arx` 的 Tianzheng `acad.exe` 中执行 direct worker。

核心调用顺序：

```text
AcDbDatabase ctor
  -> readDwgFile(source)
  -> sub_1C01B310
  -> sub_1C01E850
  -> SaveAsTArch3(database, output, selector, false)
  -> AcDbDatabase dtor
```

当前已验证：

```text
Tianzheng version = T3
CAD version = AutoCAD 2004
selector = 0x10
SaveAsTArch3 success = 5100
```

### 输出搬运

为避免 direct worker 直接覆盖目标路径，程序会先：

1. 把源 DWG 复制到 `<workspace_root>\_t3conv_work\<source_name>.dwg`。
2. 让 direct worker 输出到 `_t3conv_work\batch_output\<source_stem>_t3.dwg`。
3. direct worker 成功后，再移动或复制到用户指定目标路径。

`_t3conv_work` 是临时 staging 目录，不是用户产物。单文件转换成功或失败后都会尝试删除该目录；需要保留现场时使用 `-d` 查看 stdout/stderr 和 `worker_status.txt`。

### 成功诊断

成功诊断通常包含：

```text
status=success
host_action=tbatsave_direct_worker_succeeded
tbatsave_direct_worker_read_status=0
tbatsave_direct_worker_save_result=5100
tbatsave_direct_worker_selector=16  # 0x10
timeout_seconds=120
```

### 失败行为

如果 direct worker 不可用或返回失败，`t3conv.exe` 直接返回失败并打印完整诊断：

```text
status=failure
host_action=tbatsave_direct_worker_failed_no_ui_fallback
ui_fallback=disabled
```

生产路径不再转入会弹窗的 `TBatSave`、`TBatSaveFolder` 或 `PLDC` 命令。这样单张异常图只影响本次调用，批量 runner 可以记录错误、重启宿主并继续下一张。

## 卡死恢复

程序复用宿主前会判断 Tianzheng `acad.exe` 健康状态。健康宿主必须满足：

- 存在 `acad.exe`。
- 已加载 `tch_kernal.arx`。
- 可见窗口未被 Windows 标记为 hung。

如果检测到 Tianzheng CAD 卡死，程序会只强杀已加载 `tch_kernal.arx` 的 `acad.exe`，避免误杀普通 AutoCAD，并清理：

```text
host_ready.txt
host_stop.txt
worker_status.txt
```

之后重新通过 `TGStart.exe` 拉起宿主。

诊断示例：

```text
host_recovery=hung_tianzheng_acad_killed
host_action=launch_tgstart_tbatsave_after_hung_recovery
```

## 批量转换

`t3conv.exe` 同时支持单文件和目录批量。Java 或其他外部程序只需要传输入和输出路径：

```powershell
.\t3conv.exe -s "C:\absolute\source.dwg" -o "C:\absolute\source_t3.dwg"
```

当 `-s` 是目录时自动进入批量父进程模式：

```powershell
.\t3conv.exe -s "C:\absolute\source-folder" -o "C:\absolute\t3-output"
```

批量父进程会逐张启动同一个 `t3conv.exe -s <dwg> -o <target>` 子进程。这样不依赖 PowerShell 脚本，同时保留单张转换崩溃不拖死整个批量任务的隔离能力。不要并发向同一个 Tianzheng `acad.exe` 注入多个 worker。

批量模式会在文件之间复用同一个 Tianzheng CAD 宿主。子进程超时、启动失败、崩溃，或 host/direct worker 动作表明当前宿主不应继续复用时，父进程会重启宿主；普通图纸转换失败不会触发每张都重启，每批次重启次数有上限。

批量 debug：

```powershell
.\t3conv.exe -s "C:\absolute\source-folder" -o "C:\absolute\t3-output" -d
```

批量默认日志和单文件一致，位于 `t3conv.exe` 所在 workspace；批量结束后会把汇总追加到同一个 `t3conv.log`：

```text
t3conv.log
log\t3conv.log.1
log\t3conv.log.2
```

默认 `t3conv.log*` 是简洁流水格式，只记录：

```text
<copyable command>
name=<source file name>
source=<absolute source dwg>
output=<absolute target dwg>
status=<success|failure>
started_at=<local time, yyyy-MM-dd HH:mm:ss>
duration=<seconds>s
size=<kilobytes>kb
error=<only when failed>
--------------------------------------------------------------------------------
```

默认日志中 `status` 是最终转换结果。`exit_code`、`save_result`、内部 `status`、目标文件字节数、字体路径、host_ready、worker 地址和 stdout/stderr 只在 `-d` / `--debug` 中展开。

批量结束后，`t3conv.log` 会追加一个分隔后的汇总块：

```text
================================================================================
batch_summary
source_dir=<absolute source folder>
output_dir=<absolute output folder>
total=20
success=20
failed=0
total_seconds=373.285
avg_seconds=18.664
================================================================================
```

日志滚动规则对单文件和批量转换一致：基础 `t3conv.log` 超过 20 MB 后，新日志会写入同级 `log` 文件夹中的 `t3conv.log.1`、`t3conv.log.2` 等文件；滚动文件最多保留 20 个，全部写满后会清空旧滚动日志并从 `.1` 重新开始。

## 错误码

| 返回码 | 名称 | 常见原因 |
| ---: | --- | --- |
| `0` | `success` | 转换成功。 |
| `1` | `file_not_found` | 源文件、配置文件、`TGStart.exe` 或安装目录缺失。 |
| `2` | `file_corrupt` | 预留错误码；当前 direct worker 读取失败通常会表现为保存失败或函数调用失败。 |
| `3` | `load_timeout` | 等待宿主 ready 超时。 |
| `4` | `tch_object_missing` | 预留错误码。 |
| `5` | `arx_load_failed` | 预留错误码。 |
| `6` | `function_call_failed` | direct worker 不可用、远程调用失败或 UI fallback 被禁用。 |
| `7` | `save_failed` | 目标已存在且 `--no-overwrite`、输出搬运失败或批量存在失败项。 |
| `8` | `out_of_memory` | 预留错误码。 |
| `9` | `argument_error` | CLI 参数缺失、未知、格式错误或取值越界。 |
| `99` | `unexpected_crash` | 进程启动失败或未知异常。 |

## 排障

| 现象 | 优先检查 |
| --- | --- |
| `AutoCAD is not installed or not found` | 设置 `autocad_root` 或 `T3CONV_AUTOCAD_ROOT`，确认 `<autocad_root>\Fonts` 存在。 |
| `Tianzheng T20V9 is not installed or not found` | 设置 `tangent_root` 或 `T3CONV_TANGENT_ROOT`，确认根目录存在。 |
| `TGStart.exe not found` | 检查 `tangent_root` 是否指向 Tianzheng T20 V9 根目录，而不是 `SYS` 子目录。 |
| `host_ready.txt` 未生成 | 直接运行一次普通转换，查看 `var\logs\trigger.log`。 |
| `tbatsave_direct_worker=acad_not_found` | 确认宿主已启动，或直接让单文件转换触发启动。 |
| `tbatsave_direct_worker=tch_kernal_not_loaded` | Tianzheng ARX 未加载完成，等待后重试或重启宿主。 |
| `tbatsave_direct_worker=timeout` | 单张 DWG 可能卡住；可以增大 `--timeout-seconds`，批量模式会记录失败并继续下一张。 |
| `tbatsave_direct_worker=output_missing` | 查看 `worker_status.txt`、批量 debug 日志和目标目录权限。 |
| 字体替换异常 | 把缺失 `.shx` / `.ttf` 放入 `fonts`，重新运行，让程序同步并重建 `fontmap.fmp`。 |
| 批量中途失败 | 查看 `t3conv.log` 末尾的 `batch_summary` 和失败文件记录；追加 `-d` 可展开子进程输出。 |

## 构建、验证与打包

构建：

```powershell
cmake -S . -B ..\_build\t3-conv-clean
cmake --build ..\_build\t3-conv-clean --config Debug
```

执行计划：

```powershell
..\_build\t3-conv-clean\src\t3conv\Debug\t3conv.exe `
  --dry-run `
  -s "C:\absolute\sample.dwg"
```

预期包含：

```text
mode=tgstart_tbatsave_no_ui
worker_status=<workspace_root>\var\host\worker_status.txt
timeout_seconds=120
script_contents:
```

单元测试：

```powershell
python -m unittest discover -s tests
```

打包：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\package.ps1
```

默认覆盖生成 `<项目根目录>\release\t3-conv.zip`。ZIP 只包含：

```text
t3conv.exe
t3conv.ini
restore-autocad-settings.cmd
runtime\tgstart_host\*.lsp
fonts\
docs\README-packaged.md
```

不要打包 `var`。`var\runtime`、`var\host`、`var\logs` 和 `var\bootstrap.json` 都是目标机器首次运行时生成的本机状态。

## 当前边界

- 当前稳定生产目标是单文件无 UI 转 T3；目录批量是外层逐文件调用。
- 源文件、目标文件、输出目录和日志路径都要求绝对路径。
- `TBatSaveFolder` 原生 ctx 构造不是当前最小生产入口。
- 教育版警告目前只能尽量抑制提示，不保证剥离教育版标记。
- 代理对象、外部参照和确认对话框通过宿主静默变量和 direct worker 主路径抑制。
- 需要启动 CAD 时，必须通过 `<tangent_root>\TGStart.exe`，不要直接启动裸 `acad.exe`。
- Tianzheng 安装目录本身不由本项目清理。项目内发布包只保留生产链路需要的运行文件。
