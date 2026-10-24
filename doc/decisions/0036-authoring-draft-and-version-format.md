# 0036 — 工作台草稿与精确工程版本的持久化

日期：2026-10-23；依据：工作台方案§7/8、NA-01/09/11。实施中的格式契约，不表示NA全通过。

草稿存成一个`drafts/<id>.aadraft`文件：`AudioAlgoDraft/1\n`，随后ID、十进制generation、language、原始JSON声明和原始源码各自以`UTF-8字节长度\n`封装。一次原子替换保存正文和声明，避免多文件撕裂；纯服务不需要JUCE/JSON解析器。允许不完整声明和源码，不把草稿保存等同于检查。独立读取器另行按字节解析并比对测试输入，不调用产品编解码。后续导出包仍使用标准manifest和源码。

工程schema2在每个受管NodeInstance增加`managed_version`，字段为`package_id`、64位小写十六进制`content_sha256`、`package_path`、`wrapper_version`。路径是相对工程的携带包目录；同type或同package的全部实例必须锁同一内容hash。type_id仍为逻辑ID，不能加hash伪装不同类型。未受管schema1继续原语义；schema1含受管锁必须拒绝，避免旧读取器忽略锁后执行错误代码。schema2缺包仍可加载/保留/另存，执行必须明确拒绝，不能回退latest。

含锁的schema2读写保持未知扩展字段；未知schema继续明确unsupported。旧schema1 CLI在NA0005保存了当前实际EXE及hash，用作独立旧宿主拒绝schema2证据。第一次覆盖已保存schema1前，备份原字节到`.schema-1.bak`、校验备份、验证临时schema2后才替换；已有不同内容备份不覆盖并拒绝迁移。新未保存工程直接schema2不造假备份。GUI提示/用户明确格式升级确认属于保存入口后续接入，低层事务本身不能替代提示证据。

本契约新增格式，不改旧fixture/数值容差，也不改变当前真实桌面暂停。NA-11和受影响AR-13/PRJ用真实Red/Green及独立字节读取验收。
