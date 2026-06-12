# 引入 MultiLaneTransport 组合器，不让 VecEnv 直接管理 N 个 Transport

Ns3VecEnv 长期重构需要支持 per-lane 操作：`step(lane)`、`reset(lane)`、`recv_any()`。如果 VecEnv 直接持有 `vector<unique_ptr<Transport>>`，有两个问题：

1. **Transport 选择逻辑散落 VecEnv**：capability 检查、lane error 映射、recv_any 遍历散落各处，重构到多实例 per-lane queue 时需要改 VecEnv 内部。
2. **接口绑定实现**：VecEnv 的单元测试需要 mock N 个 Transport，而 `MultiLaneTransport` 可以 mock 单层。

引入 `MultiLaneTransport`（初版实现 `PerLaneTransportGroup`）作为中间组合器：

```
VecEnv → MultiLaneTransport → N × Transport (SPSC QueueTransport)
```

接口包含 `Send(laneId, msg)`、`Recv(laneId, msg)`、`RecvAny(msg)`、`GetLaneCapabilities()`。

`RecvAny` 是 VecEnv 场景的关键设计：VecEnv 不知道哪个 lane 先完成，需要 "任一 lane 有数据就返回" 的语义，这不适合放在单 Transport 实例上。

初版 `PerLaneTransportGroup` 内部 `vector<unique_ptr<Transport>>`，未来可替换为单实例 per-lane queue，不改变 VecEnv 依赖的接口。
