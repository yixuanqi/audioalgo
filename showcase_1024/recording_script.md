# 录屏与截图操作卡

> 目标：一次性拍齐帖子所需全部素材，预计 10 分钟。
> 原则：只演示已验证功能；所有数字（SHA、误差）都是屏幕上真实出现的。

## 已由自动化完成（2026-10-22，见 `screenshots/`）

- [x] **截图1** `screenshot1_mixer_overview.png` —— 双源 Mixer 图 + 渲染波形 + 状态栏
- [x] **截图2** `screenshot2_mixer_parameters.png` —— Mixer 参数面板特写
- [x] **截图3** `screenshot3_status_sha.png` —— 状态栏 `48000 frames · aee80dff539f…`
- [x] **加片** `stage2_gain_run_full.png` —— Gain 演示全景（波形 + SHA `99c4d00f…`）
- 关键证据：编辑器 GUI 运行的输出 SHA 与 CLI 运行**完全一致**（aee80dff539f…），确定性渲染跨引擎成立

剩余人工项只有录屏 A/B/C。

## 0. 开拍前检查（2 分钟）

- [ ] 关闭所有正在运行的 AudioAlgo Editor（任务管理器搜 "AudioAlgo"，单实例约束）
- [ ] 关闭系统通知（专注助手/勿扰）
- [ ] 屏幕分辨率 1920×1080、缩放 100%~125%，录屏区域取编辑器窗口即可
- [ ] 录屏工具就绪（Win+G / OBS / 手机横拍均可）
- [ ] 已生成演示工程：`showcase_1024\mixer_demo\mixer_demo.aaproj`（没有就先跑 `python showcase_1024\make_mixer_demo.py`）
- [ ] 已跑通一次 `showcase_1024\run_cli_demo.ps1`，确认两个 oracle 都是 PASSED

## 1. 录屏 A：编辑器完整闭环（约 3 分钟）

启动：双击 `build\windows-debug\bin\AudioAlgo Editor.exe`

| 步骤 | 操作 | 解说词参考 | 画面重点 |
|---|---|---|---|
| A1 | 新建工程（New） | "从一个空工程开始" | 空画布 |
| A2 | 从节点库拖入 File、Gain、Output | "节点库拖进来，连成 File→Gain→Output" | 拖放瞬间、连线变色 |
| A3 | 选中 Gain，滑块 0 dB → +6 dB | "滑块调参，非法值会被表单拒绝" | 参数面板特写 |
| A4 | 点 Run，等波形出现 | "状态栏有 run id、帧数和 SHA-256" | 波形 + 状态栏 SHA 特写 |
| A5 | 点 Play，放 3~5 秒 | "直接用声卡试听渲染结果" | 按钮变 Pause |
| A6 | Ctrl+Z / Ctrl+Y 演示一次 | "拓扑和参数都有历史" | 节点恢复 |
| A7 | 保存工程 | "保存为 .aaproj，纯 JSON" | 保存对话框 |

## 2. 录屏 B：双源 Mixer（约 2 分钟）

| 步骤 | 操作 | 解说词参考 | 画面重点 |
|---|---|---|---|
| B1 | Open `showcase_1024\mixer_demo\mixer_demo.aaproj` | "两个独立文件源，各自解码、快照" | 分叉汇合的图全貌（截图1 同款） |
| B2 | 选中 Mixer：primary 0 dB、secondary −6 dB | "主/副通道独立增益" | 参数面板特写（截图2 同款） |
| B3 | 把 secondary 从 −6 拉到 0，点 Run | "波形肉眼可见变响——两路能量叠加" | 波形振幅变化 |
| B4 | 状态栏特写停 2 秒 | "确定性 SHA，同工程重跑逐字节一致" | run id / frames / SHA（截图3 同款） |

## 3. 录屏 C：CLI + 独立 oracle（约 2 分钟）

终端里执行（脚本已封好）：

```powershell
powershell -ExecutionPolicy Bypass -File showcase_1024\run_cli_demo.ps1
```

| 步骤 | 画面重点 |
|---|---|
| C1 | CLI 打印 `run_id= / output= / frames= / output_sha256=` |
| C2 | Gain oracle 报告 `"status": "passed"`、`max_abs_error` |
| C3 | Mixer oracle 报告 `"status": "passed"`、`max_abs_error`（1e-08 量级） |
| C4 | 结尾一行 `== ALL DEMO VERIFICATIONS PASSED ==` |

加分点：`run_cli_demo.ps1` 跑两次，Gain 的 `output_sha256` 两次都是 `99c4d00fdb3811b3…`——可以现场口播"确定性渲染：同一工程重跑，逐字节一致"。

解说词参考："编辑器和 CLI 共用同一运行引擎；再用一个独立写的 Python oracle 逐样本比对，误差在 1e-8 量级——不是听起来对了，是数值上对了。"

## 4. 静态截图清单（从录屏里截或单独拍）

| 帖子占位 | 内容 | 来自 |
|---|---|---|
| 截图1 | 双源 Mixer 图 + 波形 | B1/B3 画面 |
| 截图2 | Mixer 参数面板 | B2 |
| 截图3 | 状态栏 run id / frames / SHA | A4 或 B4 |

## 5. 拍完自检

- [ ] 录屏里能看到完整 SHA-256 字符串（至少一次清晰停留）
- [ ] 波形有肉眼可见变化（调参前后）
- [ ] oracle 输出 `"status": "passed"` 清晰可读
- [ ] 没有拍到内部路径/公司信息/其他无关窗口
