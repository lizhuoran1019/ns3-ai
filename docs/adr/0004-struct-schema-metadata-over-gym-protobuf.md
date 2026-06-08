# 以共享内存结构层 Schema 元数据替代 Gym protobuf 兼容性校验

Gym 初始化握手兼容性校验（Slice 3 of PRD #52）原计划在 protobuf 消息层通过 `SimInitMsg`/`SimInitAck` 新增协议版本字段和 `min_compatible` 范围协商实现。实际采用的方式是在共享内存头部（`Ns3AiMsgProtocolHeader`）承载 struct 级别的 schema hash/version，由通用 `ValidateProtocolHeader()` 做等值校验，不涉及 Gym 协议层的修改。

## 决定的理由

- **拦截时机更早**：校验发生在任何 protobuf 消息交换之前的共享内存会话握手阶段，不兼容的两端在进入 Gym 协议之前就失败。
- **覆盖更可靠**：`NS3_AI_GYM_MSG_SCHEMA_HASH` 覆盖 `Ns3AiGymMsg` 的完整二进制布局（buffer size + 字段偏移），比 protobuf 字段级别的校验更能检测 ABI 不兼容。具体的 protobuf schema 差异会在 protobuf runtime 层以解析错误的形式暴露，不会静默损坏数据。
- **不需要范围兼容性**：Gym 协议目前不存在 v1↔v2 前向兼容的需求。唯一需要检测的场景是两端使用了不同版本的代码，此时 struct hash 等值比对恰好是最直接的检测方式。
- **模板化默认可复用**：`Ns3AiMsgTypeSchemaDefaults<PayloadType>` 使任何消息接口都能自带 schema 元数据，不限于 Gym。

## 未采用的方式

- **protobuf 层兼容性校验**（原计划）：为 `SimInitMsg`/`SimInitAck` 新增 `gym_protocol_version`、`min_compatible_gym_protocol_version`、`proto_schema_hash` 等字段，在 `OpenGymInterface::Init()` 和 `Ns3Env.initialize_env()` 中分别做双向校验。该方案引入 Gym 协议版本范围协商语义，增加了 protobuf wire 格式复杂度，且校验时机在 struct 层校验之后，是冗余的。
- **`ns3-ai-gym-protocol.h` 专用头文件**：原计划新建头文件定义协议常量。实际将常量直接定义在 `ns3-ai-gym-msg.h`（`Ns3AiGymMsg` 所在位置），使 hash 紧邻被校验的结构定义，减少维护者遗忘更新的概率。
