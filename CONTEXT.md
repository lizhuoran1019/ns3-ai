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

### 校验模式

**严格模式（Strict Mode）**：
schema hash/version 必须非零且两端相等，缺失或 mismatch → 报错失败。Ns3AiSchemaValidationMode::Strict。
_Avoid_: 宽松校验、自动兼容

**兼容模式（Compatibility Mode）**：
允许一端缺失 metadata → 输出 deprecation warning；双方都非零时 mismatch 仍报错失败。Ns3AiSchemaValidationMode::Compatibility。
_Avoid_: 跳过校验、静默兼容

**禁用模式（Disabled Mode）**：
完全跳过 schema 校验 → 输出 visible warning。Ns3AiSchemaValidationMode::Disabled。
_Avoid_: 关闭告警、静默跳过

### 测试接缝

**测试接缝（Test Seam）**：
PRD #52 定义的跨语言兼容性测试分层，按隔离程度从低到高：L0 C++ headless 单元测试、L1 Python wrapper 单元测试（mock 后端）、L2 Python 集成测试（同进程真实共享内存）、L3 Gym protocol 测试（仅未来引入 Gym protocol 兼容性时适用）。按 seam 分层保证测试不依赖外部进程且不互相干扰。
_Avoid_: 端到端集成测试作为唯一验证手段

**Gym Mock 测试（Gym Mock Test）**：
归属于 L1 测试接缝的 Gym 环境 mock 测试，放置在 `python_utils/tests/test_ns3ai_gym_env_mock.py`。测试跨包导入 `model/gym-interface/py` 下的 `Ns3Env`，但导入路径依赖仅限于测试层，不污染包实现。mock 测试模拟 raw pybind 接口，不依赖共享内存、ns-3 二进制或 Gym binding 构建产物。
_Avoid_: 在 Gym 接口包内新建测试套件

**reset_required 即 isGameOver**：
Issue 中 "reset_required signal" 是需求描述用语，不是代码实体。在 `Ns3Env` 中终止/重置信号由 `EnvStateMsg.isGameOver` 承载。mock 测试不应新增 `reset_required` 协议字段，只验证 `isGameOver=True` 时 `reset()` 的正确行为。测试命名应当使用 `game_over` 而非 `reset_required`。
_Avoid_: 新增协议字段、在测试命名中使用 reset_required

**Gym Mock 的 mock 深度**：
Gym L1 mock 测试在 `Ns3AiMsgInterface` 层切入，以 `autoStart=False` 构造 `Ns3Env` 后手动替换 `env.msgInterface`。mock 目标仅限于 `PyRecvBegin`/`PyRecvEnd`/`PySendBegin`/`PySendEnd`/`GetCpp2PyStruct`/`GetPy2CppStruct` 六个方法，protobuf 载荷真实构造（`SimInitMsg.SerializeToString()`、`EnvStateMsg.SerializeToString()`）。不 mock `py_binding` 模块，不全 mock `Experiment.run()`。
_Avoid_: mock py_binding 模块、全 mock Experiment.run()

**Gym Mock 使用自定义 FakeGymInterface**：
Gym mock 测试使用自定义 `FakeGymInterface` 类而非 `unittest.mock.Mock`。内部维护 `_read_q` 读取队列（预装 protobuf 字节，按调用顺序 pop）和 `writes` 写回追踪列表。`GetCpp2PyStruct().get_buffer()` 从队列弹出下一 payload，`GetPy2CppStruct().get_buffer_full()` 捕获 Python 侧写回的 protobuf 字节。每步后通过 `fake_interface.writes[-1]` / `fake_interface.last_written` 断言 action、ack、stopSimReq。
_Avoid_: 使用 unittest.mock 链式调用模拟有状态 Gym 协议

**编译期依赖隔离（Gym Mock）**：
Gym mock 测试在 import `Ns3Env` 前通过 `sys.modules["ns3ai_gym_env.ns3ai_gym_msg_py"]` 注入 fake py_binding，规避真实 pybind11 `.so` 的 import 依赖。fake 至少提供 `msg_buffer_size` 属性与 `Ns3AiMsgInterfaceImpl`（可调用类，构造方法为空，返回 mock 对象）。不从测试目录提交假 `.py` 文件，不修改生产代码的 import 结构。
_Avoid_: 在生产代码中改为惰性 import、在源码树中提交伪造编译产物

**Experiment 超时测试方案**：
L1 超时测试使用真实短等待（`syncTimeoutUs=10_000` 即 10ms），子进程保持存活，`wait_ready()` 持续抛出 `Ns3AiSessionTimeoutError`，`_wait_ready_or_subprocess_exit()` 在 deadline 后重新抛出同名异常。不 mock `time.monotonic()` 或 `time.sleep()`。
_Avoid_: mock 时间函数、零等待超时测试

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

### ABI 变更门禁

**ABI 变更门禁（ABI Change Gate）**：
保护共享内存二进制布局的编译期和运行时双层防御。编译期通过 `sizeof`/`alignof` 静态断言锁定 `Ns3AiMsgSync` 和 `Ns3AiMsgProtocolHeader` 的布局；运行时通过 `NS3_AI_MSG_ABI_VERSION` 跨进程校验防止旧 ABI 打开新 ABI 的段。布局变更时必须同时更新版本号和静态断言值。
_Avoid_: 仅依赖版本号、仅依赖大小检查
