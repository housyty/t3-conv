# t3-conv

[English README](./README.md)

`t3-conv` 是一个 Windows 命令行工具，用于通过无 UI 的 `TBatSave` 链路，将天正 DWG 转换为 T3 DWG。

## 便携包下载

可直接下载 Windows x64 便携包：[`release/t3-conv.zip`](./release/t3-conv.zip)。

该包可以解压后运行，但它不是独立的天正或 AutoCAD 替代品；目标机器仍然需要安装 Tianzheng TArch T20 V9 和受支持的 AutoCAD。

相比弹窗式 CAD 界面操作，本工具省掉了重复点击菜单、确认对话框、逐文件人工等待和不必要的宿主重启。按当前本地冒烟测试，宿主就绪后单文件 direct worker 转换通常是数秒到十几秒级；目录批量场景通常会比人工弹窗流程快数倍，因为可以无人值守并复用 Tianzheng CAD 宿主。实际速度取决于 DWG 大小、字体、外部参照、机器性能以及宿主是否已热启动。

当前生产链路刻意保持收敛：

```text
t3conv.exe
  -> 读取 t3conv.ini
  -> 自动定位 Tianzheng T20 V9 和 AutoCAD
  -> 启动或复用后台 Tianzheng CAD 宿主
  -> 等待天正 ARX、字体和无弹窗变量准备完成
  -> 调用天正 T3 保存逻辑转换单个 DWG
  -> 输出 T3 DWG 和简洁转换日志
  -> 失败时返回诊断，不弹出交互式 UI
```

实际运行时，`t3conv.exe` 启动 CAD 的目的不是做界面自动化，而是创建一个真实的天正运行环境。宿主就绪后，转换由已验证的 TBatSave direct worker 驱动，不依赖点击菜单或发送会弹窗的 UI 命令。

本项目不使用 `accoreconsole`、旧 Python 原型、ARX 测试骨架、job/result 文件协议，也不在失败时回退到会弹窗的 UI 命令。

## 功能特性

- 支持单个 DWG 转 T3，默认输出 `_t3.dwg`。
- 支持目录批量转换，内部逐个启动隔离的 `t3conv.exe` 子进程。
- 可复用已就绪的 Tianzheng CAD 宿主。
- 复用宿主前会检测并恢复卡死的 Tianzheng `acad.exe`。
- 运行状态集中放在 `var`，避免把项目脚本堆进 Tianzheng 安装目录。
- 自动把项目 `.shx` / `.ttf` 字体安全同步到 AutoCAD `Fonts`，遇到同名冲突不会覆盖原文件。
- 输出简洁日志、日志内批量汇总和可选 debug 诊断。

## 环境要求

- Windows x64。
- 本机已安装 Tianzheng TArch T20 V9。
- 本机已安装 AutoCAD 2020-2026，且存在可用的 `Fonts` 目录。
- 从源码构建时需要 CMake 3.20+ 和支持 C++17 的 MSVC 工具链。

便携包和本仓库不包含 Tianzheng 或 AutoCAD 文件。

## 配置

唯一配置文件是 [`t3conv.ini`](./t3conv.ini)。自动检测可用时，配置项可以全部保持注释：

```ini
[paths]
; tangent_root=C:\path\to\TArchT20V9
; autocad_root=C:\Program Files\Autodesk\AutoCAD 2020

[fonts]
; fontalt=HZTXT.SHX
```

解析优先级：

- `paths.tangent_root`：INI 配置，其次 `T3CONV_TANGENT_ROOT`，最后自动检测。
- `paths.autocad_root`：INI 配置，其次 `T3CONV_AUTOCAD_ROOT`，最后从 AutoCAD 2020-2026 中自动检测。
- `fonts.fontalt`：INI 配置，其次 `T3CONV_FONTALT`，最后使用 `HZTXT.SHX`。
- 项目字体固定读取 [`fonts`](./fonts) 目录。

如果没有找到 Tianzheng T20 V9、`TGStart.exe`、AutoCAD 或 AutoCAD `Fonts` 目录，`t3conv.exe` 会在启动转换前退出并报错。

## 使用方式

源文件、目标文件、输出目录和日志路径都需要使用绝对路径。

用最少参数转换单个 DWG：

```powershell
.\t3conv.exe -s "C:\path\source.dwg"
```

源文件也可以作为第一个位置参数传入：

```powershell
.\t3conv.exe "C:\path\source.dwg"
```

默认输出：

```text
C:\path\source_t3.dwg
```

显式指定目标文件：

```powershell
.\t3conv.exe -s "C:\path\source.dwg" -o "C:\path\source_t3.dwg"
```

批量转换目录：

```powershell
.\t3conv.exe -s "C:\path\dwgs" -o "C:\path\t3-output"
```

当 `-s` 指向目录时，`t3conv.exe` 会自动进入批量父进程模式；不需要 `--batch` 参数。

真实执行时默认输出保持简洁。需要查看执行计划时使用 `--dry-run`，需要内部诊断时使用 `-d` / `--debug`。

单文件和目录批量转换默认都把 `t3conv.log` 写到 `t3conv.exe` 所在的便携 workspace 目录：

- `t3conv.log`：简洁的逐文件日志。非 debug 日志会记录可复制命令、源文件、输出文件、最终状态、`started_at` 本地执行时间点、耗时、输出大小和失败摘要。
- `log\t3conv.log.1`、`log\t3conv.log.2` 等：滚动日志。`t3conv.log` 超过 20 MB 后，新日志会写入 `log` 文件夹；滚动文件超过 20 个后，会自动清空旧滚动日志并从 `.1` 重新开始。
- `batch_summary`：目录批量结束后追加在 `t3conv.log` 末尾，记录总数、成功数、失败数、总耗时和平均耗时。

追加 `-d` / `--debug` 后，会输出执行计划，并在日志中展开子进程 stdout/stderr 和内部诊断。

批量转换会在文件之间复用同一个 Tianzheng CAD 宿主。只有超时、启动失败、崩溃或明确的宿主侧失败时才会重启宿主；普通单张图纸转换失败只记录日志，然后继续下一张。

单文件转换会在 workspace 下临时创建 `_t3conv_work` 作为 staging 目录，避免 direct worker 直接覆盖目标文件；每次转换结束后会自动删除该目录。

宿主控制是可选能力，正常使用时不需要手动执行。普通单文件和目录转换会按需自动复用或启动 Tianzheng CAD 宿主；只有排障、预热或手动停止时才需要使用相关 host 参数。

## 命令参数

| 参数 | 说明 |
| --- | --- |
| `-s <path>`, `--source <path>` | 源 DWG 或源目录，必须是绝对路径；也可以作为第一个位置参数传入。 |
| `-o <path>`, `--target <path>` | 单文件转换时表示目标 DWG；源路径是目录时表示批量输出目录。必须是绝对路径。 |
| `--output-dir <dir>` | 派生输出目录，必须是绝对路径。 |
| `--log <path>` | 显式日志路径，必须是绝对路径。 |
| `-d`, `--debug` | 输出执行计划，并展开 stdout/stderr 和内部诊断。 |
| `--timeout-seconds <n>` | 宿主启动、单文件转换和 direct worker 等待超时，默认 `120` 秒。 |
| `--retries <n>` | 批量失败重试次数，默认 `1`。 |
| `--no-overwrite` | 目标已存在时不覆盖。 |
| `--dry-run` | 只输出执行计划，不启动 CAD，也不修改 `tangent.mnl`。 |
| `--json` | 输出 JSON 风格的执行计划。 |
| `--tbatsave-bindmode <n>` | 逆向 / 诊断保留参数。当前 direct worker 生产链路不把它作为稳定转换参数。 |
| `--tbatsave-bindref <n>` | 逆向 / 诊断保留参数。当前 direct worker 生产链路不把它作为稳定转换参数。 |
| `--host-status` | 检查后台宿主是否 ready。 |
| `--host-stop` | 请求后台宿主循环停止。 |
| `--host-start` | 可选预热命令，用于启动或复用后台 Tianzheng CAD 宿主循环。 |

## 构建

```powershell
cmake -S . -B ..\_build\t3-conv-release
cmake --build ..\_build\t3-conv-release --config Release
```

生成的程序位于：

```text
..\_build\t3-conv-release\src\t3conv\Release\t3conv.exe
```

## 打包

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\package.ps1
```

脚本默认构建 Release 版本，并生成：

```text
<项目父目录>\dist\t3-conv.zip
```

ZIP 只包含运行必需文件：

- `t3conv.exe`
- `t3conv.ini`
- `runtime\tgstart_host\*.lsp`
- `fonts\`
- `docs\README-packaged.md`

ZIP 不包含 `src`、`tests`、`var`、构建中间文件、Tianzheng 文件或 AutoCAD 文件。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| [`src/common`](./src/common) | 公共类型、错误码和路径工具。 |
| [`src/t3conv`](./src/t3conv) | 当前生产 CLI 实现。 |
| [`runtime/tgstart_host`](./runtime/tgstart_host) | 启动时渲染到 `var/runtime` 的 LSP 模板。 |
| [`fonts`](./fonts) | 可选项目字体，会同步到 AutoCAD `Fonts`。 |
| [`docs`](./docs) | 开发文档和原生无 UI 协议说明。 |
| [`tools/package.ps1`](./tools/package.ps1) | Windows 打包脚本。 |
| `var/host` | 生成的宿主标记和 direct worker 状态文件。 |
| `var/runtime` | 生成的本机 runtime LSP 和 `fontmap.fmp`。 |
| `var/logs/trigger.log` | LSP 侧诊断日志。 |
| `var/bootstrap.json` | 生成的启动状态快照。 |

`var` 下的文件都是运行态产物，可以删除，程序会按需重新生成。

## 文档

- [t3conv 开发指南](./docs/t3conv-developer-guide.md)：说明本项目的配置、CLI、运行态布局、宿主复用、启动自修复、转换、批量、日志、排障和打包行为。
- [TBatSave direct worker 协议](./docs/tbatsave-direct-worker-protocol.md)：说明已逆向验证的 direct worker 协议，适合其他语言实现同等能力时参考。

## 当前边界

- 当前已验证的生产目标是通过 TBatSave direct worker 输出单文件 T3。
- 目录转换是外层批量 runner，内部逐张调用单文件链路。
- direct worker 失败时直接返回错误和诊断，不会回退到会弹窗的 `TBatSave`、`TBatSaveFolder` 或 `PLDC` 命令。
- 源路径、目标路径、输出目录和日志路径都必须是绝对路径。
- 教育版提示只能尽量通过宿主变量抑制，本工具不保证剥离教育版标记。
- 外部参照、代理对象、字体缺失、异常图纸和确认对话框，通过宿主静默变量、字体映射、direct worker 检查和逐文件诊断处理，不做交互式 UI 自动化。

## 免责声明

本仓库内容仅用于技术研究、互操作性分析、自动化测试、协议研究、逆向分析及兼容性学习交流。请勿将本项目用于商业破解、授权绕过、非法传播、未授权商业使用或任何违反相关法律法规的用途。

本项目不包含、不提供、不分发、不捆绑、不销售，也不修改任何第三方商业软件、许可证或专有组件。用户需自行合法获取并安装所需的软件及有效授权。

仓库中涉及的相关软件、名称、商标、版权及其他知识产权均归其原作者或所属公司所有，包括但不限于天正、Autodesk 等相关权利方。

本项目的目的仅限于研究软件互操作性、自动化流程及相关技术实现，不针对任何商业软件的破解、绕过或替代。

如果任何版权持有人或相关权利人认为仓库中的部分内容存在不当、侵权或需要移除的情况，请通过 Issue 或邮件联系仓库维护者。我会在确认后及时进行审核、修改或删除相关内容。
