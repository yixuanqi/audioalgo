# ADR 0040：自定义节点工作区 schema 1、运行文件清单与内容身份

- 状态：**accepted（W0 契约冻结，2026-10-26）**
- 关联：[多文件工作区方案](../plan_node_workspace_implementation.md)、[NWS 逐项验收](../testing_node_workspace_authoring.md)、唯一当前摘要 `doc/development_status.md`
- 影响范围：`tests/fixtures/node_workspace/**`（冻结夹具与独立期望）、`tests/node_workspace_manifest.json`（30 条 NWS 映射）、后续 `libs/audio_node_authoring/Workspace.*` 与生产面板入口
- 不改动：`.aadraft` 格式与既有字节（ADR0036/0037）、Python 包 `manifest.json` schema 1、`.aaproj` schema 1/2、发布节点 SDK C ABI v1

## 1. 背景

NWS-01～10 之前只是设计文本。若先写生产解析器再定字段、清单和 hash 算法，夹具、期望值和实现会互相"对齐"，无法证明实现真的满足独立期望。本 ADR 在写任何产品解析器**之前**冻结以下内容：

1. 作者工作区目录格式（schema 1）与 `node.json` 字段；
2. 规范化 JSON 编码（可复现字节）；
3. 运行内容文件白名单与排除集；
4. 文件清单与整树内容 hash 算法；
5. 路径、预算与创建/转换事务语义；
6. 冻结夹具与**独立**期望（Python 标准库工具，不 import 被测实现）。

W0 只冻结契约与夹具；任何 NWS 子项在 W0 期间都不能记为 `passed`。真实产品行为从 W1 起按 Red → 最小实现 → 同用例 Green 推进。

## 2. 决策：目录格式

每个工作区目录**恰好一个节点**，权威声明只有一份根 `node.json`：

```text
<workspace>/
  node.json          # 唯一可编辑声明（schema 1）
  src/**             # Python 源码树；C++ 源文件
  include/**         # C++ 头文件（可选）
  <resources...>     # 显式资源（可选）
  tests/**           # 作者测试，永不进入运行内容
  .vscode/**  .git/** __pycache__/** build/** ... # 编辑器/构建产物，永不进入运行内容
```

`node.json`（schema 1）字段：

| 字段 | 类型 | 必需 | 约束 |
| --- | --- | --- | --- |
| `workspace_schema` | integer | 是 | 恰为 `1`；其他值 → `workspace_schema_unsupported` |
| `language` | string | 是 | `"python"` 或 `"cpp"`；其他 → `workspace_language_unsupported` |
| `node` | object | 是 | SDK 节点描述（`type_id`/`version`/`display_name`/`inputs`/`outputs`/`format`/`parameters`/`execution` 等原字段），唯一可编辑定义；Check 阶段才做 SDK 校验 |
| `python` | object | python 必需 | `source_root` 必须为 `"src"`；`entrypoint` 形如 `module.path:function` |
| `native` | object | cpp 必需 | `sources`（非空相对路径数组）、`include_dirs`（相对目录数组，可空） |
| `resources` | array | 否 | 显式相对文件路径数组；缺省等价于 `[]` |

政策：

- **未知字段保留**：`node.json` 顶层、`node`、`python`、`native` 中的未知键在读取时原样保留在内存模型中，任何本产品写回（W2 起的表单保存）都不得静默删除。未知键在 W1 不参与解释，也不等于已支持能力。
- **重复键拒绝**：同一对象内重复键 → `workspace_json_duplicate_key`，不做"后者覆盖"。
- 非 UTF-8、BOM、NaN/Infinity、尾随字节 → `workspace_json_invalid`。
- `python`/`native` 段缺失或不匹配 `language` → `workspace_contract_invalid`。
- 每个工作区一个节点；多节点目录不属于 schema 1。

**校验顺序冻结**为：`json`（解析/重复键/BOM/非有限数）→ `schema`（`workspace_schema`、`language`、`node.type_id`、`python`/`native` 段）→ `paths`（相对路径、入口解析、大小写冲突）→ `existence`（声明文件/资源存在）→ `budget`（§7 上限）→ `manifest`（§5 清单与 hash）。同一目录命中多个错误时报告顺序中的第一个，诊断代码即该层代码；`tests/fixtures/node_workspace/invalid/expectations.json` 固定每类负例的期望代码与所属层。

## 3. 决策：规范化 JSON 编码

本产品写出的 JSON（`node.json` 及后续表单保存）必须使用**规范化紧凑编码**，以便字节可复现：

1. 对象键按**源文档出现顺序**（插入顺序）输出，不排序、不去重；
2. 数组保持原顺序；
3. 分隔符只有 `,` 与 `:`，无空格、无换行；
4. 字符串：`"` → `\"`，`\` → `\\`，`\b \f \n \r \t` 用短转义，其余码位 `< 0x20` 用 `\u00xx`（小写十六进制）；`/` 不转义；非 ASCII 以原 UTF-8 字节输出；
5. 数字保留**解析时的原 token 文本**（例如 `0.5`、`-2`、`1e-5` 原样输出，不做浮点重格式化）；
6. `true`/`false`/`null` 按原样输出。

数字 token 保留规则消除了不同标准库浮点格式化差异，使 C++ 与独立 Python 期望能够逐字节比较。

## 4. 决策：运行内容白名单与排除集

`Check`/`Apply` 的运行内容（runtime content）只包含：

1. 根 `node.json`（必须存在）；
2. `src/**` 下所有普通文件（递归）；
3. `include/**` 下所有普通文件（递归）；
4. `resources` 中显式列出的每个文件。

其余一切都不进入运行内容，且**修改它们不得改变运行内容 hash**。冻结排除集（按路径组件判断）：

- 首段或任一路径组件属于：`tests`、`.git`、`.vscode`、`.idea`、`__pycache__`、`build`、`.build`、`staging`、`dist`、`out`、`node_modules`、`.pytest_cache`、`.mypy_cache`；
- 任一路径组件以 `.` 开头（`node.json` 单独显式包含，不受此规则影响）；
- 扩展名为 `.pyc`/`.pyo`。

排除只影响运行内容与 hash；不删除、不移动用户文件。

## 5. 决策：文件清单与整树内容 hash

清单项：`{relative_path, byte_size, sha256}`。

- 相对路径使用 `/` 分隔符、不含前导 `./`、不含空段；
- 排序键：**相对路径的 UTF-8 字节序列**升序（与区域设置无关）；
- 清单文本（UTF-8，`\n` 换行）：

```text
AudioAlgoNodeWorkspace/1
<relative_path>
<byte_size>
<sha256 小写十六进制>
...
```

- `content_sha256` = 上述文本的 SHA-256（小写十六进制）。
- 只有**运行内容字节、相对路径集合**参与身份；时间戳、属性、`tests/**`、编辑器缓存不参与。
- 未实现/未通过 W0 的能力（C++ 构建、wrapper、解释器）另进入"环境身份"，不混入本 hash。

## 6. 决策：路径规则

每个声明的相对路径必须满足：

- 非空、长度 ≤ 512 字节、使用 `/`；
- 非绝对路径：拒绝 `C:`、`/`、`\` 开头、`\\?\`、`\\.\`、UNC；
- 不含 `.`/`..` 段、不含空段、不以 `.` 或空格结尾、不含 `:`（NTFS ADS）；
- Windows 下大小写不敏感唯一：任意两个规范化路径不得仅大小写不同（→ `workspace_path_case_conflict`）；
- 不得指向工作区外：读取与复制前对每一级真实路径检查符号链接/重解析点，命中即 `workspace_path_invalid`；
- 目标文件/目录必须存在于声明位置。

## 7. 决策：预算（与既有包限额核对后锁定）

| 项 | 上限 | 依据 |
| --- | --- | --- |
| `node.json` 字节 | 1 MiB（1,048,576） | 与既有 authoring index 协议上限一致 |
| 运行内容源码文件数（`node.json` + `src/**` + `include/**`） | 128 | 既有 Python 源码 128 文件预算 |
| 运行内容源码总字节 | 8,372,224（8 MiB − 16 KiB） | 既有 `8 MiB` 源码预算，预留 wrapper/manifest 开销 |
| 显式资源文件数 | 64 | W0 新锁定 |
| 显式资源总字节 | 67,108,864（64 MiB） | 与任务载荷 64 MiB 对齐 |
| 单个相对路径 | 512 字节 | W0 新锁定 |
| 工作区根绝对路径 | 32,000 UTF-16 单元 | 扩展路径余量 |

超限必须结构化拒绝（`workspace_budget_exceeded`），禁止静默截断。

## 8. 决策：创建与转换事务

- `create(target, draft)`：目标不存在或为**已存在的空目录**时才创建；非空目标 → `workspace_target_not_empty`，绝不删除/覆盖无关文件。父目录缺失时才创建；已存在的卷根/共享根复用（沿用 ADR0037 的卷根保存修复）。
- 写入内容：`node.json`（规范化编码，键序 `workspace_schema`、`language`、`node`、`python`/`native`、`resources`）、`src/<module>/__init__.py`（0 字节）、`src/<module>/process.py`（= 草稿 `source` 原字节）。`<module>` 由草稿 ID 规范化（小写、`-`→`_`，数字开头加 `node_` 前缀），入口为 `<module>.process:process`（模块路径→文件 `src/<module>/process.py`）。
- 每次写入使用临时文件 + 原子替换，并回读校验字节；失败只清理自己创建的临时文件/空目标目录。
- **失败注入缝（W1 起）**：`WorkspaceStore` 接受 `WorkspacePhase{create,write,verify,replace}` 谓词，测试用它验证失败原子性；注入失败统一返回 `workspace_write_failed`，只回收本次调用自己创建的文件、临时文件与空目录，目标目录既有字节、既有空目录与既有非空目录都不被删除或覆盖。
- `convert(aadraft, target)`：先记录原 `.aadraft` 字节、SHA-256 与存在性；以只读方式解析后按 `create` 生成目录；结束前重读原文件并核对未变，变化 → `workspace_original_changed`。原 `.aadraft` 在任何路径上都**不被写入**、不被转换、不被删除；旧单文件仍可独立打开/检查/Apply。
- `open(directory)`：只读校验 schema 与清单，返回权威声明（`node` 对象规范编码）、入口源文件字节、文件清单与 `content_sha256`；不写任何文件。缺失/损坏/越界 → 结构化诊断，不创建默认节点、不回退到其他版本。

## 9. 冻结夹具与独立期望

| 路径 | 内容 |
| --- | --- |
| `tests/fixtures/node_workspace/python_two_files/` | `node.json`（schema 1，`process.py` 调用 `filter.py`，双命名输入）、`src/print_a/{__init__,process,filter}.py`、`tests/test_process.py`、`.vscode/settings.json`、`build/cache.bin` |
| `tests/fixtures/node_workspace/python_two_files_v2/` | 仅辅助文件系数改变（0.25 → 0.5），`node.json` 不变 |
| `tests/fixtures/node_workspace/native_two_sources/` | `node.cpp` 调用 `filter.cpp` 与 `include/my_filter/filter.h`，含 `tests/filter_test.cpp` |
| `tests/fixtures/node_workspace/legacy/print_a.aadraft` | 旧单文件夹具，公式与 `python_two_files` 相同 |
| `tests/fixtures/node_workspace/invalid/` | schema/路径/入口/大小写负例，含期望诊断代码 |
| `tests/fixtures/node_workspace/tools/workspace_expectation.py` | **独立**标准库工具：自行解析 JSON/`.aadraft`、计算规范化编码、清单与 hash；不 import 被测实现 |
| `tests/fixtures/node_workspace/expected/workspace_lock.json` | 由上述工具生成的冻结期望：逐文件字节数/SHA-256、`content_sha256`、规范化声明、入口源字节 hash、实际被排除但仍存在的文件 |

`tests/node_workspace_manifest.json` 把 NWS-01.a～NWS-10.c 共 30 条逐项绑定到阶段、层次、测试名、旧 NA/PC 映射和独立产物；W0 只能登记 `not_run`。`tests/infrastructure/test_node_workspace_manifest.py` 独立重算夹具 hash 与清单，夹具/期望被改动而未更新 ADR 与本 ADR 的版本标记时失败。

数值期望与实现分离：Python 节点 `y = 0.5*a - 0.25*b`（v2 为 `0.5*a - 0.5*b`），C++ 节点 `y = -2*a + 0.125*b + step*n`；独立 float64 oracle 不放 import 路径，不 import 被测模块或生成 wrapper。

## 10. 明确不属于本 ADR 的能力

- Python 多文件真实执行、Ready/Apply 冻结快照、runner 导入与诊断（W2）；
- 面板声明表单、外部编辑器协议、冲突比较（W3）；
- C++ 独立构建/probe/PendingRestart（W4，依赖 PC-07 基础闭环）；
- 工程携带、正式 Release 与真实桌面通过（W5，桌面暂停时记 `not_verified`）；
- 旧 `.aadraft` 自动转格式、多节点目录、目录级自定义 `CMakeLists.txt`、任意 pip 依赖安装、热插拔。

## 11. 后果

- 正向：schema、字节编码、清单和 hash 在写解析器之前就有独立期望，W1 起可用同一夹具做跨实现比对；未知字段与排除集明确，避免后续静默删字段或把 `tests/**` 计入运行身份。
- 代价：规范化编码要求保留数字 token，需要在 C++/Python 两侧都避免"重新格式化浮点"；今后修改任一冻结项必须同时更新 ADR 版本标记、夹具、独立期望与 NWS 映射。
- 约束：本 ADR 冻结的是**契约**。NWS 子项通过仍以 `tests/node_workspace_manifest.json` 中绑定测试的真实执行结果为准；W0 期间没有任何 NWS 子项可记为 `passed`。
