# 原生 Node SDK v1

当前支持 Windows x64、C ABI v1、float32 planar、同采样率/通道的 N 入 N 出 PCM transform。内置节点保持原有注册。用户 DLL 无需宿主 import library、JUCE、源码树或算法库；普通 C/C++ 编译工具链与 Windows/MSVC 运行库是声明依赖。v1 不承诺任意工具链兼容、原生崩溃隔离、热替换、热卸载或跨进程恢复。

## 安装与独立构建

开发者从已配置的构建导出 SDK（此操作不编译宿主）：

```powershell
cmake --install build/windows-debug --config Debug --component AudioAlgoNodeSDK --prefix C:/AudioAlgoSDK
```

用户只需要拿到该安装目录，在**仓库外**复制模板后构建。`C:/AudioAlgoSDK`、`C:/MyAudioNode` 与插件目录是示例安装位置，可替换为自己的可写目录：

```powershell
Copy-Item C:/AudioAlgoSDK/share/AudioAlgoNodeSDK/native_stateful C:/MyAudioNode -Recurse
cmake -S C:/MyAudioNode -B C:/MyAudioNode-build -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH=C:/AudioAlgoSDK
cmake --build C:/MyAudioNode-build --config Release
cmake --install C:/MyAudioNode-build --config Release --prefix C:/AudioAlgoPlugins/stateful
```

安装内容是 `include/audioalgo/node_abi_v1.h`、`lib/cmake/AudioAlgoNodeSDK/` CMake 包、`share/AudioAlgoNodeSDK/native_stateful/` 模板。导出的 `AudioAlgo::node_abi` 是纯头文件 target。模板只依赖它与标准库。

开发中的可选日志契约另附 `node_log_abi_v1.h` 与 `log_queue_v1.hpp`，不改旧 v1 头文件或入口。当前已验证 SDK 独立消费者、实例队列、宿主可选表协商，以及实际混合 Run/CLI 的 sink 绑定、worker 消费、原始日志归档和运行日志 UI：缺入口继续使用旧 v1，错误大小/版本/缺函数/协商拒绝在注册前失败。每实例在 worker 绑定身份；process/reset 只入预分配队列，准备/处理与最终 reset 完成后由 worker 消费，detach 后再释放实例及模块。实际中文/emoji、四级日志、NUL 及所测控制字符已由 GUI/CLI/历史/Copy 验证；原生任务单／双实例合计16MiB恰好与超1字节、持续有限洪泛、失败不发布及数值恢复已通过 worker/CLI 核验；实际队满、非法 UTF-8、超长和错误结构消息的 SDK 状态、生产拒绝诊断与恢复已有验证；实际混合 Run 的两个原生区域各16MiB、仅后区域超限及恢复已有验证；两个独立并行 RunService 请求各16MiB及单请求超限隔离已有验证；worker已保存每流64KiB完整UTF8尾部及独立原始/队满/尾部计数；GUI洪泛和任意控制序列仍待验收。旧 DLL 与新 SDK 的节点库/Inspector 能力文字和 CLI 目录已核对；真实桌面展示、完整取消/关闭/失败生命周期仍待验收。正式 SDK 发布包及完整宿主接入另行验收，现有历史发布 zip 保留原字节。

包目录包含 `manifest.json`、`external_stateful.dll` 及 DLL 自身依赖。依赖 DLL 应放同一目录。manifest 的 `library` 必须是包内相对路径；manifest 与 DLL 内嵌 JSON 必须相同。修改插件 ID、版本、节点描述、端口或参数时同步更改 `manifest.json.in`，构建自动把同一 manifest 嵌入 DLL。

## 发现和使用

GUI/CLI 共用 `register_host_nodes` 和 `load_native_plugins`。设置 `AUDIOALGO_PLUGIN_PATH` 后**重新启动**应用。Windows 下多个根用分号分开；有该变量时替代默认根。未配置时默认扫描 EXE 旁的 `plugins/` 和 `%APPDATA%/AudioAlgo/plugins/`。每个根支持根目录本身的 manifest 和下一层各包目录，完整路径排序去重，重复 ID 的后一个包拒绝。无效包产生带 manifest 路径的诊断，其余内置节点仍可运行。

```powershell
$env:AUDIOALGO_PLUGIN_PATH='C:/AudioAlgoPlugins'
& 'C:/AudioAlgo/AudioAlgo Editor.exe'
& 'C:/AudioAlgo/audio_cli.exe' --list-nodes
& 'C:/AudioAlgo/audio_cli.exe' run C:/Projects/plugin.aaproj C:/Runs/new-run 127
```

CLI 也可重复传 `--plugin-dir <root>`，显式目录替代环境/default。`--list-nodes` 输出 JSON 目录和诊断，存在坏包时退出1；`run` 输出诊断但仍可运行不使用坏包的内置图。GUI 在启动日志报告坏包。节点库、端口与参数表单来自同一描述；添加 External Stateful Blend 后接 `primary`、`reference`，可选择 `main` 或 `side` 输出。

示例的独立公式为 `main[n] = revision * gain * primary[n] + reference[n] + bias * n`，`side[n] = primary[n] - reference[n]`，n 每实例从0累计，prepare/reset 后归零。参数、格式、状态均属于实例，不能用共享全局变量存放。示例是可替换的真实算法教学实现，不是降噪器。

## ABI 与描述契约

导出 `audioalgo_node_api_v1`，使用头文件规定的调用约定、默认结构对齐及 `uint32_t` 大小/版本字段。函数表包含 create、prepare、process、reset、destroy。create 在 DLL 中分配 opaque instance，destroy 必须在同一 DLL 释放。所有输入字符串含明确字节长度；API/manifest 存储由模块持有，其余字符串/参数/缓冲区/错误存储属于调用方且仅当前调用有效。错误为有界 code/message，不能跨边界传 STL、JUCE 对象或异常。

prepare 可分配，process/reset 不分配、不访问文件、不能保存传入 view。单实例调用串行，多实例可并发。宿主每个节点/运行创建独立实例，参数以验证过的 JSON 快照传入 prepare，进程关闭先回收工作与实例。工厂与实例各自持有模块引用，实例析构先 reset/destroy，再释放最后模块引用；不依赖发现结果或 catalog 对象的存活期。

可选日志的独立入口为 `audioalgo_node_log_api_v1`，大小/版本必须完全匹配；缺入口表示日志不可用，不能拒绝合法旧 v1 计算。消息按显式 UTF-8 字节数借用到 emit 返回，单条最多 1024 字节；允许空消息和内嵌 NUL，消费者须尊重长度。`audioalgo::sdk::LogQueueV1` 在构造时拥有 64 个固定槽，回调只校验/复制/更新无锁计数，由 worker 在 process/reset 之外 pop。满队列、超长、非法 UTF-8 和关闭均返回具体状态，不静默裁剪；满队列有效尝试仍消耗序号，计数/字节数保留缺口。每实例一个生产者/消费者，prepare/process/reset/set_sink 串行，不允许额外后台生产线程。先停止调用、worker reset/drain、detach，再销毁实例和模块；close 不会抢占进行中的 emit 或任意原生代码。此队列契约不替代宿主完整日志预算、异常隔离或生命周期验收。

manifest schema1 的必需顶层字段：`plugin_id`、`version`、`abi_version:1`、`platform:"windows"`、`architecture:"x64"`、`library`、非空 `nodes`。每节点包含 type_id/version/display_name、ports、parameters、sample_rates_hz、channel_counts。port 含 id/direction/type_id/required；parameter 含 id/kind/default，可用 minimum/maximum/step/unit/display_name/help/choices/input_resource。kind 支持 bool/int/float/enum/string/path，约束与内置 SDK 同源验证。

当前只准入 native_block 和 audio.pcm@1。可声明 variable block 或 fixed_block_frames；固定帧默认拒绝不完整尾帧，显式声明 `fixed_frame_end_policy:"zero_pad"` 后由通用 FIFO 适配 host 块长。静态已知算法延迟可用 `latency_frames` 声明（默认0），与F-1样本缓冲延迟相加；JUCE合流补偿、区域末端只裁剪一次。run.json记录raw/aligned长度及offset，非零延迟另有实际output.raw.wav。v1 process必须对所有端口完整消费/产出算法帧，部分消费/产出以`plugin_partial_block_unsupported`拒绝。需要部分消费、动态延迟或算法尾部协议的插件等待后续契约，不得声明后静默忽略。运行报告`plugins`逐节点记录插件/节点版本、DLL与manifest SHA-256；旧运行记录不会因新DLL更新而重写。

## 更新

关闭编辑器和 CLI，等任务回收完成；仅重新构建插件并安装到同一包目录，再启动现有宿主。示例 `-DPLUGIN_REVISION=2` 会更新行为及版本1.0.2，可用于验证流程。替换仍在进程中加载的 DLL 不受支持。无需重编、重链接或修改 EXE。

## 可重跑验收与限制

`ctest --test-dir build/windows-debug -C Debug -R '^UPN_external_sdk_pipeline$' --output-on-failure` 从安装 SDK 模板在系统临时目录构建，复制现有宿主，编译 C 头文件，实际组件调参/连线、CLI PCM 数值、负例、并发实例、catalog 回收后实例、插件更新及 EXE hash 均共享一份真实工程。命令、日志、JUnit、精确源码绑定由 `scripts/record_command.py` 采集，内部证据在 `build/reports/loops/P1-native-sdk/<UTC>/report.json`。

真实桌面继续暂停；组件证据不能替代桌面/设备验收。HTTP 代理关闭与无网络依赖的独立构建不等同操作系统防火墙隔离，正式 UPN-06 禁网/干净安装环境仍须额外取得证据。完整 Release 与所有原累计验收尚未完成。

原生日志的原始文件旁有同名 `.summary.json`：`tail_utf8_base64` 保存最多64KiB的完整UTF-8后缀（允许NUL），`tail_discarded_bytes` 只计尾部淘汰，原始文件不因此截断。`persisted_bytes` 与 `accepted_bytes` 分开；`raw_complete` 只表示SDK接受的字节已全部持久化，`all_messages_accepted` 另表SDK是否拒绝过消息。两者和 `failure_code` 应共同读取，不能把队满/非法/超限失败记作完整成功。

SDK sink 上下文在串行 `set_sink(instance, NULL)` 返回前仍有效。若插件在该调用中同步发出有效日志，宿主会在 detach 返回后再次由 worker 消费，再关闭队列和写捕获摘要，避免已接受的最后消息遗漏。此保证不允许额外后台生产线程，也不等于完整取消/关闭/崩溃隔离验收。
