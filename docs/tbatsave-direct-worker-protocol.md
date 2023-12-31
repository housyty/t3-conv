# TBatSave direct worker 协议

## 适用范围

本文档面向需要用其他语言重新实现 `TBatSave` direct worker 无 UI 调用的开发者。

它不依赖 `t3conv`、不描述某个项目的 CLI 参数，也不要求使用 C++。任何能完成 Windows 进程枚举、远程内存读写和远程线程调用的语言都可以按本文档实现，例如 C#、Go、Rust、Python、Node.js native addon 或 C/C++。

当前已实机验证的能力是：

- 在 Tianzheng T20 V9 已加载的 `acad.exe` 进程内执行。
- 对单个 DWG 执行 T3 转换。
- 无需弹出 `TBatSave` UI。
- 成功输出 `<源文件名>_t3.dwg`。

当前不把 `TBatSaveFolder` 的原生 ctx 构造作为稳定入口；批量转换建议由外层枚举 DWG 后逐个调用本文档描述的单文件协议。

## 完备性结论

如果目标是开发“固定导出为 T3 + AutoCAD 2004 DWG”的无 UI 转换功能，本文档已经给出了可实现所需的最小完整协议：

```text
天正版本 = T3
CAD 版本 = AutoCAD 2004
输出位置 = 调用方指定，通常为源文件夹
输出文件名 = <source_stem>_t3.dwg
绑定参照 = 不作为稳定参数
绑定 / 插入 = 不作为稳定 direct worker 参数
```

如果目标是 100% 复刻 `TBatSave` 对话框的全部 UI 参数，当前协议还不是完整替代品。原因是“绑定参照 / 绑定 / 插入”三项属于 `TBatSave` 单文件导出对象的 UI 状态，不属于当前 direct worker 的稳定参数；`TBatSaveFolder` 的原生 ctx 构造也尚未作为稳定入口交付。

因此，本文档的交付边界是：

| 能力 | 状态 |
| --- | --- |
| 单文件无 UI 转 T3 | 已验证，可开发 |
| 输出 AutoCAD 2004 DWG | 已验证，可开发 |
| 输出到原文件夹 | 调用方路径策略，可开发 |
| 自动添加 `_t3` 后缀 | 调用方路径策略，可开发 |
| 外层批量处理 | 调用方枚举文件，可开发 |
| 任意 CAD 版本选择 | 枚举已整理，只有 AutoCAD 2004 在当前 direct worker 链路实测 |
| 任意 Tianzheng 版本选择 | 枚举已整理，只有 T3 在当前 direct worker 链路实测 |
| 绑定参照 / 绑定 / 插入 | 字段线索已整理，尚不是稳定 direct worker 参数，不建议作为业务参数承诺 |

## 运行前提

调用方必须先准备好宿主进程：

1. 通过 Tianzheng 的 `TGStart.exe` 启动 CAD。
2. 等待 `acad.exe` 出现。
3. 等待目标 `acad.exe` 加载 `tch_kernal.arx`。

不要直接启动裸 `acad.exe` 作为 Tianzheng 宿主。当前验证链路要求 `tch_kernal.arx` 已在进程内加载。

目标进程识别条件：

| 条件 | 说明 |
| --- | --- |
| 进程名 | `acad.exe` |
| 必需模块 | `tch_kernal.arx` |
| 推荐选择 | 多个 `acad.exe` 同时存在时，优先选择最新启动且已加载 `tch_kernal.arx` 的进程 |

## 核心结论

实际可稳定调用的最小链路不是完整 `TBatSave` 命令 handler，而是 `TBatSave` 在 T3 分支内部使用的原生保存 worker。

已验证调用顺序：

```text
AcDbDatabase::ctor(db, 0, 1)
  -> AcDbDatabase::readDwgFile(db, source_path, 3, 0, nullptr)
  -> sub_1C01B310(db)
  -> sub_1C01E850(db)
  -> selector = 0x10
  -> SaveAsTArch3(db, output_path, selector, 0)
  -> AcDbDatabase::dtor(db)
```

成功条件：

```text
readDwgFile 返回 0
SaveAsTArch3 返回 5100 (0x13EC)
输出 DWG 文件存在
```

## UI 参数映射

本节对应 `TBatSave` 对话框中的参数，用于其他语言实现时做产品接口设计。

### 文件名

direct worker 不打开文件选择 UI。调用方必须显式传入：

```text
source_path: 绝对路径，UTF-16LE，NUL 结尾
output_path: 绝对路径，UTF-16LE，NUL 结尾
```

### 天正版本

截图中的“天正版本”对应 `TBatSave` 内部的目标 Tianzheng version，也就是逆向笔记里的 `r13`。

| UI 文案 | target version | 默认后缀 | worker | 当前协议状态 |
| --- | ---: | --- | --- | --- |
| 天正 3 文件 | `3` | `_t3.dwg` | `SaveAsTArch3 / sub_1C027310` | 已验证 |
| 天正 5 文件 | `5` | `_t5.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| 天正 6 文件 | `6` | `_t6.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| 天正 7 文件 | `7` | `_t7.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| 天正 8 文件 | `8` | `_t8.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| 天正 9 文件 | `9` | `_t9.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V2 文件 | `10` | `_t20V2.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V3 文件 | `11` | `_t20V3.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V4 文件 | `12` | `_t20V4.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V5 文件 | `13` | `_t20V5.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V6 文件 | `14` | `_t20V6.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V7 文件 | `15` | `_t20V7.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V8 文件 | `16` | `_t20V8.dwg` | `sub_1C023A80` | 已逆向，未纳入当前协议 |
| T20 V9 文件 | `17` | `_t20V9.dwg` | 未确认 | 不可作为稳定入口 |

当前“无 UI 转 T3”只需要固定：

```text
target_version = 3
suffix = "_t3.dwg"
worker = SaveAsTArch3 / sub_1C027310
```

### CAD 版本

截图中的“CAD 版本”对应最终 DWG 落盘格式。当前 direct worker 通过 `SaveAsTArch3` 的 `selector` 参数控制。

`sub_1C024940` 已确认接受 `0x0E..0x14` 这组 selector。结合当前实测 `AutoCAD 2004 文件 -> selector=0x10/16`，可整理为：

| UI 文案 | AutoCAD API save code | TBatSave selector | 当前协议状态 |
| --- | ---: | ---: | --- |
| AutoCAD R14 文件 | `12` | `0x0E` / `14` | 枚举线索 |
| AutoCAD 2000 文件 | `24` | `0x0F` / `15` | 枚举线索 |
| AutoCAD 2004 文件 | `32` | `0x10` / `16` | 已验证 |
| AutoCAD 2007 文件 | `36` | `0x11` / `17` | 枚举线索 |
| AutoCAD 2010 文件 | `48` | `0x12` / `18` | 枚举线索 |
| AutoCAD 2013 文件 | `56` | `0x13` / `19` | 枚举线索 |
| AutoCAD 2018 文件 | `60` | `0x14` / `20` | 枚举线索 |

注意：

- `AutoCAD API save code` 是 COM / AutoCAD API 层常见保存枚举。
- `TBatSave selector` 是 Tianzheng 内部保存 helper 使用的枚举。
- direct worker 调 `SaveAsTArch3` 时应传 `TBatSave selector`，不要传 AutoCAD API save code。
- 当前已实机验证的 T3 目标是 `selector=0x10`，也就是 AutoCAD 2004 DWG。

### 自动添加文件名后缀

direct worker 不读取该 UI 勾选项。它由调用方在外层实现。

推荐规则：

| UI 状态 | 调用方行为 |
| --- | --- |
| 勾选 | `output_path = <source_dir>\<source_stem>_t3.dwg` |
| 不勾选 | `output_path` 由调用方显式传入 |

为了避免覆盖源文件，转 T3 的默认实现应始终生成 `_t3.dwg`。

### 导出到原文件夹

direct worker 不读取该 UI 勾选项。它同样由调用方决定输出路径。

| UI 状态 | 调用方行为 |
| --- | --- |
| 勾选 | 输出目录使用源文件所在目录 |
| 不勾选 | 输出目录使用用户选择目录 |

### 绑定参照 / 绑定 / 插入

这 3 个 UI 参数不属于当前 direct worker 的稳定入参。

已确认的字段线索：

| UI 项 | 字段位置 | 当前状态 |
| --- | --- | --- |
| 绑定参照 | `[this+0x958+0xE8]` | 字段已定位，未接入 direct worker |
| 绑定 / 插入共享模式 | `this+0x950` | 字段已定位，`0 / 非 0` 两支已确认 |
| 绑定镜像控件 | `[this+0xA48+0xF0]` | UI 镜像字段 |
| 插入镜像控件 | `[this+0xB40+0xF0]` | UI 镜像字段 |

当前不能稳定写死：

```text
0 = 绑定，1 = 插入
```

或：

```text
0 = 插入，1 = 绑定
```

如果业务目标只是“无 UI 转 T3”，建议让 direct worker 按默认状态输出，不把绑定参照 / 绑定 / 插入作为稳定 API 承诺。若必须实现这些 UI 参数，需要继续实现完整 `TBatSave` 单文件导出对象实例捕获和 setter 调用，而不是只改全局值。

## 版本和地址表

以下 RVA 基于当前验证版本的 `tch_kernal.arx`。

已整理的验证环境：

| 项 | 值 |
| --- | --- |
| Tianzheng 产品 | TArch T20 V9 |
| AutoCAD 2020 ARX 目录 | `sys23x64` |
| AutoCAD 2021 ARX 目录 | `sys24x64` |

当前 C++ 实现会按 AutoCAD 宿主年份选择 Tianzheng ARX 目录：AutoCAD 2020 -> `sys23x64`，AutoCAD 2021 -> `sys24x64`。进程选择时会优先匹配计划中的 `<tangent_sys_dir>\tch_kernal.arx`，避免多版本 AutoCAD 同时运行时误选其它天正宿主。

同一 Tianzheng 根目录中可能同时存在 `sys18x64` 到 `sys24x64` 多套 ARX。调用方必须根据实际 `acad.exe` 加载的模块来计算 `tch_kernal_base`，不能假设固定目录。

调用方必须以模块实际加载基址为准，最终 VA 计算方式为：

```text
function_va = tch_kernal_base + rva
```

| 名称 | AutoCAD 2020 / `sys23x64` RVA | AutoCAD 2021 / `sys24x64` RVA | 说明 |
| --- | ---: | ---: | --- |
| `AcDbDatabase::ctor` | `0x64A8D0` | `0x64156A` | 构造数据库对象 |
| `AcDbDatabase::dtor` | `0x64A8D6` | `0x641570` | 析构数据库对象 |
| `AcDbDatabase::readDwgFile` | `0x64A912` | `0x6415AC` | 读取源 DWG |
| T3 前置组/集合预处理 | `0x01B310` | `0x01EB00` | 处理 `TCH_GROUP` 等组数据 |
| T3 前置块递归预处理入口 | `0x01E850` | `0x01E200` | 处理块参照递归转换 |
| `SaveAsTArch3` | `0x027310` | `0x0273A0` | T3 专用保存 worker |
| selector base 全局值 | `0x9E6C7C` | 不使用 | 当前 C++ direct worker 固定 `selector=0x10` |
| non-UI flag | `0x9F95B0` | 不写入 | 仅在已验证布局中写入 |
| UI state flag | `0x9E6C80` | 不写入 | 仅在已验证布局中写入 |

兼容性要求：

- 实现方应在调用前校验 `tch_kernal.arx` 版本或关键函数签名。
- 如果签名不匹配，不应继续远程调用。
- 这些 RVA 不是 AutoCAD 通用 API，而是 Tianzheng T20 V9 当前版本内的实现细节。

## 远程调用模型

推荐实现方式是在目标 `acad.exe` 中分配一段远程内存，写入：

1. 一小段 x64 调用 stub。
2. 控制块（control block）。
3. UTF-16LE 源路径。
4. UTF-16LE 输出路径。
5. 一块用于 `AcDbDatabase` 实例的远程内存。

然后使用 `CreateRemoteThread` 从 stub 起始地址执行。

### 推荐内存布局

可以使用以下布局：

| 区域 | 偏移 | 建议大小 | 说明 |
| --- | ---: | ---: | --- |
| code | `0x0000` | `< 0x0800` | x64 调用 stub |
| control | `0x0800` | `0x0200` | 控制块 |
| source path | `0x0A00` | `0x0800` | UTF-16LE 源 DWG 路径 |
| output path | `0x1200` | `0x0E00` | UTF-16LE 输出 DWG 路径 |
| db object | `0x2000` | `0x4000` | `AcDbDatabase` 对象内存 |

总分配大小：

```text
0x6000
```

内存权限：

```text
PAGE_EXECUTE_READWRITE
```

调用结束后必须释放远程内存。

## 控制块格式

控制块用于调用方读取远程执行结果。

字段按小端序存储：

| 偏移 | 类型 | 字段 | 说明 |
| ---: | --- | --- | --- |
| `0x00` | `uint64` | `magic` | 建议值 `0x314B524F57584254`，ASCII 语义为 `TBXWORK1` |
| `0x08` | `uint32` | `step` | 执行进度 |
| `0x0C` | `uint32` | `status` | worker 状态 |
| `0x10` | `int32` | `read_status` | `readDwgFile` 返回值 |
| `0x14` | `int32` | `save_result` | `SaveAsTArch3` 返回值 |
| `0x18` | `uint32` | `selector_base` | 可选记录字段；固定 AutoCAD 2004 时可写 `0` |
| `0x1C` | `uint32` | `selector` | 实际传给 `SaveAsTArch3` 的 selector |
| `0x20` | `uint32` | `thread_exit_code` | 远程线程退出码，通常等于 `status` |
| `0x24` | `uint32` | `reserved` | 保留 |
| `0x28` | `uint64` | `db_remote` | 远程 `AcDbDatabase` 内存地址 |
| `0x30` | `uint64` | `source_remote` | 远程源路径地址 |
| `0x38` | `uint64` | `output_remote` | 远程输出路径地址 |

`status` 取值：

| 值 | 说明 |
| ---: | --- |
| `0` | 未完成或未写入 |
| `1` | 成功 |
| `2` | 读取 DWG 失败 |
| `3` | 保存失败 |

`step` 取值：

| 值 | 说明 |
| ---: | --- |
| `1` | stub 已进入 |
| `2` | `AcDbDatabase::ctor` 已返回 |
| `3` | `readDwgFile` 已返回 |
| `4` | `sub_1C01B310` 已返回 |
| `5` | `sub_1C01E850` 已返回 |
| `6` | `SaveAsTArch3` 已返回 |
| `7` | 开始析构数据库对象 |
| `8` | 析构完成，准备返回 |

## ABI 和函数参数

目标进程是 Windows x64，必须遵守 Microsoft x64 ABI：

- 前 4 个整型或指针参数依次放入 `RCX`、`RDX`、`R8`、`R9`。
- 调用前预留 shadow space。
- 栈保持 16 字节对齐。
- 返回值从 `RAX/EAX` 读取。

当前验证 stub 使用的栈空间：

```text
sub rsp, 0x48
...
add rsp, 0x48
ret
```

### 1. 构造 `AcDbDatabase`

```c
AcDbDatabase_ctor(db, 0, 1)
```

寄存器：

```text
RCX = db_remote
RDX = 0
R8D = 1
CALL tch_kernal_base + layout.AcDbDatabase_ctor_rva
```

### 2. 读取 DWG

```c
readDwgFile(db, source_path, 3, 0, nullptr)
```

寄存器与栈参数：

```text
RCX = db_remote
RDX = source_path_remote
R8D = 3
R9D = 0
[RSP + 0x20] = 0
CALL tch_kernal_base + layout.AcDbDatabase_readDwgFile_rva
```

成功返回：

```text
EAX = 0
```

如果 `EAX != 0`，应跳到清理流程并设置 `status=2`。

### 3. T3 前置预处理

```c
layout.preprocess_groups(db)
layout.preprocess_blocks(db)
```

寄存器：

```text
RCX = db_remote
CALL tch_kernal_base + layout.preprocess_groups_rva

RCX = db_remote
CALL tch_kernal_base + layout.preprocess_blocks_rva
```

这两步来自 `TBatSave` T3 分支内部 worker 前置链。不要省略，否则输出可能不是完整 Tianzheng T3 降级结果。

### 4. 设置 selector

```c
selector = 0x10
```

当前实测 T3 + AutoCAD 2004 转换中，最终 `selector` 为 `16`。如果只实现截图里的默认“T3 文件 + AutoCAD 2004 文件”，跨语言实现可以直接传 `0x10`，不要依赖当前 UI 全局状态。

如果要跟随 `TBatSave` 当前 UI 中选中的 CAD 版本，可读取：

```c
selector_base = *(uint32_t*)(tch_kernal_base + 0x9E6C7C)
selector = selector_base + 0x0E
```

但这只适合“复用当前全局选择”的实现，不适合对外承诺可重复结果的无 UI 服务。当前 C++ 实现会记录 `selector_base` 和最终 `selector`，并以 `selector=0x10` 作为 T3 + AutoCAD 2004 的成功验收条件。

当前 C++ direct worker 对 AutoCAD 2020 和 AutoCAD 2021 都固定写入：

```text
selector_base = 0x10
selector = 0x10
```

这样不依赖不同 `sysXXx64` 中可能迁移或消失的 UI 全局变量。

### 5. 调用 T3 保存 worker

```c
SaveAsTArch3(db, output_path, selector, 0)
```

寄存器：

```text
RCX = db_remote
RDX = output_path_remote
R8D = selector
R9D = 0
CALL tch_kernal_base + layout.save_as_tarch3_rva
```

成功返回：

```text
EAX = 5100
```

如果 `EAX != 5100`，应设置 `status=3`。

### 6. 析构数据库对象

无论读取或保存是否成功，都应调用析构：

```c
AcDbDatabase_dtor(db)
```

寄存器：

```text
RCX = db_remote
CALL tch_kernal_base + layout.AcDbDatabase_dtor_rva
```

## 无 UI 标志

在 AutoCAD 2020 / `sys23x64` 执行远程 stub 前，建议写入以下两个进程内全局值：

```text
*(uint32_t*)(tch_kernal_base + 0x9F95B0) = 1
*(uint32_t*)(tch_kernal_base + 0x9E6C80) = 1
```

它们用于压制 Tianzheng 内部 UI 状态，避免进入交互式路径。

写入失败时，不建议继续调用。

AutoCAD 2021 / `sys24x64` 当前 direct worker 不写入这两个旧 RVA；执行前依靠 host 启动脚本设置静默环境，并通过关键函数签名校验确认运行时布局匹配。

## 弹窗与异常图纸处理策略

direct worker 的主路径不经过 `TBatSave` 对话框，因此普通确认对话框不会从命令 handler 中弹出。但 CAD 宿主仍可能因为字体、代理对象、外部参照或异常 DWG 触发 AutoCAD / Tianzheng 级别提示。实现方应在启动宿主后、执行远程 worker 前设置静默环境，并把逐文件失败写入日志。

推荐宿主静默变量：

```text
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
FONTMAP=<调用方生成的字体映射文件>
FONTALT=HZTXT.SHX
```

`ISAVEBAK=0` 用于避免转换时生成 `.bak`；`DownGradeSaveFlag=T` 与 `g_bSaveProxyGraph=1` 保持降级保存和代理图形兼容设置。当前 `runtime/tgstart_host/tbatsave_experimental_trigger.lsp` 会按这些值设置宿主环境。

字体策略不要只依赖 `FONTALT=simplex.shx`。更稳的做法是把项目内新增字体同步到 AutoCAD `Fonts` 目录，然后生成独立 `FONTMAP`，优先把常见中文字体映射到同步后的字体文件；只有映射不到时再使用 `FONTALT` 兜底。

示例 `FONTMAP`：

```text
hztxt.shx;<autocad_root>/Fonts/HZTXT.SHX
hzfs.shx;<autocad_root>/Fonts/HZFS.SHX
hzht.shx;<autocad_root>/Fonts/HZHT.SHX
hzkt.shx;<autocad_root>/Fonts/HZKT.SHX
hzst.shx;<autocad_root>/Fonts/HZST.SHX
txt.shx;<autocad_root>/Fonts/HZTXT.SHX
gbcbig.shx;<autocad_root>/Fonts/gbcbig0.shx
complex.shx;<autocad_root>/Fonts/HZTXT.SHX
simplex.shx;<autocad_root>/Fonts/HZTXT.SHX
```

项目内新增字体的处理规则应是：

- 先把 `<workspace_root>\fonts` 里的 `.shx` / `.ttf` 同步到 `<autocad_root>\Fonts`。
- 目标已存在且文件大小相同，认为已处理，跳过。
- 目标已存在但文件大小不同，不覆盖原字体；复制为 `t3conv_<原名>_<size>_<hash>.<ext>` 项目专属副本，并在 `FONTMAP` 中把原字体名映射到该副本。
- 项目专属副本已存在且大小相同，认为已处理，跳过。
- `fonts` 目录不存在或为空时，跳过同步。

| 风险 | 当前 direct worker 处理方式 | 推荐实现要求 |
| --- | --- | --- |
| 字体缺失 | worker 本身不弹出字体选择 UI，但 `readDwgFile` 过程中仍可能触发宿主字体替换逻辑。 | 项目字体先同步到 AutoCAD `Fonts` 目录，再设置 `FONTMAP` 和 `FONTALT`；日志记录同步结果和映射文件路径。 |
| 教育版警告 | 当前协议不保证剥离教育版标记，也不把“教育版清洗”作为 direct worker 的返回项。 | 静默变量可能减少提示，但真正剥离需要单独的预清洗或重写流程；如业务强依赖，必须另做可验证步骤，不应声称 direct worker 已保证剥离。 |
| 代理对象 | 可通过宿主环境设置 `PROXYNOTICE=0`、`PROXYSHOW=0`、`PROXYWEBSEARCH=0` 抑制提示。 | 不把代理对象提示当成交互入口；是否保留代理图形由 Tianzheng 保存 worker 和图纸内容决定，调用方只记录转换结果。 |
| 外部参照 | 当前 direct worker 读取单个 DWG，不实现绑定外部参照。 | 设置 `XREFNOTIFY=0`、`XLOADCTL=0`、`XREFCTL=0` 抑制提示；如需绑定外参，必须另做前置处理，不能把 `TBatSaveFolder` 或 direct worker 视为已绑定。 |
| 确认对话框 | 远程 worker 固定路径、固定 selector，不走文件选择和保存确认 UI；AutoCAD 2020 布局会额外写入两个 Tianzheng no-UI 标志。 | 设置 `FILEDIA=0`、`CMDDIA=0`、`EXPERT=5`，调用前删除旧输出，避免覆盖确认；超时后记录文件并进入下一张。 |
| 文件破坏 | `readDwgFile` 返回非 0 时设置 `status=2`，不继续调用 `SaveAsTArch3`。 | 逐文件记录 `read_status`、`step`、源路径和错误；批量任务继续后续文件，不让单张坏图阻塞全批次。 |

结论：字体缺失、代理对象、外部参照和确认对话框以“宿主静默变量 + direct worker 不走 UI”处理；文件破坏以 `read_status` 失败退出并记录；教育版警告目前只能抑制提示，不能声称已经剥离教育版标记。

## 调用流程伪代码

下面是跨语言实现时应遵循的流程。

```text
pid = find newest acad.exe with loaded tch_kernal.arx
tch_base = module_base(pid, "tch_kernal.arx")

open process with:
  PROCESS_CREATE_THREAD
  PROCESS_VM_OPERATION
  PROCESS_VM_READ
  PROCESS_VM_WRITE
  PROCESS_QUERY_INFORMATION

if using sys23x64:
  write uint32(1) to tch_base + 0x9F95B0
  write uint32(1) to tch_base + 0x9E6C80

remote = VirtualAllocEx(size=0x6000, PAGE_EXECUTE_READWRITE)

code_addr    = remote + 0x0000
control_addr = remote + 0x0800
source_addr  = remote + 0x0A00
output_addr  = remote + 0x1200
db_addr      = remote + 0x2000

write control block
write source path as UTF-16LE NUL-terminated string
write output path as UTF-16LE NUL-terminated string
write x64 stub
FlushInstructionCache(code_addr)

thread = CreateRemoteThread(start=code_addr)
wait up to 60 seconds

if timeout:
  terminate thread only as last resort
  release remote memory
  report timeout

read control block
release remote memory

success =
  control.magic == TBXWORK1 &&
  control.status == 1 &&
  control.read_status == 0 &&
  control.save_result == 5100 &&
  control.selector == 0x10 &&
  output file exists
```

## 建议对外参数模型

其他语言实现可以把本文档包装成一个简单 API。

### 单文件转换请求

```text
source_path: string      必填，绝对路径
output_path: string      可选，默认 <source_dir>\<source_stem>_t3.dwg
tianzheng_version: enum  当前固定 T3
cad_version: enum        当前固定 ACAD2004
overwrite: bool          默认 true
timeout_ms: uint32       默认 120000
```

### 单文件转换响应

```text
success: bool
source_path: string
output_path: string
status: uint32
step: uint32
read_status: int32
save_result: int32
selector: uint32
elapsed_ms: uint64
error: string
```

### 批量转换

批量转换不要调用 `TBatSaveFolder`，建议外层枚举文件并逐个调用单文件协议。

```text
for each dwg in input_folder:
  if file name does not end with _t3.dwg:
    convert_one(dwg)
```

批量实现建议：

- 串行执行，先保证稳定。
- 每个文件独立设置输出路径。
- 每个文件独立检查 `status/read_status/save_result/output_exists`。
- 遇到失败不要退出整个批次，记录失败项后继续下一张。
- 不要并发向同一个 `acad.exe` 注入多个 direct worker。

## 输出路径规则

推荐调用方自行决定输出路径。

如果需要兼容当前验证行为，可以使用：

```text
<source_dir>\<source_stem>_t3.dwg
```

注意：

- 源路径和输出路径都应传入绝对路径。
- 路径字符串应写入远程进程为 UTF-16LE，以 `NUL` 结尾。
- 调用前应确保输出目录存在。
- 调用前建议删除同名旧输出，避免把旧文件误判成成功。

## 开发者最小实现清单

要让其他语言完整实现“无 UI 转 T3”，至少需要实现下面 9 个能力：

1. 通过 `TGStart.exe` 启动 Tianzheng CAD，或复用已启动的 Tianzheng `acad.exe`。
2. 枚举 `acad.exe` 进程，并确认目标进程加载了 `tch_kernal.arx`。
3. 获取 `tch_kernal.arx` 在目标进程中的实际模块基址。
4. 打开目标进程，具备远程线程、远程内存读写和查询权限。
5. 如果当前布局提供已验证 non-UI 标志地址，则写入两个 non-UI 标志。
6. 分配远程内存并写入控制块、UTF-16LE 源路径、UTF-16LE 输出路径和 x64 stub。
7. 按 Microsoft x64 ABI 调用本文档列出的 6 步函数链。
8. 读取控制块并判断 `status=1`、`read_status=0`、`save_result=5100`、`selector=0x10`。
9. 检查输出文件存在，并向上层返回逐文件结果。

## 错误处理

| 现象 | 建议判断 |
| --- | --- |
| 找不到 `acad.exe` | 宿主未启动 |
| 找不到 `tch_kernal.arx` | Tianzheng 未加载完成或不是 Tianzheng CAD |
| `OpenProcess` 失败 | 权限不足或位数不匹配 |
| `VirtualAllocEx` 失败 | 进程权限或内存不足 |
| 路径写入失败 | 路径过长、权限不足或进程句柄权限不足 |
| 远程线程超时 | 目标进程卡住、DWG 异常或 worker 阻塞 |
| `read_status != 0` | 源 DWG 读取失败 |
| `save_result != 5100` | Tianzheng 保存 worker 失败 |
| `status=1` 但输出不存在 | 文件系统写入失败或输出路径不合法 |

## 不推荐的入口

### 直接调用完整 `TBatSave` handler

`TBatSave` 的正式 handler 已定位，但它包含 UI 选择、文件列表、输出目录和版本状态等上下文构造。直接从外部调用完整 handler 需要复原大量 UI/ctx 前置状态，不是当前稳定协议。

### `TBatSaveFolder`

`TBatSaveFolder` 是独立 handler，不是简单参数别名。它内部会构造自己的 ctx，并从 ctx 字段读取目录、版本和 selector。当前尚未把它整理成稳定的无 UI 原生协议。

### `r13 == 17`

当前已确认在 `TBatSave` 和 `TBatSaveFolder` 两个正式入口里，`r13 == 17` 没有稳定 worker 分发。因此不要把 `17` 当成当前可调用的 T3 转换版本码。

## 与 `TBatSave` 的关系

本文档使用的 direct worker 不是绕过 Tianzheng 转换逻辑的普通 `saveAs`。

它复用了 `TBatSave` T3 分支中的关键原生链路：

```text
sys23x64: sub_1C01B310 -> sub_1C01E850 -> SaveAsTArch3 / sub_1C027310
sys24x64: sub_1C01EB00 -> sub_1C01E200 -> SaveAsTArch3 / 0x0273A0
```

也就是说，输出是通过 Tianzheng 的 T3 专用保存 worker 生成，而不是仅调用 AutoCAD 的普通 DWG 另存。

## 最小验收标准

其他语言实现完成后，至少应满足以下验收条件：

1. 能自动找到已加载 `tch_kernal.arx` 的 Tianzheng `acad.exe`。
2. 能对单个源 DWG 生成 `_t3.dwg`。
3. 控制块返回：

```text
status=1
read_status=0
save_result=5100
selector=0x10
```

4. 输出文件实际存在且更新时间为本次调用时间。
5. 连续转换多个文件时，`acad.exe` 不崩溃、不进入 UI 阻塞。

## 参考测试结果

当前环境中曾对 9 个 DWG 顺序执行该协议，全部走 direct worker 成功：

```text
count=9
success=9
total_wall_seconds≈6.6
avg_wall_seconds≈0.73
save_result=5100
```

该耗时包含调用方进程启动、路径准备、远程线程执行和输出文件落盘检查。其他语言常驻实现如果复用进程句柄和预生成 stub，平均耗时可能更低。
