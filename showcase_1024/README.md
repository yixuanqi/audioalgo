# showcase_1024 — TRAE 1024 作品秀发布材料

本文件夹是发布专用材料，不属于产品代码，不触碰任何产品文件；公开仓库时整个文件夹可排除。

## 材料清单

| 文件 | 用途 |
|---|---|
| `post.md` | 帖子正文终稿（替换 4 处【占位】即可发） |
| `recording_script.md` | 录屏/截图分步操作卡（含解说词与画面重点） |
| `checklist.md` | 发布前检查清单（真实性/安全/操作） |
| `make_mixer_demo.py` | 生成双源 Mixer 演示工程（确定性输出，纯标准库） |
| `verify_mixer_demo.py` | Mixer 运行独立 oracle（逐样本，纯标准库） |
| `run_cli_demo.ps1` | 一键 CLI 演示：Gain+Mixer 各跑一次并双 oracle 复核 |
| `screenshots/` | **已拍好的正式截图**（3 张 + 1 张 Gain 加片）与 CLI 运行产物 |
| `automation/` | 截图自动化脚本（启动编辑器/点击/截屏）、原始 gain_demo 备份 |
| `mixer_demo/` | 生成的演示工程（.aaproj + 两个 48kHz 立体声 PCM16 WAV） |
| `runs/` | CLI 演示运行产物（每次运行时间戳目录，含 run.json/complete.json/output.wav/独立验证报告） |

## 使用顺序

1. `python showcase_1024\make_mixer_demo.py`（如 `mixer_demo/` 不存在；**当前已生成并验证**）
2. `powershell -ExecutionPolicy Bypass -File showcase_1024\run_cli_demo.ps1` —— 必须看到 `== ALL DEMO VERIFICATIONS PASSED ==`（**2026-10-22 已实测通过**：Gain 误差 1.48e-08、Mixer 误差 1.48e-08，`runs/` 里留有本次运行产物与独立验证报告）
3. 按 `recording_script.md` 拍录屏与截图
4. 按 `checklist.md` 逐项检查后，粘贴 `post.md` 发布

## 技术备注

- 相对资源路径按工程文件所在目录解析（`ProjectResourceService::resolve`），所以 `mixer_demo/` 可整体移动，WAV 引用不会失效
- `verify_mixer_demo.py` 的公式与容差（`2e-6 + 1e-5*|expected|`）与仓库 `scripts/verify_run_audio.py` 同源；Mixer 公式 `10^(db/20)` 取自 `libs/audio_algorithms/Mixer.cpp`
- 双源 WAV 格式/帧数刻意完全一致：运行服务对成对源做格式/帧数/通道预校验（T25 契约），不一致会被拒绝——这不是 bug，是设计
- 帖中数字溯源：242/242 与 oracle 误差见 `doc/development_status.md` 顶部 M21/P0 摘要
