# QueueTransport ring-buffer 采用 per-direction 独立控制块 + cache-line padding

双向 ring-buffer 有两种布局选择：

**方案 A — 4 个 index 平铺在同 sync block**：`CppWriteIndex / PyReadIndex / PyWriteIndex / CppReadIndex` 在一个 struct 中。短期简单但两个方向共享 cache line，在 VecEnv 高吞吐场景下 C++-write + Python-write 交替访问产生 false sharing。

**方案 B — per-direction 独立控制块**：`RingBufferControl cpp2py` 和 `RingBufferControl py2cpp` 分离，每方向 64 字节对齐。选此方案：

- 与当前 mailbox 的对称语义一致（Cpp2Py / Py2Cpp 独立索引）
- 两个方向可以独立配置 `depth`、`backpressurePolicy`、`slotSize`
- false sharing 风险最低

`RingBufferControl` 使用单调递增 `uint64_t writeIndex/readIndex`（非循环后 index），实际槽位映射为 `slot = index & (depth - 1)`。depth 要求 power-of-two，避免除法。`writeIndex` 和 `readIndex` 各自放在独立 cache line 上，writer 和 reader 的写入不会互相干扰。

初版 SPSC 只需要 `std::memory_order_release`/`std::memory_order_acquire`，不需要 CAS。

共享内存布局：

```
[ QueueTransportSync (common: session/error/heartbeat) ]
[ RingBufferControl cpp2py ]  — 64-byte aligned
[ RingBufferControl py2cpp ]  — 64-byte aligned
[ ProtocolHeader ]
[ Cpp2Py slots (depth × slotSize) ]
[ Py2Cpp slots (depth × slotSize) ]
```
