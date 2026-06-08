# ns3-ai 消息接口

ns3-ai 是 ns-3 的一个 contrib 模块，位于 `contrib/ai` 下，通过共享内存进程间通信将 ns-3 C++ 模拟与 Python 端 AI 工作流连接起来。

## 语言

### 会话

**共享内存会话（Shared Memory Session）**：
由一个 C++ 对端和一个 Python 对端通过共享内存通道共同拥有的通信生命周期。会话在共享状态被创建或加入时开始，在两个对端就该代际达成关闭或错误结论时结束。
_Avoid_: 运行、进程、回合、连接

**就绪的共享内存会话（Ready Shared Memory Session）**：
C++ 对端和 Python 对端均已完成会话握手并确认对方能够参与协议的共享内存会话。
_Avoid_: 本地初始化、已打开的段

**就绪等待（Ready Wait）**：
Python 端等待共享内存会话变为就绪的过程。在会话就绪或已运行时成功；在会话失败时立即失败；在未观察到终端或就绪状态且超过调用者截止时间时超时失败。
_Avoid_: 启动时休眠、进程预热延迟

**运行中的共享内存会话（Running Shared Memory Session）**：
对端已完成握手并可交换邮箱载荷的共享内存会话。"运行中"不依赖首次载荷交换已发生。
_Avoid_: 首条消息、活跃发送

**关闭握手（Close Handshake）**：
共享内存会话的结束过程，一端请求正常关闭，另一端确认该关闭。关闭握手不同于单侧完成标志，因为双方都观察到结束结论。
_Avoid_: 完成标志、进程退出

**失败的共享内存会话（Failed Shared Memory Session）**：
达到错误结论的共享内存会话代际。失败的代际是终态的；恢复意味着创建新代际而非在失败代际中继续。
_Avoid_: 暂停的会话、可恢复的会话

**会话状态（Session State）**：
一个共享内存会话代际的共享生命周期状态，由双方对端可见。描述会话处于初始化、就绪、运行、关闭、已关闭还是失败。
_Avoid_: 对端状态、发送状态、接收状态

**对端状态（Peer State）**：
共享内存会话中一个参与者的本地同步状态。描述该对端在邮箱协议中的具体活动，如就绪、发送、接收、完成或错误。
_Avoid_: 会话状态

**会话 ID（Session ID）**：
一个逻辑共享内存会话的标识，用于关联双方对端的诊断信息和生命周期事件。
_Avoid_: 段名、前缀

**代际 ID（Generation ID）**：
一个共享内存会话共享态的一次具体创建轮次的标识。用于区分新创建的会话代际与使用了相同会话名的过期对端或残留状态。
_Avoid_: 会话 ID、重试计数

### 对端与角色

**对端（Peer）**：
参与共享内存会话的进程角色，仅有 Cpp 和 Py 两种。每端在 `Ns3AiMsgSync` 中有独立的状态字段（`m_cppState`/`m_pyState`），状态机对称：Ready → Sending → Ready 或 Ready → Receiving → Ready，由数据交换的发起方向决定。
_Avoid_: 客户端/服务端、发送方/接收方

**会话创建者（Session Creator）**：
创建共享内存会话状态代际的对端。创建者身份不使其成为会话生命周期的拥有者。
_Avoid_: 会话拥有者

**会话拥有者（Session Owner）**：
不存在共享内存会话生命周期的单一拥有者；双方对端都必须参与握手以及关闭或错误结论。
_Avoid_: 创建者

**对端死亡（Peer Death）**：
一个对端无法继续参与当前共享内存会话代际的情况。可通过进程退出、代际不匹配、协议超时或显式致命错误原因观察到。
_Avoid_: 仅进程退出

**错误原因（Error Reason）**：
一个共享内存会话代际达到错误结论的原因，如超时、模式不匹配、协议不匹配、对端死亡或用户中断。错误原因不是会话状态。
_Avoid_: 超时状态、崩溃状态

### 数据交换

**双缓冲区（Dual Buffer）**：
两个方向独立的消息槽：cpp2py（C++ 写入、Python 读取）和 py2cpp（Python 写入、C++ 读取）。每个方向由一对二元信号量计数器保护——EmptyCount 表示缓冲区空闲可写，FullCount 表示缓冲区已满可读。计数器仅在 0 和 1 之间振荡。
_Avoid_: 输入/输出、请求/响应

### 启动和 API

**Python 先启动（Python-First Startup）**：
Python 端在启动或等待 ns-3 对端加入之前创建共享内存会话代际的启动顺序。
_Avoid_: Python 拥有会话

**ns-3 先启动（ns-3-First Startup）**：
ns-3 端在 Python 端打开并加入之前创建共享内存会话代际的启动顺序。
_Avoid_: ns-3 拥有会话

**Python 生命周期枚举（Python Lifecycle Enum）**：
Python 消息接口包装层使用的 Python 枚举，将会话状态、关闭原因、错误原因和对端身份表示为结构化生命周期值，而非字符串或原始整数。
_Avoid_: 魔法整数、自由格式原因字符串

**Python 消息接口包装层（Python Message Interface Wrapper）**：
ns3-ai 提供的 Python 端包装层，为一个共享内存消息接口提供规范的 Python API，同时将载荷访问和邮箱操作委托给生成的 pybind 消息接口对象。
_Avoid_: 原始 pybind 接口、以生成绑定作为规范 Python API

### 模式元数据

**模式哈希（Schema Hash）**：
消息结构定义的单向哈希值，用于检测 C++ struct 与 Python binding 是否完全一致。哈希由 `Ns3AiMsgTypeSchemaDefaults<PayloadType>` 模板提供，在共享内存头部交换并由对端校验。
_Avoid_: 协议版本、兼容性 token

**模式版本（Schema Version）**：
消息结构定义的单调递增整数版本号，用于辅助人工判断结构新旧。与模式哈希共同组成模式元数据。
_Avoid_: 协议版本号

**模式元数据（Schema Metadata）**：
模式哈希与模式版本的统称，在共享内存会话握手期间由 `Ns3AiMsgProtocolHeader` 承载，由 `ValidateProtocolHeader()` 根据 `Ns3AiSchemaValidationMode`（Strict/Compatibility/Disabled）执行校验。模式元数据校验是结构级等值比对，不是协议版本范围协商。
_Avoid_: 协议兼容性、版本兼容性检查
