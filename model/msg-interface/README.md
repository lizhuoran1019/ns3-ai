# Message Interface

## Scope

The message interface is the low-level shared-memory IPC layer used by ns3-ai. It is lower level than the Gym interface and exposes the C++ payload objects directly to both peers.

The transport is a bidirectional, single-slot, lock-step mailbox. It is not a general message queue: each direction has one empty/full slot, and both peers must call the corresponding begin/end operations in a compatible order. This design minimizes serialization overhead, but it does not provide buffering, batching, priority, drop policy, overwrite policy, or multi-consumer routing.

Use this interface when you need direct access to an existing C++ `struct` or shared-memory `std::vector`. Use the Gym interface when you need Gymnasium-compatible reset/step semantics.

## Current protocol guarantees

- Semaphore waits use bounded waiting by default. The default timeout is 300 seconds; setting the timeout to `0` restores unbounded waiting.
- `SetHandleFinish(true)` enables an out-of-band finish flag so the Python side can stop waiting when the C++ side ends the session.
- A protocol header is stored beside the payload objects. It records ABI version, payload sizes, and optional schema hash/version metadata.
- Schema metadata is a layout compatibility check for C++ payloads. It is not a language-independent IDL or stable wire format.
- Per-interface configuration is available through `Ns3AiMsgInterfaceConfig`; this avoids mutating singleton defaults when one process needs multiple differently configured interfaces.

## Struct-based message interface

The struct-based mode shares one C++ object in each direction. In the A-Plus-B example, C++ sends two values and Python returns their sum:

```c++
struct EnvStruct
{
    uint32_t env_a;
    uint32_t env_b;
};

struct ActStruct
{
    uint32_t act_c;
};
```

Acquire an interface with either the legacy singleton defaults or an explicit per-instance config:

```c++
ns3::Ns3AiMsgInterfaceConfig config;
config.m_isMemoryCreator = false;
config.m_useVector = false;
config.m_handleFinish = true;
config.m_syncTimeoutUs = 300000000;
config.m_names = ns3::Ns3AiMsgInterface::MakeNames("apb-0");

auto* msgInterface = ns3::Ns3AiMsgInterface::Get()->GetInterface<EnvStruct, ActStruct>(
    config,
    "apb-0");
```

A C++ send/receive cycle must be paired with the opposite Python receive/send cycle:

```c++
msgInterface->CppSendBegin();
msgInterface->GetCpp2PyStruct()->env_a = temp_a;
msgInterface->GetCpp2PyStruct()->env_b = temp_b;
msgInterface->CppSendEnd();

msgInterface->CppRecvBegin();
uint32_t sum = msgInterface->GetPy2CppStruct()->act_c;
msgInterface->CppRecvEnd();
```

The synchronization calls are part of the protocol. Calling `CppRecvBegin()` first requires the Python peer to call `PySendBegin()` first. A wrong order no longer spins forever by default; it fails after the configured timeout.

## Vector-based message interface

The vector mode shares a Boost interprocess vector in each direction. It is useful for a fixed batch of payload objects, but it still uses the same single-slot synchronization protocol.

```c++
config.m_useVector = true;
auto* msgInterface = ns3::Ns3AiMsgInterface::Get()->GetInterface<EnvStruct, ActStruct>(
    config,
    "apb-vector-0");

msgInterface->CppSendBegin();
msgInterface->GetCpp2PyVector()->at(0).env_a = temp_a;
msgInterface->GetCpp2PyVector()->at(0).env_b = temp_b;
msgInterface->CppSendEnd();
```

The vector payload can hold many elements, but the channel still publishes one vector snapshot per send/end pair. It is not an asynchronous queue.

## Finish handling

When `m_handleFinish` is enabled, C++ can mark the session as finished without waiting for the C++ to Python data slot:

```c++
msgInterface->CppSetFinished();
```

Python should check the finish flag after receiving begins:

```python
msgInterface.PyRecvBegin()
if msgInterface.PyGetFinished():
    done = True
```

The finish flag is intentionally small. It only tells the peer that the shared session is finished; it does not carry a structured reason or close handshake.

## Schema metadata

Schema descriptors can be created with the helper macros:

```c++
static const auto envSchema = NS3_AI_MSG_SCHEMA(
    EnvStruct,
    1,
    NS3_AI_MSG_FIELD(EnvStruct, ns3::Ns3AiMsgFieldType::UInt32, env_a),
    NS3_AI_MSG_FIELD(EnvStruct, ns3::Ns3AiMsgFieldType::UInt32, env_b));

static const auto actSchema = NS3_AI_MSG_SCHEMA(
    ActStruct,
    1,
    NS3_AI_MSG_FIELD(ActStruct, ns3::Ns3AiMsgFieldType::UInt32, act_c));

config.SetSchemas(envSchema, actSchema);
```

The creator writes schema metadata into the shared protocol header. The opener validates payload size unconditionally and validates schema hash/version when non-zero schema metadata is provided.

## Python binding model

The Python side reuses the C++ implementation through pybind11. Because the C++ interface is template-based, each payload pair still needs a binding module that instantiates `Ns3AiMsgInterfaceImpl<Cpp2Py, Py2Cpp>`.

`ns3-ai-pybind-schema.h` provides helper functions for schema-driven scalar field binding, but it is not a full binding generator and it is intentionally kept out of the normal ns-3 C++ aggregate header to avoid forcing ordinary C++ executables to link against Python.

## Design limits

The message interface optimizes direct shared-memory data exchange. It does not by itself provide runtime supervision, worker restart, heartbeat, multi-slot buffering, routing, or a language-independent schema system. Higher-level code must own those policies explicitly.
