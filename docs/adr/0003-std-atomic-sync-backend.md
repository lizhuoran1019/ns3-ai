# 以 `std::atomic` 替代自定义同步层，不引入额外抽象

同步层（`Ns3AiSemaphore`、`Ns3AiMsgSync`、`Ns3AiMsgInterfaceImpl` 内联原子函数）当前依赖 GCC 内置（`__atomic_*`）操作 `volatile` 变量。本决策将这些替换为标准 C++ `std::atomic`，且不引入中间抽象层。

## 决定

所有用于并发同步的 `volatile` 字段迁移为 `std::atomic<T>`。`Ns3AiSemaphore` 的公开接口直接操作 `std::atomic<uint8_t>*`，`Ns3AiMsgInterfaceImpl` 直接对成员调用 `.load()`/`.store()`/`.compare_exchange_strong()`。不引入 "SyncBackend" 抽象——`std::atomic` 就是后端。

## 为何不引入 SyncBackend 抽象

`Ns3AiSemaphore` 的唯一调用者是 `Ns3AiMsgInterfaceImpl`，两者通过 `std::atomic` 作为底层类型耦合。插入额外抽象层的每一处都是间接调用，而带来的唯一理论收益——"未来切换同步原语"——在本协议的上下文中不成立：信号量只需要二元 CAS/exchange，不存在替换为其他同步机制的场景。

## 考虑的替代方案

- **保留 GCC 内置** — 不可移植到 MSVC；`volatile` + 内置的组合语义在 C++ 标准中无定义。
- **引入 SyncBackend 虚接口** — 增加虚函数调用开销和模板复杂度，对调用者不透明，且本协议的同步语义太简单，不需要这种灵活性。
- **加宽计数器（uint8_t→uint16_t）** — 改变 `Ns3AiMsgSync` 的 sizeof 和字段偏移，破坏 Python 端共享内存兼容性。转而采用二元饱和信号量（exchange/CAS）。
