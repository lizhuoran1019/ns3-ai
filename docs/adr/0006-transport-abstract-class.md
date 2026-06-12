# 传输层采用 C++ 虚基类 + 工厂模式，而非模板-only 多态

ns-3 代码通常倾向编译期模板多态；但 ns3-ai 的传输层选择运行期虚基类 + `TransportFactory::Create(TransportConfig)` 作为抽象方式。目的是让 Gym、VecEnv、Python binding 在运行期无感知地切换 `MailboxTransport`、`QueueTransport`、`BatchTransport`，而不需要在模板实例化点做编译期选择。

考虑过的替代方案：
- **模板-only**：单条 lane 内部 `QueueTransport<EnvStruct, ActStruct>` 确实能避免虚函数开销；但上层 `MultiLaneTransport` 和 `VecEnv` 必须对所有模板参数实例化才能统一 `vector<Transport<...>>`，且 factory 代码需要编译期发散。
- **虚基类 + 内部 CRTP 优化**：公共接口虚调用，走虚基类；hot path（slot copy、index CAS）内部可 CRTP 或 static dispatch 优化，不要求所有行为都虚。

`TransportConfig` 计划使用 `std::variant<MailboxConfig, QueueConfig, BatchConfig>` — variant 自身携带类型，`TransportFactory::Create(const TransportConfig&)` 内部 `std::visit` 分发，不需要额外 `TransportKind` 参数。

> **当前状态：** 本 ADR 记录的接口是 **planned design**。`TransportFactory` 和 `TransportConfig` variant 尚未实现。当前 `model/transport/` 仅包含 `Transport` 虚基类和 `QueueTransport` 设计骨架。
>
> 实施 `TransportFactory` 时，本 ADR 的设计不变：
> - `TransportConfig` = `std::variant<MailboxTransportConfig, QueueTransportConfig, BatchTransportConfig>`
> - `TransportFactory::Create(const TransportConfig&)` 返回 `std::unique_ptr<Transport>`
