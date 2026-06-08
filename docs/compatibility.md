# 跨语言 Schema/ABI 兼容性策略

本文将说明 ns3-ai 的跨语言兼容性治理体系，涵盖校验层次、校验模式、schema 元数据生成与声明、以及常量变更策略。

---

## 目录

- [两层校验体系 + Gym 特化注入](#两层校验体系--gym-特化注入)
- [三种校验模式](#三种校验模式)
- [旧 binding 迁移路径](#旧-binding-迁移路径)
- [Schema hash/version 生成](#schema-hashversion-生成)
- [Schema 元数据声明模式](#schema-元数据声明模式)
- [Python 侧元数据读取优先级](#python-侧元数据读取优先级)
- [常量变更策略](#常量变更策略)
- [Gym 当前兼容性策略](#gym-当前兼容性策略)
- [PR checklist 参考](#pr-checklist-参考)

---

## 两层校验体系 + Gym 特化注入

ns3-ai 在共享内存会话（[CONTEXT](../CONTEXT.md)）握手阶段执行两层校验，由 `ValidateProtocolHeader()`（`model/msg-interface/ns3-ai-msg-interface.h`）统一完成：

### 1. ABI / layout compatibility

校验共享内存协议头和同步对象的底层 ABI/layout 是否一致。关注：

- Header magic（`NS3_AI_MSG_HEADER_MAGIC`）
- ABI version（`NS3_AI_MSG_ABI_VERSION`）
- Protocol header size（`sizeof(Ns3AiMsgProtocolHeader)`）
- 双向 payload size（`sizeof(Cpp2PyMsgType)` / `sizeof(Py2CppMsgType)`）

此层不依赖校验模式，任何 mismatch 都会立即失败并报告 `ProtocolMismatch`。

### 2. Payload schema metadata compatibility

校验 C++/Python 双向 payload struct 的 schema 元数据是否一致。关注：

- `cpp2py_schema_hash` / `py2cpp_schema_hash`
- `cpp2py_schema_version` / `py2cpp_schema_version`

此层按 `Ns3AiSchemaValidationMode` 执行三种模式（见下节）。

### Gym 特化注入

Gym 接口不再拥有独立的 protocol-version handshake 层。Gym binding 通过 `Ns3AiMsgTypeSchemaDefaults<Ns3AiGymMsg>` 模板特化将 `NS3_AI_GYM_MSG_SCHEMA_HASH` / `NS3_AI_GYM_MSG_SCHEMA_VERSION` 注入到上述 payload schema metadata 层，以统一的 L2 校验机制完成兼容性检测。

---

## 三种校验模式

| 模式 | 枚举值 | 行为 |
|---|---|---|
| **Strict**（默认） | `Ns3AiSchemaValidationMode::Strict` | schema hash/version 必须非 0 且两端相等。任意一方缺失或 mismatch → 失败抛出异常 |
| **Compatibility** | `Ns3AiSchemaValidationMode::Compatibility` | 允许一端缺失 metadata → 输出 deprecation warning；两端都非零时 mismatch 仍失败。用于旧 binding 过渡 |
| **Disabled** | `Ns3AiSchemaValidationMode::Disabled` | 完全跳过 schema 校验 → 输出 visible warning。仅在开发初期临时使用 |

### 配置方式

**C++ 端**：通过 `Ns3AiMsgInterfaceConfig` 的 `m_schemaValidationMode` 字段设置，默认 `Strict`。

**Python 端**：通过 `Ns3AiMsgInterface.create()/open()` 或 `Experiment` 构造函数的 `schema_validation_mode` 参数设置。支持字符串 `"strict"` / `"compatibility"` / `"disabled"` 或 `SchemaValidationMode` 枚举值。

---

## 旧 binding 迁移路径

旧 binding（未声明 schema metadata）应按以下阶段单向迁移：

```
Disabled → Compatibility → Strict
```

各阶段说明：

| 阶段 | 状态 | 操作 | 预期行为 |
|---|---|---|---|
| 1. 临时 Disabled | binding 无任何 schema metadata | `schema_validation_mode="disabled"` | 校验完全跳过，输出 visible warning。确认 struct 可正常工作 |
| 2. 过渡 Compatibility | binding 已添加 deprecated 别名 (`schema_hash` / `schema_version`) | `schema_validation_mode="compatibility"` | 一端缺失时输出 deprecation warning，不阻塞运行。验证 hash 值与对端一致 |
| 3. 最终 Strict | binding 已添加定向 metadata (`cpp2py_schema_hash` / `py2cpp_schema_hash` / versions) | `schema_validation_mode="strict"`（默认） | 完全校验。缺失或 mismatch 立即失败 |

迁移一旦进入下一阶段不应回退。最终目标是所有 binding 运行在 `Strict` 模式。

---

## Schema hash/version 生成

### `ComputeNs3AiMsgSchemaHash()` 输入要素

`ComputeNs3AiMsgSchemaHash()`（`model/msg-interface/ns3-ai-msg-interface.h`）使用 FNV-1a 哈希算法从以下字段计算 64-bit hash：

1. **Schema name** — 字符串，如 `"EnvStruct"`
2. **Schema version** — `uint32_t` 版本号
3. **Struct size** — `sizeof(payload_struct)`
4. 每个 field：
   - **Field name** — 字符串
   - **Field type** — `Ns3AiMsgFieldType` 枚举值（UInt8/16/32/64、Int8/16/32/64、Float、Double、Bool 等）
   - **Field offset** — 在 struct 中的字节偏移
   - **Field size** — `sizeof(field_type)`
   - **Field count** — 数组元素个数（单字段为 1）

### 手动计算示例

参考 `examples/a-plus-b/use-msg-stru/apb.h` 中的注释和常量：

```cpp
/**
 * Schema hash 由 ComputeNs3AiMsgSchemaHash() 从以下输入计算：
 *   struct EnvStruct (version 1, size 8)
 *   fields:
 *     env_a  (UInt32, offset=0, size=4, count=1)
 *     env_b  (UInt32, offset=4, size=4, count=1)
 */
inline constexpr uint64_t ENV_STRUCT_SCHEMA_HASH   = 0xa66c066d6d9b046fULL;
inline constexpr uint32_t ENV_STRUCT_SCHEMA_VERSION = 1;
```

---

## Schema 元数据声明模式

### 1. Payload header 中定义常量

在 payload struct 所在的头文件中定义：

```cpp
inline constexpr uint64_t MY_STRUCT_SCHEMA_HASH   = 0x...ULL;
inline constexpr uint32_t MY_STRUCT_SCHEMA_VERSION = 1;
```

### 2. `Ns3AiMsgTypeSchemaDefaults<T>` 特化

使 C++ 端在编译期持有 schema 元数据：

```cpp
template <>
struct ns3::Ns3AiMsgTypeSchemaDefaults<::MyStruct>
{
    static constexpr uint64_t SchemaHash = ::MY_STRUCT_SCHEMA_HASH;
    static constexpr uint32_t SchemaVersion = ::MY_STRUCT_SCHEMA_VERSION;
};
```

### 3. pybind module 中导出为 module attr

在 `*_py.cc` 中导出 4 个定向 attr 和 2 个 deprecated 别名：

```cpp
// 定向 attr（优先读取）
m.attr("cpp2py_schema_hash")    = py::int_(ENV_STRUCT_SCHEMA_HASH);
m.attr("py2cpp_schema_hash")    = py::int_(ACT_STRUCT_SCHEMA_HASH);
m.attr("cpp2py_schema_version") = ENV_STRUCT_SCHEMA_VERSION;
m.attr("py2cpp_schema_version") = ACT_STRUCT_SCHEMA_VERSION;

// deprecated 别名（旧 wrapper 回退）
m.attr("schema_hash")    = py::int_(ENV_STRUCT_SCHEMA_HASH);
m.attr("schema_version") = ENV_STRUCT_SCHEMA_VERSION;
```

**注意**：
- `cpp2py_*` 对应 C++→Python 方向的 struct
- `py2cpp_*` 对应 Python→C++ 方向的 struct
- `schema_hash` / `schema_version` 是单向的 deprecated 别名（仅对应 cpp2py 方向），保留用于旧 `Ns3AiMsgInterface` / `Experiment` wrapper 的回退兼容
- Gym binding（`model/gym-interface/py/msg_py_binding.cc`）因 Gym 双向共用同一 struct，hash 和 version 在两个方向上相同

---

## Python 侧元数据读取优先级

`ns3ai_utils.py` 中 `_resolve_metadata()` 实现以下优先级：

```
1. 显式参数（explicit arg）
     ↓ 未提供
2. module 定向 attr（cpp2py_schema_hash / py2cpp_schema_hash）
     ↓ 未提供
3. module deprecated 别名（schema_hash / schema_version），触发 DeprecationWarning
     ↓ 仍未提供
4. 返回 0
```

在 `Strict` 模式下值 0 会导致校验失败并报错；在 `Compatibility` 模式下值 0 触发 warning；在 `Disabled` 模式下静默通过。

---

## 常量变更策略

| 常量 | 变更动作 | 触发条件 |
|---|---|---|
| **Schema hash** | 重新计算 | payload struct 字段增删改、类型变化、offset/size/count 变化 |
| **Schema version** | 人工递增 | 需要维护者显式标记的 payload schema 语义变化 |
| **`NS3_AI_MSG_ABI_VERSION`** | 修改 | `Ns3AiMsgProtocolHeader` 或 sync/header ABI layout 发生变化 |

### ABI 版本说明

`NS3_AI_MSG_ABI_VERSION`（当前值：1）仅在共享内存协议头（`Ns3AiMsgProtocolHeader`）或同步区域（`Ns3AiMsgSync`）的二进制布局变化时才需要修改。这是极低频操作，通常只发生在 ns3-ai 核心开发中。

---

## Gym 当前兼容性策略

- Gym 不做独立的 protocol version negotiation
- 兼容性校验完全依托于上述两层体系：
  - **L1**：`Ns3AiGymMsg` 的 `sizeof` 和 `buffer` layout 由 ABI/layout 层校验
  - **L2**：`Ns3AiGymMsg` 的 `NS3_AI_GYM_MSG_SCHEMA_HASH` / `NS3_AI_GYM_MSG_SCHEMA_VERSION` 通过 `Ns3AiMsgTypeSchemaDefaults` 注入 payload schema 层校验
- Gym Python binding（`ns3ai_gym_msg_py` 模块）暴露了完整的双向 schema metadata（`cpp2py_schema_hash` / `py2cpp_schema_hash` / versions）

> 注：原 [#21](https://github.com/lizhuoran1019/ns3-ai/issues/21) 提议的 Gym protobuf protocol version negotiation 已在设计审查中关闭为 `not_planned`。如未来重新需要 Gym protocol-level negotiation，应另开 issue。

---

## PR checklist 参考

当修改涉及 schema/ABI 兼容性时，对照以下清单逐项检查：

```markdown
- [ ] 此修改是否影响跨语言 schema/ABI 兼容性？如果是，是否更新了对应 schema hash/version 或 ABI version？
- [ ] 新 binding 是否暴露了 cpp2py/py2cpp 双向 schema metadata？
- [ ] 所有测试接缝（按 PRD #52 test seam 分层）是否已同步更新？
  - L0: `test/ns3-ai-schema-validation-test-suite.cc`
  - L1: `python_utils/tests/test_ns3ai_utils_lifecycle.py`
  - L2: `python_utils/tests/test_ns3ai_utils_integration.py`
  - L3: Gym protocol tests（仅未来重新引入 Gym protocol compatibility 时适用）
```
