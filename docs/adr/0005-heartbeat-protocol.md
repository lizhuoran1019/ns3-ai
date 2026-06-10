# 采用双向心跳实现共享内存对端死亡检测

共享内存会话（[CONTEXT](../CONTEXT.md)）目前仅在 `WaitForSync` 信号量等待过程中通过 `DEFAULT_SEM_WAIT_TIMEOUT_US`（300 秒）超时检测对端异常退出。这一机制有两个缺陷：
1. 完全空闲的会话（无数据交换）无法检测对端死亡
2. 即使有数据交换，检测延迟可能高达 300 秒

本 ADR 设计心跳协议，使任一对端可在对方无声退出后合理时间内检测并产生 `Ns3AiMsgErrorReason::PeerDeath` 结构化错误。

---

## 决策 1：发起方 — 双向心跳

每端独立发布自己的存活信号，独立检测对端存活信号是否停滞。不是 request/response ping，不需要 ACK。

**理由：**
- 与现有对称对端模型（`m_cppState`/`m_pyState`）一致，不引入主从关系
- 单方发起意味着非发起端仍依赖数据交换超时检测对端死亡
- 双向心跳让两端用**相同机制**检测对端死亡

**未采用：**
- **主从心跳**：一端 ping 另一端 pong，人为引入不对称
- **单向心跳**：只有 C++ 端发送，Python 端仍依赖 `sync_timeout_us`

## 决策 2：心跳间隔 — 默认 1 秒，可配置

| 参数 | 默认值 | 合法范围 |
|---|---|---|
| `heartbeat_period_us` | 1,000,000 (1 秒) | [100,000, 60,000,000] |

- C++ 端和 Python 端可独立配置自己的发送周期
- 1 秒间隔将 silent death 检测从 300 秒量级降到秒级
- 每端每秒 1 次 atomic store，开销可忽略
- 100ms 下限避免用户误设过高频心跳；60s 上限确保 `N=3` 超时仍小于 300s 数据交换超时

**未采用：**
- **统一周期**：两端负载形态不同，不应强制相同周期
- **硬编码周期**：缺乏可配置性无法适应不同场景

## 决策 3：超时模型 — 计数器 + 本地 wall-clock 追踪

共享内存中只放 `std::atomic<uint64_t>` 单调递增计数器，不发布时间戳或周期配置。

**共享内存字段（`Ns3AiMsgSync`）：**

```cpp
std::atomic<uint64_t> m_cppHeartbeatCounter{0};    // C++ 端写入
std::atomic<uint64_t> m_pyHeartbeatCounter{0};     // Python 端写入
```

**每端本地追踪：**

```
m_lastObservedPeerCount  — 上次看到的对端计数值
m_lastObservedPeerTime   — 当时本地的 steady_clock::now()

检测逻辑（运行在 WaitForSync 自旋循环中）:
  peer_cnt = peer_counter.load(acquire)
  if peer_cnt == m_lastObservedPeerCount:
      if now - m_lastObservedPeerTime > heartbeat_timeout_us:
          MarkPeerError(peer, PeerDeath)
  else:
      m_lastObservedPeerCount = peer_cnt
      m_lastObservedPeerTime = now
```

**约束：**

```text
heartbeat_timeout_us ∈ [max(3 × heartbeat_period_us, 300,000), sync_timeout_us - 1]
```

默认 `period=1s, timeout=3s`。`3×` 下限提供调度抖动容错。

**正确性：**
- `uint64_t` 在 1Hz 递增下约 5.8 亿年回绕，不在关注范围内
- Linux `CLOCK_MONOTONIC`（`steady_clock`）跨进程时间线一致，可安全比较
- 检测方只看"对端 counter 是否在我的超时窗口内推进"，不依赖对端周期配置
- 两端周期不对称由用户保证：本端 `heartbeat_timeout_us` 必须大于对端实际 `heartbeat_period_us`

**未采用：**
- **共享 timestamp**：跨语言时间戳编码、单位、时钟源一致性增加复杂度
- **固定绝对超时窗口**：不如 counter + 本地时钟灵活
- **单次丢失即判死**：调度抖动和 GIL 竞争可能造成假阳性
- **对端周期自动协商**：协议复杂化，收益有限

## 决策 4：线程模型 — Python 后台发布 + C++ WaitForSync 嵌入检测

发布（publish）与检测（detect）解耦：

### 发布

| 端 | 机制 | 行为 |
|---|---|---|
| C++ | `WaitForSync` 自旋循环中嵌入 | `self_counter.fetch_add(1, release)` |
| Python | `threading.Thread`（daemon=True） | 每秒调用 `HeartbeatPublish()`（`fetch_add`） |

### 检测

| 端 | 机制 | 行为 |
|---|---|---|
| C++ | `WaitForSync` 自旋循环中嵌入 | 检查对端 counter 是否推进 |
| Python | **不检测** | 依赖 pybind 调用的 C++ 侧 `WaitForSync` |

**Python daemon 约束：**
- `heartbeat_publish()` 必须是纯原子操作：只 `fetch_add(1, release)`，不读对端、不写 error、不改 session state、不抛异常
- `daemon=True` 不足以安全退出；必须提供显式 `start_heartbeat_publisher()` / `stop_heartbeat_publisher()`，在 `close()`/`__del__` 中先 stop 后释放 raw_interface

**设计理由：**
- C++ 在 `Simulator::Run()` 期间不发心跳是正常状态（C++ 没有阻塞等待 Python），Python 不应在此时判 C++ 死亡。将检测限制在 `WaitForSync` 中避免了此假阳性
- 只有当本端在 `WaitForSync` 中等待对端释放 slot 时，对端 counter 长期不推进才有死亡语义
- Python daemon 持续发布 `m_pyHeartbeatCounter`，使 C++ 在等 Python 时不会误判 Python idle 为死亡

**未采用：**
- **C++ `std::thread`**：为心跳引入独立线程破坏了 ns-3 单线程模型，且只在 `WaitForSync` 等待期间才有检测意义
- **Python 侧检测**：C++ 在 `Simulator::Run()` 期间不发布，Python 侧检测必然产生假阳性
- **两端的 daemon 线程**：不需要——检测不应空闲时进行

## 决策 5：布局影响 — `Ns3AiMsgSync` 末尾新增 2 × `uint64_t`

**受影响结构体：`Ns3AiMsgSync`**（`model/msg-interface/ns3-ai-msg-interface.h`）

```cpp
struct Ns3AiMsgSync
{
    // 现有 14 个 atomic 单字节字段（offset 0-13, sizeof 14, alignof 1）
    std::atomic<uint8_t> m_cpp2pyEmptyCount{1};
    // ... 其他 13 个现有字段保持不变 ...

    // [offset 14-15: PADDING, 2 bytes, for 8-byte alignment of uint64_t]

    // 新增心跳计数器
    std::atomic<uint64_t> m_cppHeartbeatCounter{0};   // offset 16
    std::atomic<uint64_t> m_pyHeartbeatCounter{0};    // offset 24
};
// sizeof = 32, alignof = 8
```

**ABI 门禁触发项（ref #58）：**

| 门禁项 | 当前值 | 新值 |
|---|---|---|
| `sizeof(Ns3AiMsgSync)` | 14 | 32 |
| `alignof(Ns3AiMsgSync)` | 1 | 8 |
| `NS3_AI_MSG_ABI_VERSION` | 1 | 2 |
| `offsetof(m_cppHeartbeatCounter)` | N/A | 16 |
| `offsetof(m_pyHeartbeatCounter)` | N/A | 24 |

**未受影响：**
- `Ns3AiMsgProtocolHeader` — 无字段变更，仅 version bump
- Payload struct schema — 不变，schema hash/version 无需更新

## 决策 6：Error 状态 — 无额外 Grace Period

心跳超时触发后**立即**调用 `MarkPeerError(peer, PeerDeath)` 将会话推至 Error 状态，不设置额外缓冲阶段。

**理由：**
- `heartbeat_timeout_us`（默认 `3 × period = 3s`）本身就是容错窗口
- 如果 3 秒内对端 counter 毫无推进，极大概率不是瞬时抖动

**WaitForSync 立即返回要求：**
`MarkPeerError` 仅设置 session state 无法打断当前线程的 `WaitForSync` 自旋。实现必须让 `WaitForSync` 感知 `m_sessionState == Error` 并立即返回 Aborted，而非继续等待直到 `sync_timeout_us` 耗尽。

返回路径优先级：

```text
WaitForSync 返回后:
  1. if sessionState == Error → 传播错误原因（PeerDeath/Timeout/ProtocolMismatch）
  2. else if m_isFinished → 正常 finish 路径
  3. else if timeout → MarkPeerError(peer, Timeout)
```

---

## 全链路验证

### 正常心跳维持

| 步骤 | C++ 侧 | Python 侧 |
|---|---|---|
| 1 | `WaitForSync` 进入自旋 | daemon 每 1s `fetch_add(m_pyCounter)` |
| 2 | spin 迭代中 `fetch_add(m_cppCounter)` | — |
| 3 | 检查 `m_pyCounter` 推进 → 重置本地 watch | — |
| 4 | 信号量可用 → 返回 Acquired | — |

### C++ 崩溃后 Python daemon 行为

| 步骤 | 事件 |
|---|---|
| 1 | C++ 进程崩溃（kill -9） |
| 2 | `m_cppHeartbeatCounter` 停止推进 |
| 3 | Python daemon **不检测** → 无错误 |
| 4 | Python 下次 `PySendBegin`/`PyRecvBegin` → pybind 调用 C++ `WaitForSync` |
| 5 | C++ `WaitForSync` 自旋中检查 `m_cppCounter` → 停滞 |
| 6 | 超过 `heartbeat_timeout_us` → `MarkPeerError(Cpp, PeerDeath)` |
| 7 | `m_sessionState = Error` → `WaitForSync` abort → pybind 异常传播 |

### Python 崩溃后 C++ 检测

| 步骤 | 事件 |
|---|---|
| 1 | Python 进程崩溃（kill -9） |
| 2 | `m_pyHeartbeatCounter` 停止推进 |
| 3 | C++ `WaitForSync` 自旋中发现 `m_pyCounter` 停滞 |
| 4 | 超过 `heartbeat_timeout_us` → `MarkPeerError(Py, PeerDeath)` |
| 5 | `m_sessionState = Error` → wait abort → C++ 抛出 `Ns3AiProtocolError`（错误原因 PeerDeath） |

### 对现有逻辑的影响

- **无影响**：心跳是新增功能，不改变现有 `WaitForSync`/`MarkPeerError`/`TryBeginDataExchange` 路径的逻辑
- 唯一变化是 `WaitForSync` 自旋循环扩展了 abort 条件（`m_sessionState == Error`）
- 现有 300s `sync_timeout_us` 行为不变，仅作为心跳机制的保守后备

---

## 相关文档

- [兼容性策略](/contrib/ai/docs/compatibility.md) — ABI version 1→2 更新
- [CONTEXT.md](../CONTEXT.md) — 新增心跳术语
- Issue #58 — ABI/layout change gate
- Issue #48 — 心跳实现（本 ADR 决策后进入实现阶段）
