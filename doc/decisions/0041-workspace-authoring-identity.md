# ADR0041 — 目录工程保留单文件作者身份

日期：2026-10-27。状态：accepted；NWS0004 已实现并有 Red/Green，正式阶段仍未通过。

## 问题与依据

NWS0004 的真实 Check/Apply 用例先应用 .aadraft，再创建并重开其目录。此前目录会话以 type_id 派生新 ID，导致 package_id 与原节点不同，Apply 将原可用节点当作另一个包的同类型冲突。原 .aadraft ID 不能从规范化 Python 模块名无损恢复，也不能自动接管所有同 type_id 包。转换应保留原作者身份，同时继续保护旧精确包。

## 决策

应用创建/转换的目录增加可选 `.audioalgo-authoring.json`，仅含 `schema_version: 1` 和 `draft_id`，不含节点声明或源码。这是作者身份记录，不是第二份草稿。node.json 仍是唯一权威声明，源码仍只在 src/；原 .aadraft 不改写。记录只在创建事务写入，使用同一失败回收；读取上限16 KiB，draft_id 沿用现有 ASCII/128字符约束，损坏或链接拒绝。

已有目录（包括 ADR0040 冻结夹具）没有该文件时保留原 type_id 派生身份，不自动写文件。重开有记录的目录时沿用原 draft_id，因此已应用单文件转目录可以在原 managed package 中生成新不可变版本。相同 type_id 的无关目录仍按原注册冲突拒绝，不全局采用后加载覆盖。

该隐藏身份文件按 ADR0040 既有点文件排除规则不属于运行内容，不改变冻结 node.json、runtime_files 或 AudioAlgoNodeWorkspace/1 的 hash；冻结夹具、oracle 和期望不变。ID 属于检查身份而非源码 hash：整树快照另复制并核对这份小记录，Check 前后或 UI 磁盘刷新发现作者身份变化也要撤销旧请求/Ready。发布包的 plugin_id 已绑定 draft_id，冻结 runtime manifest 将其纳入包内容身份；运行不读取作者记录或目录。

此补充只解决创建/转换身份，W0 原路径/源码/预算契约和 NWS/NA/PC 全部验收继续有效。不表示 W1/W2 通过。
