# ADR0042 — 多文件 Python 使用冻结 runner 的函数适配器

日期：2026-10-27。状态：accepted；以 NWS0005 的 Red/Green 验证，正式阶段出口仍未通过。

ADR0040 的源码文件上限128包含 node.json。原实现为每次目录 Check 生成 authoring_wrapper.py 和 syntax_check.py，即使作者源码合法，内部封装也会越过现有 Python 包的128文件上限。不得增加运行器上限或减少作者配额来绕过这个问题。

目录封装的 manifest.json 代替 node.json，源码仍逐文件复制到 workspace_src/。函数适配器移动到受冻结 runner hash 保护的 audioalgo_runner.workspace_function，目录包不再产生适配器源码文件。包 manifest 保持 schema 1，增加可选 authoring_profile={kind:"workspace-function-v1",syntax_only:bool}，节点 entrypoint 使用 workspace_src.<module>:<function>。普通 Python 插件和已有单文件封装继续使用原类入口；没有 profile 时不改变加载规则。未知或损坏 profile 拒绝，不推断适配。

语法检查使用相同源码清单的独立语法包，syntax_only=true 时仅编译所有 .py，绝不导入用户模块；诊断保留 src/相对文件、行、Unicode字符列。正常包在 prepare 时导入函数，保持 initialize/process/reset 的原协议和每实例 state。运行器自身、包 manifest、全部源码仍分别冻结、核 hash，解释器/runner 环境继续绑定 Ready。

源码128文件、8MiB运行包预算，协议/日志/载荷上限均不变。此片仅解决生成文件挤占源码配额；资源与源码字节极限仍需独立验收，不能据此将 NWS-09.a 或 W2 标为 passed。冻结夹具与独立期望不变。
