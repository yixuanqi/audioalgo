# 自定义节点文件与目录

2026-10-24，适用于 NA0024～30 的新版源码。旧的已运行窗口不会随编译自动更新；正常启动入口见 [一键编译并运行](build_and_run.md)。

## 保存和打开

新建自定义节点后，首次点击 **Save draft** 或 Ctrl+S 选择目录及文件名。`.aadraft` 包含源码、节点声明、身份和编辑版本，允许保存尚未通过检查的内容。保存成功后显示实际路径；后续保存写回该文件。

**Save as...** 或 Ctrl+Shift+S 可以换位置和名称，保留节点身份。取消不改变原文件绑定或内容；已有目标必须明确确认覆盖，确认后再次发生外部修改则拒绝覆盖。遇到外部修改，请保留两份文件，用新的文件名另存为后再比较。**Open draft** 可以重新打开选定文件；**Recover draft** 保留原恢复目录中的草稿入口。源码单独导出为 `.py` 尚未实现，不能用它替代完整草稿。

**Save & close** 必须完成用户文件保存后才关闭。选择取消或写入失败会保留面板；未保存时可用 **Discard changes** 明确放弃关闭。关闭不会自动 Apply；Apply 与保存是独立操作。

2026-10-26 修复：已存在的卷根（例如 `D:\`）在保存时直接复用，不再请求“创建卷根目录”。这只修复了误报的 `create_directories` 拒绝访问；若目标卷本身没有文件写入权限，仍会明确失败，可选择有写权限的子目录。当前构建已通过自动化，真实桌面选择器及该卷实际写入仍待验收。多文件节点工程/外部 IDE 的选择见[开发方式评估提案](plan_node_development_workspace.md)，不属于当前 `.aadraft` 能力。

## Python 多文件节点工程（2026-10-27 开发子集）

1. 在 **New custom node** 填写 Python 声明和入口源码，然后点击 **Node project folder...** 选择空目录。也可先用 **Open draft** 打开旧 .aadraft，再复制转换；原文件不改写。目录内可选 `.audioalgo-authoring.json` 只记录原作者 ID，以便转换已应用节点后仍沿用原受管包；它不是源码或第二份节点声明，请保留。
2. 成功后直接显示 **Python node project**、工程路径和文件列表。**Choose editor...** 可选择编辑器 .exe，随后 **Open code project** 打开目录；没有选择时使用系统打开文件夹。VS Code 可选，设置当前仅在本次宿主会话内保留。
3. 在编辑器中修改 `src/` 的入口和辅助模块。模块内用相对导入，例如 `from .filter import mix`。AudioAlgo 的预览只读，磁盘是唯一源码来源；每 750ms 后台核对，也可点 **Refresh disk**。运行内容变化显示 **Disk changed** 并撤销旧 Ready。
4. 点击 **Check / trial run**，稳定冻结整个源码树后静态检查全部 .py，再在独立 runner 契约试跑。语法错误显示相对文件、1-based 行/列（Unicode 字符），点击诊断可在预览定位，并交给选定编辑器；VS Code 支持行列，通用编辑器至少收到文件路径。**Definition** 和 **Logs** 可按需展开。
5. Ready 后 **Apply** 安装本机不可变版本并刷新节点库。关闭工程面板，从库拖入画布、连接输入和 Output、Run。Apply 新版本不会升级已有图；保存过的图继续锁定原精确包，升级走原 **Upgrade project** 流程。

终端可调用同一检查服务：`audio_cli check-node <node-project-folder> <work-directory>`。输出 Ready 的 workspace/content/package 身份，失败返回结构化 code/file/line/column，失败退出非零；不自动 Apply。

Python 目录也冻结 src/ 下的数据文件与 node.json 的 resources 显式资源列表。源码旁文件可由 Path(__file__).parent 定位，根资源可由入口文件所在位置向上定位工程根；资源随不可变版本安装，运行不依赖作者目录。资源路径与内部 manifest.json/snapshot.json 或映射后的 workspace_src 路径冲突会拒绝。

当前限制：极长清单/大声明与极限源码组合、完整冲突/状态/故障矩阵及编辑器真实启动仍待验收。声明表单当前只读，可从外部编辑 node.json。C++ 目录可只读重开，但工作台多文件独立构建/probe/待重启尚未交付。W1/W2/W3 正式出口未通过；组件预览和 CLI 数值不替代暂停中的真实桌面/DPI。

## 恢复备份

恢复备份继续自动防抖写入，状态 **Recovery backed up; file unsaved** 表示有恢复副本、用户文件仍未保存。自动恢复不会覆盖用户文件，也不会清除未保存标记。恢复时显示副本、原文件路径和更新时间；原文件被外部改动时仍按冲突规则保护。

恢复文本及原文件绑定分别原子写入。若中断导致绑定与文本版本不一致，保留恢复文本、显示警告并解除目标绑定，让用户重新选位置，不能猜测原文件位置并覆盖。

## 三种内部目录

| 按钮 | 用途与生效时间 |
| --- | --- |
| Work folder... | 检查和生成的工作文件。验证可写后本次立即使用，需重新检查；不会搬动已应用版本。 |
| Recovery folder... | 自动恢复副本。验证后本次立即使用，旧恢复文件保留。 |
| Library folder... | 将全部已安装不可变版本和索引复制到用户选定的空目录，核验后保存下次启动设置；旧库保留。 |

当前路径可查看按钮提示。节点库复制仅在检查、应用、运行空闲时允许；复制后当前库继续供现有工程使用，暂时禁用新的 Apply，避免旧库再增加未复制版本。保存文件并正常重启后使用新库。设置保存失败会明确提示，当前库继续有效。

工作/恢复目录的设置不可持久化时，面板显示 **Session only; settings not saved**，仅本次会话生效。设置文件默认在 `%APPDATA%/AudioAlgo/authoring-locations.json`；损坏设置会显示诊断，不覆盖原字节。环境变量 `AUDIOALGO_AUTHORING_ROOT`、`AUDIOALGO_AUTHORING_WORK_ROOT`、`AUDIOALGO_AUTHORING_RECOVERY_ROOT` 优先于设置及默认目录，生效时界面提示覆盖来源，不伪报已切换。

## 当前验证边界

生产组件与实际文件、隔离检查、两版本复制库运行、Python OFF 和纯库自动化已验证。真实桌面的原生选择器、Release 新进程与 DPI 仍待手工验收，未记通过。工程本身“另存为携带输入音频及精确节点包”仍是独立的后续任务。
