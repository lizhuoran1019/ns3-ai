# TransportCapabilities 采用 enum-graded 设计而非 bool salad

初始设计使用 bool flags（`supportsLaneIsolation`、`supportsConcurrentSend`）描述 transport 能力，但这会导致几个问题：`supportsConcurrentSend=true` 无法区分 SPSC vs MPSC；`supportsLaneIsolation=true` 过度承诺 mailbox 不可能提供的东西。

改用 enum 级别表达保证：

- **`ConcurrencyModel`**: `SPSC | MPSC | SPMC | MPMC` 而非 `supportsConcurrentSend: bool`
- **`LaneGuarantee`**: `None | SharedQueueWithLaneMetadata | PhysicalPerLaneTransport` 而非 `supportsLaneIsolation: bool`
- **`OrderingGuarantee`**: `None | StrictPerDirection | TotalOrder` 而非 `supportsMessageOrdering: bool`

Backpressure 能力使用 `uint32_t bitmask`（`BackpressureCapability`）替代单个 bool。

VecEnv 构造时检查 `capabilities.laneGuarantee >= RequiredLaneGuarantee`，而不是检查 `capabilities.supportsLaneIsolation`。这允许 `MailboxTransport` 支持 `preservesLaneMetadata=true` 但不支持 lane 隔离。

如果初始用 bool，后续 enum 化时需要改所有 consumer 的比较逻辑，破坏面更大。
