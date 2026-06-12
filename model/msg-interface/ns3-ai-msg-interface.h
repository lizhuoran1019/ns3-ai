/*
 * Copyright (c) 2023 Huazhong University of Science and Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_MSG_INTERFACE_H
#define NS3_AI_MSG_INTERFACE_H

#include "ns3-ai-errors.h"
#include "ns3-ai-semaphore.h"

#include <ns3/singleton.h>

#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>

#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * \file ns3-ai-msg-interface.h
 * \ingroup ai
 *
 * MailboxTransport — single-slot, lock-step shared-memory IPC channel.
 *
 * This is NOT a general-purpose message queue. It provides:
 * - Single-slot: only one in-flight message per direction at a time.
 * - Lock-step: producer waits until consumer consumes, then exchanges roles.
 * - Bidirectional: C++→Python and Python→C++ each have one slot.
 * - Request/Response: one side sends, the other receives, then reverses.
 *
 * It does NOT provide:
 * - Queue depth / buffering / batching.
 * - Message routing / lane isolation / multi-consumer dispatch.
 * - Async send / non-blocking recv (beyond timeout-based spin).
 * - Per-message ordering guarantees beyond strict alternation.
 *
 * For queue, batch, or multi-agent transport semantics, see QueueTransport,
 * BatchTransport, or higher-level MultiLaneTransport wrappers.
 */

namespace ns3
{

static constexpr uint32_t NS3_AI_MSG_HEADER_MAGIC = 0x4E334149;
static constexpr uint16_t NS3_AI_MSG_ABI_VERSION = 2;

/* ---- ABI 静态断言：std::atomic 在共享内存中的布局安全 ---- */
static_assert(sizeof(std::atomic<uint8_t>) == sizeof(uint8_t),
              "std::atomic<uint8_t> 的大小必须等于 uint8_t");
static_assert(sizeof(std::atomic<uint16_t>) == sizeof(uint16_t),
              "std::atomic<uint16_t> 的大小必须等于 uint16_t");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "std::atomic<uint32_t> 的大小必须等于 uint32_t");
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t),
              "std::atomic<uint64_t> 的大小必须等于 uint64_t");
static_assert(sizeof(std::atomic<bool>) == sizeof(bool),
              "std::atomic<bool> 的大小必须等于 bool");
static_assert(std::atomic<uint8_t>::is_always_lock_free,
              "std::atomic<uint8_t> 在共享内存中必须为无锁");
static_assert(std::atomic<uint16_t>::is_always_lock_free,
              "std::atomic<uint16_t> 在共享内存中必须为无锁");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> 在共享内存中必须为无锁");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "std::atomic<uint64_t> 在共享内存中必须为无锁");
static_assert(std::atomic<bool>::is_always_lock_free,
              "std::atomic<bool> 在共享内存中必须为无锁");

static_assert(alignof(std::atomic<uint8_t>) == alignof(uint8_t),
              "std::atomic<uint8_t> 的对齐必须等于 uint8_t");
static_assert(alignof(std::atomic<uint16_t>) == alignof(uint16_t),
              "std::atomic<uint16_t> 的对齐必须等于 uint16_t");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t),
              "std::atomic<uint32_t> 的对齐必须等于 uint32_t");
static_assert(alignof(std::atomic<uint64_t>) == alignof(uint64_t),
              "std::atomic<uint64_t> 的对齐必须等于 uint64_t");

enum class TransportPeer : uint8_t
{
    Cpp = 1,
    Py = 2
};

inline std::ostream&
operator<<(std::ostream& os, TransportPeer peer)
{
    switch (peer)
    {
    case TransportPeer::Cpp:
        return os << "cpp";
    case TransportPeer::Py:
        return os << "python";
    }
    return os << "unknown";
}

enum class Ns3AiMsgPeerState : uint8_t
{
    Initializing = 0,
    Ready = 1,
    Sending = 2,
    Receiving = 3,
    Finished = 4,
    Error = 5
};

inline std::ostream&
operator<<(std::ostream& os, Ns3AiMsgPeerState state)
{
    switch (state)
    {
    case Ns3AiMsgPeerState::Initializing:
        return os << "initializing";
    case Ns3AiMsgPeerState::Ready:
        return os << "ready";
    case Ns3AiMsgPeerState::Sending:
        return os << "sending";
    case Ns3AiMsgPeerState::Receiving:
        return os << "receiving";
    case Ns3AiMsgPeerState::Finished:
        return os << "finished";
    case Ns3AiMsgPeerState::Error:
        return os << "error";
    }
    return os << "unknown";
}

enum class TransportSessionState : uint8_t
{
    Init = 0,
    Ready = 1,
    Running = 2,
    Closing = 3,
    Closed = 4,
    Error = 5
};

enum class TransportCloseReason : uint8_t
{
    None = 0,
    Normal = 1,
    UserInterrupted = 2
};

enum class TransportErrorReason : uint8_t
{
    None = 0,
    Timeout = 1,
    PeerDeath = 2,
    ProtocolMismatch = 3,
    StaleGeneration = 4,
    UserInterrupted = 5,
    InvalidState = 6
};

inline std::ostream&
operator<<(std::ostream& os, TransportSessionState state)
{
    switch (state)
    {
    case TransportSessionState::Init:
        return os << "init";
    case TransportSessionState::Ready:
        return os << "ready";
    case TransportSessionState::Running:
        return os << "running";
    case TransportSessionState::Closing:
        return os << "closing";
    case TransportSessionState::Closed:
        return os << "closed";
    case TransportSessionState::Error:
        return os << "error";
    }
    return os << "unknown";
}

inline std::ostream&
operator<<(std::ostream& os, TransportCloseReason reason)
{
    switch (reason)
    {
    case TransportCloseReason::None:
        return os << "none";
    case TransportCloseReason::Normal:
        return os << "normal";
    case TransportCloseReason::UserInterrupted:
        return os << "user_interrupted";
    }
    return os << "unknown";
}

inline std::ostream&
operator<<(std::ostream& os, TransportErrorReason reason)
{
    switch (reason)
    {
    case TransportErrorReason::None:
        return os << "none";
    case TransportErrorReason::Timeout:
        return os << "timeout";
    case TransportErrorReason::PeerDeath:
        return os << "peer_death";
    case TransportErrorReason::ProtocolMismatch:
        return os << "protocol_mismatch";
    case TransportErrorReason::StaleGeneration:
        return os << "stale_generation";
    case TransportErrorReason::UserInterrupted:
        return os << "user_interrupted";
    case TransportErrorReason::InvalidState:
        return os << "invalid_state";
    }
    return os << "unknown";
}

/**
 * \brief 共享内存会话同步状态。
 *
 * 所有字段通过 std::atomic 访问以实现跨进程安全。布局与旧版 volatile 字段兼容。
 */
struct MailboxSyncBlock
{
    std::atomic<uint8_t> m_cpp2pyEmptyCount{1};
    std::atomic<uint8_t> m_cpp2pyFullCount{0};
    std::atomic<uint8_t> m_py2cppEmptyCount{1};
    std::atomic<uint8_t> m_py2cppFullCount{0};
    std::atomic<bool> m_isFinished{false};
    std::atomic<uint8_t> m_sessionState{static_cast<uint8_t>(TransportSessionState::Init)};
    std::atomic<uint8_t> m_closeReason{static_cast<uint8_t>(TransportCloseReason::None)};
    std::atomic<uint8_t> m_closeRequester{0};
    std::atomic<uint8_t> m_closeAcknowledger{0};
    std::atomic<uint8_t> m_errorReason{static_cast<uint8_t>(TransportErrorReason::None)};
    std::atomic<uint8_t> m_cppState{static_cast<uint8_t>(Ns3AiMsgPeerState::Ready)};
    std::atomic<uint8_t> m_pyState{static_cast<uint8_t>(Ns3AiMsgPeerState::Ready)};
    std::atomic<uint8_t> m_lastErrorPeer{0};
    std::atomic<uint8_t> m_lastErrorCode{0};

    // padding (2 bytes at offset 14-15, for 8-byte alignment of uint64_t)
    // Per-peer heartbeat counters. Only m_pyHeartbeatCounter is currently used
    // for peer-death detection (C++ WaitForSync checks Python publisher liveness).
    // m_cppHeartbeatCounter is published by C++ WaitForSync for future observability.
    // Python wait path must NOT use m_cppHeartbeatCounter to判死 C++,
    // because C++ may be computing outside WaitForSync (no counter advancement).
    std::atomic<uint64_t> m_cppHeartbeatCounter{0}; // offset 16, C++ 端写入
    std::atomic<uint64_t> m_pyHeartbeatCounter{0};  // offset 24, Python 端写入
};

/* ---- ABI 静态断言：MailboxSyncBlock 布局锁定 ---- */
static_assert(sizeof(MailboxSyncBlock) == 32,
              "MailboxSyncBlock sizeof 不可改变，否则破坏共享内存布局");
static_assert(alignof(MailboxSyncBlock) == 8,
              "MailboxSyncBlock alignof 必须为 8（含 atomic<uint64_t> 字段）");
static_assert(offsetof(MailboxSyncBlock, m_cppHeartbeatCounter) == 16,
              "m_cppHeartbeatCounter offset 必须为 16");
static_assert(offsetof(MailboxSyncBlock, m_pyHeartbeatCounter) == 24,
              "m_pyHeartbeatCounter offset 必须为 24");

/**
 * \brief 共享内存协议头。
 *
 * 所有跨进程读写的字段使用 std::atomic 确保发布性，
 * 无需额外配对 __sync_synchronize() 全屏障。
 */
struct Ns3AiMsgProtocolHeader
{
    // m_magic 默认 0 表示尚未发布。creator 在 InitializeProtocolHeader 末尾
    // release-store 正确值作为门闩；opener 在 ValidateProtocolHeader 中
    // acquire 等待 m_magic != 0 后才读取其他字段。
    std::atomic<uint32_t> m_magic{0};
    std::atomic<uint16_t> m_abiVersion{NS3_AI_MSG_ABI_VERSION};
    std::atomic<uint16_t> m_headerSize{sizeof(Ns3AiMsgProtocolHeader)};
    std::atomic<uint64_t> m_sessionId{0};
    std::atomic<uint64_t> m_generationId{0};
    std::atomic<uint32_t> m_cpp2pyPayloadSize{0};
    std::atomic<uint32_t> m_py2cppPayloadSize{0};
    std::atomic<uint32_t> m_cpp2pySchemaVersion{0};
    std::atomic<uint32_t> m_py2cppSchemaVersion{0};
    std::atomic<uint64_t> m_cpp2pySchemaHash{0};
    std::atomic<uint64_t> m_py2cppSchemaHash{0};
};

/* ---- ABI 静态断言：Ns3AiMsgProtocolHeader 布局锁定 ---- */
static_assert(sizeof(Ns3AiMsgProtocolHeader) == 56,
              "Ns3AiMsgProtocolHeader sizeof 不可改变，否则破坏共享内存布局");
static_assert(alignof(Ns3AiMsgProtocolHeader) == 8,
              "Ns3AiMsgProtocolHeader alignof 必须为 8（含 uint64_t 字段）");

enum class Ns3AiMsgFieldType : uint16_t
{
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    RawBytes
};

struct Ns3AiMsgField
{
    std::string m_name;
    Ns3AiMsgFieldType m_type;
    std::size_t m_offset{0};
    std::size_t m_size{0};
    std::size_t m_count{1};
};

struct Ns3AiMsgSchema
{
    std::string m_name;
    uint32_t m_version{1};
    uint64_t m_schemaHash{0};
    uint32_t m_size{0};
    std::vector<Ns3AiMsgField> m_fields;
};

template <typename PayloadType>
struct Ns3AiMsgTypeSchemaDefaults
{
    static constexpr uint64_t SchemaHash = 0;
    static constexpr uint32_t SchemaVersion = 0;
};

#ifdef NS3_NS3_AI_GYM_MSG_H
template <>
struct Ns3AiMsgTypeSchemaDefaults<::Ns3AiGymMsg>
{
    static constexpr uint64_t SchemaHash = NS3_AI_GYM_MSG_SCHEMA_HASH;
    static constexpr uint32_t SchemaVersion = NS3_AI_GYM_MSG_SCHEMA_VERSION;
};
#endif

inline uint64_t
Ns3AiMsgHashBytes(uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t
Ns3AiMsgHashString(uint64_t hash, const std::string& value)
{
    return Ns3AiMsgHashBytes(hash, value.data(), value.size());
}

inline uint64_t
ComputeNs3AiMsgSchemaHash(const Ns3AiMsgSchema& schema)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = Ns3AiMsgHashString(hash, schema.m_name);
    hash = Ns3AiMsgHashBytes(hash, &schema.m_version, sizeof(schema.m_version));
    hash = Ns3AiMsgHashBytes(hash, &schema.m_size, sizeof(schema.m_size));
    for (const auto& field : schema.m_fields)
    {
        const uint16_t type = static_cast<uint16_t>(field.m_type);
        hash = Ns3AiMsgHashString(hash, field.m_name);
        hash = Ns3AiMsgHashBytes(hash, &type, sizeof(type));
        hash = Ns3AiMsgHashBytes(hash, &field.m_offset, sizeof(field.m_offset));
        hash = Ns3AiMsgHashBytes(hash, &field.m_size, sizeof(field.m_size));
        hash = Ns3AiMsgHashBytes(hash, &field.m_count, sizeof(field.m_count));
    }
    return hash;
}

inline Ns3AiMsgSchema
MakeNs3AiMsgSchema(std::string name,
                   uint32_t version,
                   uint32_t size,
                   std::initializer_list<Ns3AiMsgField> fields)
{
    Ns3AiMsgSchema schema{std::move(name), version, 0, size, std::vector<Ns3AiMsgField>(fields)};
    schema.m_schemaHash = ComputeNs3AiMsgSchemaHash(schema);
    return schema;
}

/**
 * \brief Schema 校验模式。
 */
enum class Ns3AiSchemaValidationMode : uint8_t
{
    Strict = 0,
    Compatibility = 1,
    Disabled = 2
};

inline std::ostream&
operator<<(std::ostream& os, Ns3AiSchemaValidationMode mode)
{
    switch (mode)
    {
    case Ns3AiSchemaValidationMode::Strict:
        return os << "strict";
    case Ns3AiSchemaValidationMode::Compatibility:
        return os << "compatibility";
    case Ns3AiSchemaValidationMode::Disabled:
        return os << "disabled";
    }
    return os << "unknown";
}

struct MailboxTransportNames
{
    std::string m_segmentName;
    std::string m_cpp2pyMsgName;
    std::string m_py2cppMsgName;
    std::string m_lockableName;
    std::string m_headerName{"My Header"};
};

struct MailboxTransportConfig
{
    bool m_isMemoryCreator{false};
    bool m_useVector{false};
    bool m_handleFinish{false};
    uint32_t m_size{4096};
    uint64_t m_syncTimeoutUs{300000000};
    uint64_t m_heartbeatPeriodUs{1000000};    // 默认 1s，心跳发布周期
    uint64_t m_heartbeatTimeoutUs{3000000};   // 默认 3s，对端心跳超时
    MailboxTransportNames m_names{"My Seg",
                                    "My Cpp to Python Msg",
                                    "My Python to Cpp Msg",
                                    "My Lockable",
                                    "My Header"};
    uint64_t m_cpp2pySchemaHash{0};
    uint64_t m_py2cppSchemaHash{0};
    uint32_t m_cpp2pySchemaVersion{0};
    uint32_t m_py2cppSchemaVersion{0};
    uint32_t m_cpp2pySchemaSize{0};
    uint32_t m_py2cppSchemaSize{0};
    Ns3AiSchemaValidationMode m_schemaValidationMode{Ns3AiSchemaValidationMode::Strict};

    MailboxTransportConfig& SetSchemas(const Ns3AiMsgSchema& cpp2pySchema,
                                        const Ns3AiMsgSchema& py2cppSchema)
    {
        m_cpp2pySchemaHash = cpp2pySchema.m_schemaHash;
        m_py2cppSchemaHash = py2cppSchema.m_schemaHash;
        m_cpp2pySchemaVersion = cpp2pySchema.m_version;
        m_py2cppSchemaVersion = py2cppSchema.m_version;
        m_cpp2pySchemaSize = cpp2pySchema.m_size;
        m_py2cppSchemaSize = py2cppSchema.m_size;
        return *this;
    }
};

/**
 * \brief Base class for mailbox transport implementations.
 *
 * Type-erased anchor for heterogeneous mailbox instances held by
 * MailboxTransport's singleton (see m_interfaces map).
 */
class MailboxTransportBase
{
  public:
    virtual ~MailboxTransportBase() = default;
};

/**
 * \brief Single-slot lock-step mailbox between C++ and Python peers.
 *
 * This is the workhorse IPC channel. Each direction (Cpp2Py, Py2Cpp)
 * has exactly one shared-memory slot.  Sender fills it, receiver
 * consumes it, then they swap roles — strict alternation.
 *
 * \tparam Cpp2PyMsgType Struct type for the C++→Python slot.
 * \tparam Py2CppMsgType Struct type for the Python→C++ slot.
 */
template <typename Cpp2PyMsgType, typename Py2CppMsgType>
class MailboxTransportImpl : public MailboxTransportBase
{
  public:
    static constexpr uint64_t DEFAULT_SYNC_TIMEOUT_US = 300000000;

    MailboxTransportImpl() = delete;

    explicit MailboxTransportImpl(bool is_memory_creator,
                                   bool use_vector,
                                   bool handle_finish,
                                   uint32_t size = 4096,
                                   const char* segment_name = "My Seg",
                                   const char* cpp2py_msg_name = "My Cpp to Python Msg",
                                   const char* py2cpp_msg_name = "My Python to Cpp Msg",
                                   const char* lockable_name = "My Lockable",
                                   uint64_t sync_timeout_us = DEFAULT_SYNC_TIMEOUT_US,
                                   const char* header_name = "My Header",
                                   uint64_t cpp2py_schema_hash = 0,
                                   uint64_t py2cpp_schema_hash = 0,
                                   uint32_t cpp2py_schema_version = 0,
                                   uint32_t py2cpp_schema_version = 0,
                                   Ns3AiSchemaValidationMode schema_validation_mode =
                                       Ns3AiSchemaValidationMode::Strict,
                                   uint64_t heartbeat_period_us = 1000000,
                                   uint64_t heartbeat_timeout_us = 3000000)
        : m_cpp2pyStruct(nullptr),
          m_py2CppStruct(nullptr),
          m_cpp2pyVector(nullptr),
          m_py2cppVector(nullptr),
          m_sync(nullptr),
          m_header(nullptr),
          m_isCreator(is_memory_creator),
          m_useVector(use_vector),
          m_handleFinish(handle_finish),
          m_segName(segment_name),
          m_cpp2pyMsgName(cpp2py_msg_name),
          m_py2cppMsgName(py2cpp_msg_name),
          m_lockableName(lockable_name),
          m_headerName(header_name),
          m_syncTimeoutUs(sync_timeout_us),
          m_heartbeatPeriodUs(heartbeat_period_us),
          m_heartbeatTimeoutUs(heartbeat_timeout_us),
          m_cpp2pySchemaHash(cpp2py_schema_hash != 0 ? cpp2py_schema_hash :
                                                     Ns3AiMsgTypeSchemaDefaults<Cpp2PyMsgType>::SchemaHash),
          m_py2cppSchemaHash(py2cpp_schema_hash != 0 ? py2cpp_schema_hash :
                                                     Ns3AiMsgTypeSchemaDefaults<Py2CppMsgType>::SchemaHash),
          m_cpp2pySchemaVersion(cpp2py_schema_version != 0 ? cpp2py_schema_version :
                                                           Ns3AiMsgTypeSchemaDefaults<Cpp2PyMsgType>::SchemaVersion),
          m_py2cppSchemaVersion(py2cpp_schema_version != 0 ? py2cpp_schema_version :
                                                           Ns3AiMsgTypeSchemaDefaults<Py2CppMsgType>::SchemaVersion),
          m_schemaValidationMode(schema_validation_mode),
          m_isFinished(false),
          m_pyRecvHasCpp2PySlot(false)
    {
        // 预检：Strict 模式下 expected metadata 不得为 0
        if (m_schemaValidationMode == Ns3AiSchemaValidationMode::Strict)
        {
            CheckExpectedMetadataNonZero("cpp2py", "schema_hash", m_cpp2pySchemaHash);
            CheckExpectedMetadataNonZero("py2cpp", "schema_hash", m_py2cppSchemaHash);
            CheckExpectedMetadataNonZero("cpp2py", "schema_version", m_cpp2pySchemaVersion);
            CheckExpectedMetadataNonZero("py2cpp", "schema_version", m_py2cppSchemaVersion);
        }
        else if (m_schemaValidationMode == Ns3AiSchemaValidationMode::Compatibility)
        {
            if (m_cpp2pySchemaHash == 0)
            {
                WarnSchemaCompatibilityMetadataMissing("cpp2py", "schema_hash",
                                                        m_cpp2pySchemaHash, m_cpp2pySchemaHash);
            }
            if (m_py2cppSchemaHash == 0)
            {
                WarnSchemaCompatibilityMetadataMissing("py2cpp", "schema_hash",
                                                        m_py2cppSchemaHash, m_py2cppSchemaHash);
            }
            if (m_cpp2pySchemaVersion == 0)
            {
                WarnSchemaCompatibilityMetadataMissing("cpp2py", "schema_version",
                                                        m_cpp2pySchemaVersion, m_cpp2pySchemaVersion);
            }
            if (m_py2cppSchemaVersion == 0)
            {
                WarnSchemaCompatibilityMetadataMissing("py2cpp", "schema_version",
                                                        m_py2cppSchemaVersion, m_py2cppSchemaVersion);
            }
        }
        else
        {
            WarnSchemaValidationDisabled();
        }

        using namespace boost::interprocess;
        if (m_isCreator)
        {
            shared_memory_object::remove(m_segName.c_str());
            m_segment = std::make_unique<managed_shared_memory>(create_only, m_segName.c_str(), size);
            if (m_useVector)
            {
                const Cpp2PyMsgAllocator alloc_env(m_segment->get_segment_manager());
                const Py2CppMsgAllocator alloc_act(m_segment->get_segment_manager());
                m_cpp2pyVector = m_segment->construct<Cpp2PyMsgVector>(cpp2py_msg_name)(alloc_env);
                m_py2cppVector = m_segment->construct<Py2CppMsgVector>(py2cpp_msg_name)(alloc_act);
            }
            else
            {
                m_cpp2pyStruct = m_segment->construct<Cpp2PyMsgType>(cpp2py_msg_name)();
                m_py2CppStruct = m_segment->construct<Py2CppMsgType>(py2cpp_msg_name)();
            }
            m_sync = m_segment->construct<MailboxSyncBlock>(lockable_name)();
            m_header = m_segment->construct<Ns3AiMsgProtocolHeader>(header_name)();
            // 先初始化协议头（m_magic 最后 release-store 作为发布门闩），
            // 再初始化同步状态。opener 在 ValidateProtocolHeader 中
            // acquire 等待 m_magic != 0 后才读取 header 字段。
            InitializeProtocolHeader();
            InitializeSyncState();
        }
        else
        {
            m_segment = std::make_unique<managed_shared_memory>(open_only, segment_name);
            if (m_useVector)
            {
                m_cpp2pyVector = m_segment->find<Cpp2PyMsgVector>(cpp2py_msg_name).first;
                m_py2cppVector = m_segment->find<Py2CppMsgVector>(py2cpp_msg_name).first;
                RequireNonNull(m_cpp2pyVector, m_segName, "cpp2py_msg_vector", cpp2py_msg_name);
                RequireNonNull(m_py2cppVector, m_segName, "py2cpp_msg_vector", py2cpp_msg_name);
            }
            else
            {
                m_cpp2pyStruct = m_segment->find<Cpp2PyMsgType>(cpp2py_msg_name).first;
                m_py2CppStruct = m_segment->find<Py2CppMsgType>(py2cpp_msg_name).first;
                RequireNonNull(m_cpp2pyStruct, m_segName, "cpp2py_msg_struct", cpp2py_msg_name);
                RequireNonNull(m_py2CppStruct, m_segName, "py2cpp_msg_struct", py2cpp_msg_name);
            }
            m_sync = m_segment->find<MailboxSyncBlock>(lockable_name).first;
            m_header = m_segment->find<Ns3AiMsgProtocolHeader>(header_name).first;
            RequireNonNull(m_sync, m_segName, "sync", lockable_name);
            RequireNonNull(m_header, m_segName, "header", header_name);
            ValidateProtocolHeader();
        }

        // 约束校验：心跳参数
        ValidateHeartbeatConfig();
    };

    ~MailboxTransportImpl() override
    {
        if (m_isCreator)
        {
            boost::interprocess::shared_memory_object::remove(m_segName.c_str());
        }
        else if (m_handleFinish && !m_isFinished)
        {
            TryCppSetFinished();
        }
    };

    typedef boost::interprocess::
        allocator<Cpp2PyMsgType, boost::interprocess::managed_shared_memory::segment_manager>
            Cpp2PyMsgAllocator;
    typedef boost::interprocess::vector<Cpp2PyMsgType, Cpp2PyMsgAllocator> Cpp2PyMsgVector;
    typedef boost::interprocess::
        allocator<Py2CppMsgType, boost::interprocess::managed_shared_memory::segment_manager>
            Py2CppMsgAllocator;
    typedef boost::interprocess::vector<Py2CppMsgType, Py2CppMsgAllocator> Py2CppMsgVector;

    Cpp2PyMsgType* GetCpp2PyStruct()
    {
        if (m_useVector)
        {
            throw Ns3AiProtocolError(
                "ns3-ai message interface access mode mismatch in GetCpp2PyStruct: "
                "interface is configured for vector mode, not struct mode");
        }
        return m_cpp2pyStruct;
    };

    Py2CppMsgType* GetPy2CppStruct()
    {
        if (m_useVector)
        {
            throw Ns3AiProtocolError(
                "ns3-ai message interface access mode mismatch in GetPy2CppStruct: "
                "interface is configured for vector mode, not struct mode");
        }
        return m_py2CppStruct;
    };

    Cpp2PyMsgVector* GetCpp2PyVector()
    {
        if (!m_useVector)
        {
            throw Ns3AiProtocolError(
                "ns3-ai message interface access mode mismatch in GetCpp2PyVector: "
                "interface is configured for struct mode, not vector mode");
        }
        return m_cpp2pyVector;
    };

    Py2CppMsgVector* GetPy2CppVector()
    {
        if (!m_useVector)
        {
            throw Ns3AiProtocolError(
                "ns3-ai message interface access mode mismatch in GetPy2CppVector: "
                "interface is configured for struct mode, not vector mode");
        }
        return m_py2cppVector;
    };

    const Ns3AiMsgProtocolHeader* GetProtocolHeader() const
    {
        return m_header;
    };

    uint64_t GetSyncTimeoutUs() const
    {
        return m_syncTimeoutUs;
    };

    Ns3AiMsgPeerState GetCppState() const
    {
        return GetPeerState(TransportPeer::Cpp);
    };

    Ns3AiMsgPeerState GetPyState() const
    {
        return GetPeerState(TransportPeer::Py);
    };

    TransportSessionState GetSessionState() const
    {
        return static_cast<TransportSessionState>(
            m_sync->m_sessionState.load(std::memory_order_acquire));
    };

    uint64_t GetSessionId() const
    {
        return m_header->m_sessionId.load(std::memory_order_acquire);
    };

    uint64_t GetGenerationId() const
    {
        return m_header->m_generationId.load(std::memory_order_acquire);
    };

    TransportCloseReason GetCloseReason() const
    {
        return static_cast<TransportCloseReason>(
            m_sync->m_closeReason.load(std::memory_order_acquire));
    };

    TransportErrorReason GetErrorReason() const
    {
        return static_cast<TransportErrorReason>(
            m_sync->m_errorReason.load(std::memory_order_acquire));
    };

    TransportPeer GetLastErrorPeer() const
    {
        return static_cast<TransportPeer>(
            m_sync->m_lastErrorPeer.load(std::memory_order_acquire));
    };

    void RequestClose(TransportPeer peer, TransportCloseReason reason) const
    {
        if (GetSessionState() == TransportSessionState::Closed)
        {
            if (GetCloseReason() == reason)
            {
                return;
            }
        }
        EnsureSessionStateReadyOrRunning("RequestClose");
        m_sync->m_closeReason.store(static_cast<uint8_t>(reason), std::memory_order_release);
        m_sync->m_closeRequester.store(static_cast<uint8_t>(peer), std::memory_order_release);
        m_sync->m_closeAcknowledger.store(0, std::memory_order_release);
        // 门闩：m_sessionState 的 release store 确保以上所有写入对另一端可见
        m_sync->m_sessionState.store(static_cast<uint8_t>(TransportSessionState::Closing),
                                     std::memory_order_release);
    };

    void AcknowledgeClose(TransportPeer peer) const
    {
        if (GetSessionState() == TransportSessionState::Closed)
        {
            return;
        }
        EnsureSessionState(TransportSessionState::Closing, "AcknowledgeClose");
        if (m_sync->m_closeRequester.load(std::memory_order_acquire) == static_cast<uint8_t>(peer))
        {
            throw std::runtime_error(
                "ns3-ai message interface close acknowledgement must come from the peer that did not request close");
        }
        m_sync->m_closeAcknowledger.store(static_cast<uint8_t>(peer), std::memory_order_release);
        m_sync->m_sessionState.store(static_cast<uint8_t>(TransportSessionState::Closed),
                                     std::memory_order_release);
    };

    void ReportPeerDeath(TransportPeer peer) const
    {
        MarkPeerError(peer, TransportErrorReason::PeerDeath);
    };

    bool CheckGenerationId(uint64_t generationId, TransportPeer peer) const
    {
        if (generationId == GetGenerationId())
        {
            return true;
        }
        MarkPeerError(peer, TransportErrorReason::StaleGeneration);
        return false;
    };

    void CppSendBegin()
    {
        BeginDataExchangeOrThrow("CppSendBegin");
        TransitionPeer(TransportPeer::Cpp,
                       Ns3AiMsgPeerState::Ready,
                       Ns3AiMsgPeerState::Sending,
                       "CppSendBegin");
        WaitOrThrow(&m_sync->m_cpp2pyEmptyCount,
                    "CppSendBegin",
                    "cpp2py empty slot",
                    true,
                    TransportPeer::Cpp);
    };

    bool TryCppSendBegin()
    {
        if (!TryBeginDataExchange())
        {
            return false;
        }
        if (!TryTransitionPeer(TransportPeer::Cpp,
                               Ns3AiMsgPeerState::Ready,
                               Ns3AiMsgPeerState::Sending))
        {
            return false;
        }
        return HandleSyncResult(WaitForSync(&m_sync->m_cpp2pyEmptyCount, true,
                                             TransportPeer::Cpp),
                                TransportPeer::Cpp);
    };

    void CppSendEnd()
    {
        EnsurePeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Sending, "CppSendEnd");
        SemPostWithDiag(&m_sync->m_cpp2pyFullCount, "cpp2pyFullCount");
        SetPeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Ready);
    };

    void CppRecvBegin()
    {
        BeginDataExchangeOrThrow("CppRecvBegin");
        TransitionPeer(TransportPeer::Cpp,
                       Ns3AiMsgPeerState::Ready,
                       Ns3AiMsgPeerState::Receiving,
                       "CppRecvBegin");
        WaitOrThrow(&m_sync->m_py2cppFullCount,
                    "CppRecvBegin",
                    "py2cpp full slot",
                    true,
                    TransportPeer::Cpp);
    };

    bool TryCppRecvBegin()
    {
        if (!TryBeginDataExchange())
        {
            return false;
        }
        if (!TryTransitionPeer(TransportPeer::Cpp,
                               Ns3AiMsgPeerState::Ready,
                               Ns3AiMsgPeerState::Receiving))
        {
            return false;
        }
        return HandleSyncResult(WaitForSync(&m_sync->m_py2cppFullCount, true,
                                             TransportPeer::Cpp),
                                TransportPeer::Cpp);
    };

    void CppRecvEnd()
    {
        EnsurePeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Receiving, "CppRecvEnd");
        SemPostWithDiag(&m_sync->m_py2cppEmptyCount, "py2cppEmptyCount");
        SetPeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Ready);
    };

    void CppSetFinished()
    {
        RequireFinishConfigured("CppSetFinished");
        TryCppSetFinished();
    };

    bool TryCppSetFinished()
    {
        RequireFinishConfigured("TryCppSetFinished");
        if (m_isFinished || m_sync->m_isFinished.load(std::memory_order_acquire))
        {
            m_isFinished = true;
            SetPeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Finished);
            return true;
        }
        m_isFinished = true;
        m_sync->m_isFinished.store(true, std::memory_order_release);
        SetPeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Finished);
        return true;
    };

    void PyRecvBegin()
    {
        BeginDataExchangeOrThrow("PyRecvBegin");
        if (!TryPyRecvBeginAfterSessionReady())
        {
            ThrowSyncFailure("PyRecvBegin", "cpp2py full slot or finish flag", TransportPeer::Py, nullptr);
        }
    };

    bool TryPyRecvBegin()
    {
        if (!TryBeginDataExchange())
        {
            return false;
        }
        return TryPyRecvBeginAfterSessionReady();
    };

    void PyRecvEnd()
    {
        if (m_pyRecvHasCpp2PySlot)
        {
            EnsurePeerState(TransportPeer::Py, Ns3AiMsgPeerState::Receiving, "PyRecvEnd");
            SemPostWithDiag(&m_sync->m_cpp2pyEmptyCount, "cpp2pyEmptyCount");
            m_pyRecvHasCpp2PySlot = false;
            SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Ready);
        }
    };

    void PySendBegin()
    {
        BeginDataExchangeOrThrow("PySendBegin");
        TransitionPeer(TransportPeer::Py,
                       Ns3AiMsgPeerState::Ready,
                       Ns3AiMsgPeerState::Sending,
                       "PySendBegin");
        WaitOrThrow(&m_sync->m_py2cppEmptyCount,
                    "PySendBegin",
                    "py2cpp empty slot",
                    true,
                    TransportPeer::Py);
    };

    bool TryPySendBegin()
    {
        if (!TryBeginDataExchange())
        {
            return false;
        }
        if (!TryTransitionPeer(TransportPeer::Py,
                               Ns3AiMsgPeerState::Ready,
                               Ns3AiMsgPeerState::Sending))
        {
            return false;
        }
        const auto result = WaitForSync(&m_sync->m_py2cppEmptyCount, true,
                                         TransportPeer::Py);
        if (result == Ns3AiSemaphore::WaitResult::Acquired)
        {
            return true;
        }
        if (result == Ns3AiSemaphore::WaitResult::Aborted)
        {
            // Error state takes priority — heartbeat detection may have set PeerDeath
            if (GetSessionState() == TransportSessionState::Error)
            {
                return false;
            }
            if (m_handleFinish)
            {
                m_isFinished = m_sync->m_isFinished.load(std::memory_order_acquire);
            }
            SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Finished);
            return false;
        }
        if (m_handleFinish)
        {
            m_isFinished = m_sync->m_isFinished.load(std::memory_order_acquire);
        }
        MarkPeerError(TransportPeer::Py, TransportErrorReason::Timeout);
        return false;
    };

    void PySendEnd()
    {
        EnsurePeerState(TransportPeer::Py, Ns3AiMsgPeerState::Sending, "PySendEnd");
        SemPostWithDiag(&m_sync->m_py2cppFullCount, "py2cppFullCount");
        SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Ready);
    };

    bool PyGetFinished()
    {
        RequireFinishConfigured("PyGetFinished");
        m_isFinished = m_sync->m_isFinished.load(std::memory_order_acquire);
        if (m_isFinished)
        {
            SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Finished);
        }
        return m_isFinished;
    };

  private:
    bool TryPyRecvBeginAfterSessionReady()
    {
        m_pyRecvHasCpp2PySlot = false;
        if (!TryTransitionPeer(TransportPeer::Py,
                               Ns3AiMsgPeerState::Ready,
                               Ns3AiMsgPeerState::Receiving))
        {
            return false;
        }
        if (!WaitForCpp2PyDataOrFinish())
        {
            MarkPeerError(TransportPeer::Py, TransportErrorReason::Timeout);
            return false;
        }
        if (m_handleFinish)
        {
            m_isFinished = m_sync->m_isFinished.load(std::memory_order_acquire);
        }
        if (m_isFinished && !m_pyRecvHasCpp2PySlot)
        {
            SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Finished);
        }
        return true;
    };

    static uint64_t HashNamePart(uint64_t hash, const std::string& value)
    {
        return Ns3AiMsgHashString(hash, value);
    };

    static uint64_t MakeSessionId(const std::string& segmentName,
                                  const std::string& cpp2pyMsgName,
                                  const std::string& py2cppMsgName,
                                  const std::string& lockableName,
                                  const std::string& headerName)
    {
        uint64_t hash = 1469598103934665603ULL;
        hash = HashNamePart(hash, segmentName);
        hash = HashNamePart(hash, cpp2pyMsgName);
        hash = HashNamePart(hash, py2cppMsgName);
        hash = HashNamePart(hash, lockableName);
        hash = HashNamePart(hash, headerName);
        return hash | 1ULL;
    };

    static uint64_t MakeGenerationId()
    {
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) |
               1ULL;
    };

    static const char* PeerName(TransportPeer peer)
    {
        return peer == TransportPeer::Cpp ? "C++" : "Python";
    };

    static const char* StateName(Ns3AiMsgPeerState state)
    {
        switch (state)
        {
        case Ns3AiMsgPeerState::Initializing:
            return "initializing";
        case Ns3AiMsgPeerState::Ready:
            return "ready";
        case Ns3AiMsgPeerState::Sending:
            return "sending";
        case Ns3AiMsgPeerState::Receiving:
            return "receiving";
        case Ns3AiMsgPeerState::Finished:
            return "finished";
        case Ns3AiMsgPeerState::Error:
            return "error";
        }
        return "unknown";
    };

    std::atomic<uint8_t>* PeerStatePtr(TransportPeer peer) const
    {
        return (peer == TransportPeer::Cpp) ? &m_sync->m_cppState : &m_sync->m_pyState;
    };

    Ns3AiMsgPeerState GetPeerState(TransportPeer peer) const
    {
        return static_cast<Ns3AiMsgPeerState>(
            PeerStatePtr(peer)->load(std::memory_order_acquire));
    };

    void SetPeerState(TransportPeer peer, Ns3AiMsgPeerState state) const
    {
        PeerStatePtr(peer)->store(static_cast<uint8_t>(state), std::memory_order_release);
    };

    void InitializeSyncState() const
    {
        m_sync->m_sessionState.store(static_cast<uint8_t>(TransportSessionState::Init),
                                     std::memory_order_release);
        m_sync->m_closeReason.store(static_cast<uint8_t>(TransportCloseReason::None),
                                    std::memory_order_release);
        m_sync->m_closeRequester.store(0, std::memory_order_release);
        m_sync->m_closeAcknowledger.store(0, std::memory_order_release);
        m_sync->m_errorReason.store(static_cast<uint8_t>(TransportErrorReason::None),
                                    std::memory_order_release);
        SetPeerState(TransportPeer::Cpp, Ns3AiMsgPeerState::Ready);
        SetPeerState(TransportPeer::Py, Ns3AiMsgPeerState::Ready);
        m_sync->m_lastErrorPeer.store(0, std::memory_order_release);
        m_sync->m_lastErrorCode.store(0, std::memory_order_release);
        m_sync->m_cppHeartbeatCounter.store(0, std::memory_order_release);
        m_sync->m_pyHeartbeatCounter.store(0, std::memory_order_release);
    };

    bool TryTransitionPeer(TransportPeer peer,
                           Ns3AiMsgPeerState expected,
                           Ns3AiMsgPeerState next) const
    {
        std::atomic<uint8_t>* state = PeerStatePtr(peer);
        uint8_t current = static_cast<uint8_t>(expected);
        return state->compare_exchange_strong(current,
                                              static_cast<uint8_t>(next),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    };

    void TransitionPeer(TransportPeer peer,
                        Ns3AiMsgPeerState expected,
                        Ns3AiMsgPeerState next,
                        const char* operation) const
    {
        if (!TryTransitionPeer(peer, expected, next))
        {
            ThrowInvalidState(peer, operation, expected, GetPeerState(peer));
        }
    };

    void EnsurePeerState(TransportPeer peer, Ns3AiMsgPeerState expected, const char* operation) const
    {
        const Ns3AiMsgPeerState actual = GetPeerState(peer);
        if (actual != expected)
        {
            ThrowInvalidState(peer, operation, expected, actual);
        }
    };

    void EnsureSessionState(TransportSessionState expected, const char* operation) const
    {
        const auto actual = GetSessionState();
        if (actual != expected)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface invalid session state in " << operation << ": expected "
                << expected << ", got " << actual << ".";
            throw Ns3AiProtocolError(oss.str());
        }
    };

    bool TryBeginDataExchange() const
    {
        const auto actual = GetSessionState();
        if (actual == TransportSessionState::Running)
        {
            return true;
        }
        if (actual != TransportSessionState::Ready)
        {
            return false;
        }

        // Running 是粘性状态：任意一次有效数据交换开始后，会话保持 RUNNING，
        // 直到显式关闭握手或结构化错误改变生命周期状态。
        uint8_t expected = static_cast<uint8_t>(TransportSessionState::Ready);
        return m_sync->m_sessionState.compare_exchange_strong(
                   expected,
                   static_cast<uint8_t>(TransportSessionState::Running),
                   std::memory_order_acq_rel,
                   std::memory_order_acquire) ||
               expected == static_cast<uint8_t>(TransportSessionState::Running);
    };

    void BeginDataExchangeOrThrow(const char* operation) const
    {
        if (!TryBeginDataExchange())
        {
            EnsureSessionStateReadyOrRunning(operation);
        }
    };

    void EnsureSessionStateReadyOrRunning(const char* operation) const
    {
        const auto actual = GetSessionState();
        if (actual != TransportSessionState::Ready && actual != TransportSessionState::Running)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface invalid session state in " << operation
                << ": expected ready or running, got " << actual << ".";
            throw Ns3AiProtocolError(oss.str());
        }
    };

    void MarkPeerError(TransportPeer peer, TransportErrorReason reason) const
    {
        SetPeerState(peer, Ns3AiMsgPeerState::Error);
        m_sync->m_lastErrorPeer.store(static_cast<uint8_t>(peer), std::memory_order_release);
        m_sync->m_lastErrorCode.store(static_cast<uint8_t>(reason), std::memory_order_release);
        m_sync->m_errorReason.store(static_cast<uint8_t>(reason), std::memory_order_release);
        // 门闩：m_sessionState 的 release store 确保以上所有写入对另一端可见
        m_sync->m_sessionState.store(static_cast<uint8_t>(TransportSessionState::Error),
                                     std::memory_order_release);
    };

    [[noreturn]] void ThrowInvalidState(TransportPeer peer,
                                        const char* operation,
                                        Ns3AiMsgPeerState expected,
                                        Ns3AiMsgPeerState actual) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface invalid synchronization state in " << operation << " for "
            << PeerName(peer) << " peer: expected " << StateName(expected) << ", got "
            << StateName(actual) << ". Check send/receive ordering and avoid reusing a shared-memory "
            << "segment after an abnormal peer exit without recreating the session.";
        throw Ns3AiProtocolError(oss.str());
    };

    Ns3AiSemaphore::WaitResult WaitForSync(std::atomic<uint8_t>* counter,
                                            bool abortOnFinished,
                                            TransportPeer waitingPeer)
    {
        const auto hbStart = std::chrono::steady_clock::now();
        auto lastHbPublish = hbStart;
        uint32_t attempts = 0;

        while (true)
        {
            // 1. Publish self (waitingPeer) heartbeat counter at configured period.
            //    仅当距上次发布 >= m_heartbeatPeriodUs 时才写，防止高频自旋空写。
            if (m_heartbeatPeriodUs > 0)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - lastHbPublish);
                if (static_cast<uint64_t>(elapsed.count()) >= m_heartbeatPeriodUs)
                {
                    PublishSelfHeartbeat(waitingPeer);
                    lastHbPublish = now;
                }
            }

            // 2. Try acquire semaphore
            if (Ns3AiSemaphore::sem_try_wait(counter))
            {
                return Ns3AiSemaphore::WaitResult::Acquired;
            }

            // 3. Check peer heartbeat — 仅 C++ 作为等待方时检测 Python 心跳。
            //    Python 等待时不检测 C++ 心跳（C++ 不在 WaitForSync 中时也递增，
            //    无法区分 "正在计算" 和 "进程死亡"）。
            if (waitingPeer == TransportPeer::Cpp && !CheckPeerHeartbeat(waitingPeer))
            {
                return Ns3AiSemaphore::WaitResult::Aborted;
            }

            // 4. Check if session already in Error (set by CheckPeerHeartbeat or other path)
            if (GetSessionState() == TransportSessionState::Error)
            {
                return Ns3AiSemaphore::WaitResult::Aborted;
            }

            // 5. Check abort on peer finish
            if (abortOnFinished && m_handleFinish &&
                m_sync->m_isFinished.load(std::memory_order_acquire))
            {
                return Ns3AiSemaphore::WaitResult::Aborted;
            }

            // 6. Check overall sync timeout
            if (m_syncTimeoutUs > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - hbStart);
                if (static_cast<uint64_t>(elapsed.count()) >= m_syncTimeoutUs)
                {
                    return Ns3AiSemaphore::WaitResult::Timeout;
                }
            }

            Ns3AiSemaphore::Backoff(attempts++);
        }
    };

    /**
     * 发布等待方（waitingPeer）自身的心跳计数器。
     * C++ wait → 发布 m_cppHeartbeatCounter；Python wait → 发布 m_pyHeartbeatCounter。
     */
    void PublishSelfHeartbeat(TransportPeer waitingPeer) const
    {
        if (waitingPeer == TransportPeer::Cpp)
        {
            m_sync->m_cppHeartbeatCounter.fetch_add(1, std::memory_order_release);
        }
        else
        {
            m_sync->m_pyHeartbeatCounter.fetch_add(1, std::memory_order_release);
        }
    };

    /**
     * 检查 Python 对端心跳计数器是否停滞（单向检测）。
     *
     * 当前设计仅支持 C++ 作为等待方时检测 Python 侧心跳，不支持
     * 反向（Python 检测 C++）。原因：C++ 不在 WaitForSync 中时
     * 不发布心跳，无法区分 "正在计算" 和 "进程死亡"。
     *
     * \return true 表示 Python 对端活跃或心跳已禁用；
     *         false 表示检测到 Python 死亡并已标记 PeerDeath。
     */
    bool CheckPeerHeartbeat(TransportPeer waitingPeer)
    {
        (void)waitingPeer;
        if (m_heartbeatTimeoutUs == 0)
        {
            return true;
        }

        // 始终检查 Python 对端计数器 m_pyHeartbeatCounter，归因 Py
        const uint64_t peerCnt = m_sync->m_pyHeartbeatCounter.load(std::memory_order_acquire);
        const auto now = std::chrono::steady_clock::now();

        if (!m_cppObservingPy)
        {
            m_cppObservedPyCount = peerCnt;
            m_cppObservedPyTime = now;
            m_cppObservingPy = true;
            return true;
        }

        if (peerCnt == m_cppObservedPyCount)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                now - m_cppObservedPyTime);
            if (static_cast<uint64_t>(elapsed.count()) >= m_heartbeatTimeoutUs)
            {
                MarkPeerError(TransportPeer::Py, TransportErrorReason::PeerDeath);
                return false;
            }
        }
        else
        {
            m_cppObservedPyCount = peerCnt;
            m_cppObservedPyTime = now;
        }
        return true;
    };

  public:
    /**
     * Python daemon 后台线程调用此方法以发布 Python 侧心跳。
     * 纯原子 fetch_add(1, release)，不读对端、不写 error、不改 session state。
     */
    void HeartbeatPublish() const
    {
        m_sync->m_pyHeartbeatCounter.fetch_add(1, std::memory_order_release);
    };

  private:
    /**
     * 替换原有的自建循环 + Backoff 调用，统一走 WaitForSync（带心跳 + Error 感知）。
     */
    bool WaitForCpp2PyDataOrFinish()
    {
        const auto result = WaitForSync(&m_sync->m_cpp2pyFullCount, m_handleFinish,
                                          TransportPeer::Py);

        if (result == Ns3AiSemaphore::WaitResult::Acquired)
        {
            m_pyRecvHasCpp2PySlot = true;
            return true;
        }
        if (m_handleFinish)
        {
            m_isFinished = m_sync->m_isFinished.load(std::memory_order_acquire);
        }
        m_pyRecvHasCpp2PySlot = false;
        if (result == Ns3AiSemaphore::WaitResult::Aborted)
        {
            if (GetSessionState() == TransportSessionState::Error)
            {
                return false;
            }
            return true;
        }
        // Timeout
        return false;
    };

    void RequireFinishConfigured(const char* operation) const
    {
        if (!m_handleFinish)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface finish operations are not configured: "
                << "set handle_finish=true before using " << operation << ".";
            throw Ns3AiRuntimeError(oss.str());
        }
    };

    template <typename T>
    void RequireNonNull(const T* ptr,
                        const std::string& segmentName,
                        const char* objectKind,
                        const char* objectName) const
    {
        if (!ptr)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface missing shared-memory object in segment '"
                << segmentName << "': object='" << objectName << "', kind='" << objectKind
                << "'. Ensure the creator peer has created this object "
                << "and the segment/object names match.";
            throw Ns3AiRuntimeError(oss.str());
        }
    };

    void InitializeProtocolHeader()
    {
        // 先写入所有 header 字段（release store），最后写入 m_magic 作为发布门闩。
        m_header->m_abiVersion.store(NS3_AI_MSG_ABI_VERSION, std::memory_order_release);
        m_header->m_headerSize.store(sizeof(Ns3AiMsgProtocolHeader), std::memory_order_release);
        m_header->m_sessionId.store(
            MakeSessionId(m_segName, m_cpp2pyMsgName, m_py2cppMsgName, m_lockableName, m_headerName),
            std::memory_order_release);
        m_header->m_generationId.store(MakeGenerationId(), std::memory_order_release);
        m_header->m_cpp2pyPayloadSize.store(sizeof(Cpp2PyMsgType), std::memory_order_release);
        m_header->m_py2cppPayloadSize.store(sizeof(Py2CppMsgType), std::memory_order_release);
        m_header->m_cpp2pySchemaHash.store(m_cpp2pySchemaHash, std::memory_order_release);
        m_header->m_py2cppSchemaHash.store(m_py2cppSchemaHash, std::memory_order_release);
        m_header->m_cpp2pySchemaVersion.store(m_cpp2pySchemaVersion, std::memory_order_release);
        m_header->m_py2cppSchemaVersion.store(m_py2cppSchemaVersion, std::memory_order_release);
        // 门闩：m_magic release-store 确保以上所有写入对 opener 可见
        m_header->m_magic.store(NS3_AI_MSG_HEADER_MAGIC, std::memory_order_release);
    };

    /**
     * 检查 expected metadata 非零，若为零则抛 Ns3AiSchemaError。
     */
    void CheckExpectedMetadataNonZero(const char* direction,
                                       const char* field,
                                       uint64_t value) const
    {
        if (value == 0)
        {
            ThrowMissingExpectedMetadata(direction, field);
        }
    };

    /** CheckExpectedMetadataNonZero uint32_t 重载 */
    void CheckExpectedMetadataNonZero(const char* direction,
                                       const char* field,
                                       uint32_t value) const
    {
        if (value == 0)
        {
            ThrowMissingExpectedMetadata(direction, field);
        }
    };

    [[noreturn]] void ThrowMissingExpectedMetadata(const char* direction,
                                                    const char* field) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface schema validation failed:\n"
            << "direction=" << direction << "\n"
            << "field=" << field << "\n"
            << "issue=missing_expected\n"
            << "expected=(non-zero)\n"
            << "actual=(not published)\n"
            << "mode=strict\n"
            << "header=" << m_headerName << "\n"
            << "segment=" << m_segName << ".";
        // 不能在此调用 MarkPeerError：可能在构造预检阶段被调用，此时 m_sync 为 nullptr
        throw Ns3AiSchemaError(oss.str());
    };

    [[noreturn]] void ThrowMissingActualMetadata(const char* direction,
                                                  const char* field) const
    {
        MarkPeerError(TransportPeer::Py, TransportErrorReason::ProtocolMismatch);
        std::ostringstream oss;
        oss << "ns3-ai message interface schema validation failed:\n"
            << "direction=" << direction << "\n"
            << "field=" << field << "\n"
            << "issue=missing_actual\n"
            << "expected=(non-zero)\n"
            << "actual=0x0\n"
            << "mode=strict\n"
            << "header=" << m_headerName << "\n"
            << "segment=" << m_segName << ".";
        throw Ns3AiSchemaError(oss.str());
    };

    /**
     * Compatibility 模式下一侧或两侧 metadata 缺失时输出 deprecation warning。
     */
    void WarnSchemaCompatibilityMetadataMissing(const char* direction,
                                                  const char* field,
                                                  uint64_t expected,
                                                  uint64_t actual) const
    {
        if (expected == 0)
        {
            std::cerr << "ns3-ai [" << m_headerName << "] " << direction << " " << field
                      << " expected is 0 (deprecated: missing schema metadata on this side). "
                      << "Set schema_validation_mode='strict' after regenerating bindings."
                      << std::endl;
        }
        if (actual == 0)
        {
            std::cerr << "ns3-ai [" << m_headerName << "] " << direction << " " << field
                      << " actual is 0 (deprecated: missing schema metadata on peer side). "
                      << "Set schema_validation_mode='strict' after regenerating bindings."
                      << std::endl;
        }
    };

    /**
     * Disabled 模式输出 schema 校验已禁用的 visible warning。
     */
    void WarnSchemaValidationDisabled() const
    {
        std::cerr << "ns3-ai [" << m_headerName << "] "
                  << "schema validation is disabled. Layout drift may corrupt shared memory."
                  << std::endl;
    };

    /**
     * 等待 creator 完成 InitializeProtocolHeader 的发布门闩。
     *
     * m_magic 默认 0，creator 在所有 header 字段写入完成后最后一步
     * release-store 正确值。opener 在此 acquire 等待 m_magic != 0，
     * 确保后续读取所有 header 字段时已看到完成状态。
     *
     * 若 m_syncTimeoutUs > 0，超时后标记 Error 并抛异常。
     * 若 m_syncTimeoutUs == 0，无限期等待。
     */
    void WaitForHeaderPublished() const
    {
        const auto start = std::chrono::steady_clock::now();
        uint32_t attempts = 0;

        while (m_header->m_magic.load(std::memory_order_acquire) == 0)
        {
            if (m_syncTimeoutUs > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
                if (static_cast<uint64_t>(elapsed.count()) >= m_syncTimeoutUs)
                {
                    MarkPeerError(TransportPeer::Py, TransportErrorReason::Timeout);
                    std::ostringstream oss;
                    oss << "ns3-ai message interface timed out waiting for protocol header "
                           "publication after "
                        << m_syncTimeoutUs << " us. The creator may have crashed before "
                                               "initializing the shared-memory protocol header.";
                    throw Ns3AiTimeoutError(oss.str());
                }
            }
            Ns3AiSemaphore::Backoff(attempts++);
        }
    };

    void ValidateProtocolHeader() const
    {
        if (m_header == nullptr)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface could not find protocol header '" << m_headerName << "'.";
            throw Ns3AiRuntimeError(oss.str());
        }

        WaitForHeaderPublished();

        // Phase 1: 基础 header layout 校验。
        // m_sync layout 在此阶段尚不可信（ABI version / header size mismatch 意味着
        // 对端可能使用了不同的 struct 定义），因此不调用 MarkPeerError 写入 m_sync。
        const auto actualMagic = m_header->m_magic.load(std::memory_order_acquire);
        if (actualMagic != NS3_AI_MSG_HEADER_MAGIC)
        {
            ThrowMagicMismatch(NS3_AI_MSG_HEADER_MAGIC, actualMagic);
        }
        const auto actualAbi = m_header->m_abiVersion.load(std::memory_order_acquire);
        if (actualAbi != NS3_AI_MSG_ABI_VERSION)
        {
            ThrowAbiVersionMismatch(NS3_AI_MSG_ABI_VERSION, actualAbi);
        }
        const auto actualHeaderSize = m_header->m_headerSize.load(std::memory_order_acquire);
        if (actualHeaderSize != sizeof(Ns3AiMsgProtocolHeader))
        {
            ThrowHeaderSizeMismatch(sizeof(Ns3AiMsgProtocolHeader), actualHeaderSize);
        }

        // Phase 2: payload size 校验——至此 header layout 已验证兼容，可安全访问 m_sync
        if (m_header->m_cpp2pyPayloadSize.load(std::memory_order_acquire) != sizeof(Cpp2PyMsgType) ||
            m_header->m_py2cppPayloadSize.load(std::memory_order_acquire) != sizeof(Py2CppMsgType))
        {
            MarkPeerError(m_isCreator ? TransportPeer::Py : TransportPeer::Cpp,
                          TransportErrorReason::ProtocolMismatch);
            std::ostringstream oss;
            oss << "ns3-ai message interface payload size mismatch for '" << m_headerName
                << "': Cpp2PyType=" << sizeof(Cpp2PyMsgType)
                << " vs " << m_header->m_cpp2pyPayloadSize.load(std::memory_order_acquire)
                << ", Py2CppType=" << sizeof(Py2CppMsgType)
                << " vs " << m_header->m_py2cppPayloadSize.load(std::memory_order_acquire)
                << ". Both peers must use the same message struct definitions.";
            throw Ns3AiProtocolError(oss.str());
        }

        ValidateSchemaField("cpp2py", "schema_hash",
                            m_cpp2pySchemaHash,
                            m_header->m_cpp2pySchemaHash);
        ValidateSchemaField("py2cpp", "schema_hash",
                            m_py2cppSchemaHash,
                            m_header->m_py2cppSchemaHash);
        ValidateSchemaField("cpp2py", "schema_version",
                            m_cpp2pySchemaVersion,
                            m_header->m_cpp2pySchemaVersion);
        ValidateSchemaField("py2cpp", "schema_version",
                            m_py2cppSchemaVersion,
                            m_header->m_py2cppSchemaVersion);

        // 门闩：m_sessionState = Ready 的 release store 确保 creator 看到 opener 已校验通过
        m_sync->m_sessionState.store(static_cast<uint8_t>(TransportSessionState::Ready),
                                     std::memory_order_release);
    };

    /**
     * 根据 m_schemaValidationMode 执行单一 schema 字段的校验。
     */
    void ValidateSchemaField(const char* direction,
                              const char* field,
                              uint64_t expected,
                              const std::atomic<uint64_t>& actualAtomic) const
    {
        const uint64_t actual = actualAtomic.load(std::memory_order_acquire);
        switch (m_schemaValidationMode)
        {
        case Ns3AiSchemaValidationMode::Strict:
            if (expected == 0)
            {
                ThrowMissingExpectedMetadata(direction, field);
            }
            if (actual == 0)
            {
                ThrowMissingActualMetadata(direction, field);
            }
            if (expected != actual)
            {
                ThrowSchemaHeaderFailure(direction, field, expected, actual);
            }
            break;
        case Ns3AiSchemaValidationMode::Compatibility:
            if (expected == 0 || actual == 0)
            {
                WarnSchemaCompatibilityMetadataMissing(direction, field, expected, actual);
            }
            else if (expected != actual)
            {
                ThrowSchemaHeaderFailure(direction, field, expected, actual);
            }
            break;
        case Ns3AiSchemaValidationMode::Disabled:
            break;
        }
    };

    /** uint32_t 重载 */
    void ValidateSchemaField(const char* direction,
                              const char* field,
                              uint32_t expected,
                              const std::atomic<uint32_t>& actualAtomic) const
    {
        const uint32_t actual = actualAtomic.load(std::memory_order_acquire);
        switch (m_schemaValidationMode)
        {
        case Ns3AiSchemaValidationMode::Strict:
            if (expected == 0)
            {
                ThrowMissingExpectedMetadata(direction, field);
            }
            if (actual == 0)
            {
                ThrowMissingActualMetadata(direction, field);
            }
            if (expected != actual)
            {
                ThrowSchemaHeaderFailure(direction, field, expected, actual);
            }
            break;
        case Ns3AiSchemaValidationMode::Compatibility:
            if (expected == 0 || actual == 0)
            {
                WarnSchemaCompatibilityMetadataMissing(direction, field, expected, actual);
            }
            else if (expected != actual)
            {
                ThrowSchemaHeaderFailure(direction, field, expected, actual);
            }
            break;
        case Ns3AiSchemaValidationMode::Disabled:
            break;
        }
    };

    /** Phase 1 header 校验专用 throw：不调用 MarkPeerError（m_sync layout 尚不可信）。 */
    [[noreturn]] void ThrowMagicMismatch(uint32_t expected, uint32_t actual) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface header magic mismatch for '" << m_headerName
            << "': expected=0x" << std::hex << expected << " actual=0x" << actual << std::dec
            << ". The shared-memory segment may belong to a different ns3-ai version "
               "or may not be an ns3-ai segment.";
        throw Ns3AiProtocolError(oss.str());
    };

    /** Phase 1 header 校验专用 throw：不调用 MarkPeerError（m_sync layout 尚不可信）。 */
    [[noreturn]] void ThrowAbiVersionMismatch(uint16_t expected, uint16_t actual) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface ABI version mismatch for '" << m_headerName
            << "': expected=" << expected << " actual=" << actual
            << ". Both peers must use the same ns3-ai build.";
        throw Ns3AiProtocolError(oss.str());
    };

    /** Phase 1 header 校验专用 throw：不调用 MarkPeerError（m_sync layout 尚不可信）。 */
    [[noreturn]] void ThrowHeaderSizeMismatch(uint16_t expected, uint16_t actual) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface protocol header size mismatch for '" << m_headerName
            << "': expected=" << expected << " actual=" << actual
            << ". Both peers must use the same ns3-ai build.";
        throw Ns3AiProtocolError(oss.str());
    };

    [[noreturn]] void ThrowSchemaHeaderFailure(const char* direction,
                                                const char* field,
                                                uint64_t expected,
                                                uint64_t actual) const
    {
        // 注意：schema mismatch 可能是本地配置错误而非对端问题，因此不按 m_isCreator
        // 归咎对端。当前固定归咎 Py，待 #59 明确语义后重新评估。
        MarkPeerError(TransportPeer::Py, TransportErrorReason::ProtocolMismatch);
        std::ostringstream oss;
        oss << "ns3-ai message interface schema validation failed:\n"
            << "direction=" << direction << "\n"
            << "field=" << field << "\n"
            << "expected=0x" << std::hex << expected << std::dec << "\n"
            << "actual=0x" << std::hex << actual << std::dec << "\n"
            << "mode=" << m_schemaValidationMode << "\n"
            << "header=" << m_headerName << "\n"
            << "segment=" << m_segName << ".";
        throw Ns3AiSchemaError(oss.str());
    };

    /** ThrowSchemaHeaderFailure uint32_t 重载 */
    [[noreturn]] void ThrowSchemaHeaderFailure(const char* direction,
                                                const char* field,
                                                uint32_t expected,
                                                uint32_t actual) const
    {
        ThrowSchemaHeaderFailure(direction, field,
                                 static_cast<uint64_t>(expected),
                                 static_cast<uint64_t>(actual));
    };

    /**
     * 统一处理 Try* 方法的同步结果：
     * Acquired→true, Aborted(Error)→false(保留 error reason), Aborted(Finished)→false,
     * Timeout→MarkPeerError+false。
     */
    bool HandleSyncResult(Ns3AiSemaphore::WaitResult result, TransportPeer peer)
    {
        if (result == Ns3AiSemaphore::WaitResult::Acquired)
        {
            return true;
        }
        if (result == Ns3AiSemaphore::WaitResult::Aborted)
        {
            // Error state takes priority — preserve error reason from heartbeat detection
            if (GetSessionState() != TransportSessionState::Error)
            {
                SetPeerState(peer, Ns3AiMsgPeerState::Finished);
            }
            return false;
        }
        MarkPeerError(peer, TransportErrorReason::Timeout);
        return false;
    };

    void WaitOrThrow(std::atomic<uint8_t>* counter,
                     const char* operation,
                     const char* waitTarget,
                     bool abortOnFinished,
                     TransportPeer peer)
    {
        const auto result = WaitForSync(counter, abortOnFinished, peer);
        if (result == Ns3AiSemaphore::WaitResult::Acquired)
        {
            return;
        }
        if (result == Ns3AiSemaphore::WaitResult::Aborted)
        {
            // Error state takes priority — propagate error reason from heartbeat detection
            if (GetSessionState() == TransportSessionState::Error)
            {
                ThrowSyncFailure(operation, waitTarget, peer, counter);
            }
            // blocking API 被对端 finish 中止：Begin 已切换 peer state 但未拿到资源，
            // 必须抛异常避免调用者误以为成功。
            SetPeerState(peer, Ns3AiMsgPeerState::Finished);
            std::ostringstream oss;
            oss << "ns3-ai message interface aborted in " << operation
                << " for " << PeerName(peer) << " peer: the peer has finished. "
                << "Current states: C++=" << StateName(GetPeerState(TransportPeer::Cpp))
                << ", Python=" << StateName(GetPeerState(TransportPeer::Py)) << ".";
            throw Ns3AiProtocolError(oss.str());
        }
        MarkPeerError(peer, TransportErrorReason::Timeout);
        ThrowSyncFailure(operation, waitTarget, peer, counter);
    };

    [[noreturn]] void ThrowSyncFailure(const char* operation,
                                       const char* waitTarget,
                                       TransportPeer peer,
                                       const std::atomic<uint8_t>* counter) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface synchronization failed in " << operation
            << " for " << PeerName(peer) << " peer while waiting for " << waitTarget << ". ";
        if (counter != nullptr)
        {
            oss << "counter=" << +counter->load(std::memory_order_relaxed) << ", ";
        }
        oss << "timeout=" << m_syncTimeoutUs << "us. ";
        if (m_handleFinish && m_sync->m_isFinished.load(std::memory_order_acquire))
        {
            oss << "The peer has already marked the shared session as finished. ";
        }
        oss << "Current states: C++=" << StateName(GetPeerState(TransportPeer::Cpp))
            << ", Python=" << StateName(GetPeerState(TransportPeer::Py)) << ". "
            << "Check that C++ and Python send/recv calls are paired in the same order. "
            << "Increase the timeout with MailboxTransport::SetSyncTimeoutUs(), "
            << "or set it to 0 to restore unbounded waiting for intentionally long inference.";
        const auto reason = GetErrorReason();
        if (reason == TransportErrorReason::PeerDeath)
        {
            throw Ns3AiProtocolError(oss.str());
        }
        throw Ns3AiTimeoutError(oss.str());
    };

    /**
     * sem_post 包装：调用 exchange(1) 后检查旧值，若 >=1 输出诊断。
     * 用于检测重复释放协议 bug，避免静默丢失计数信号量语义。
     */
    static void SemPostWithDiag(std::atomic<uint8_t>* counter, const char* name)
    {
        const uint8_t old = Ns3AiSemaphore::sem_post(counter);
        if (old >= 1)
        {
            std::cerr << "ns3-ai [" << name << "] duplicate sem_post: old=" << +old
                      << " (protocol bug)" << std::endl;
        }
    };

    /**
     * 校验心跳参数约束。
     * - heartbeat_period_us == 0 表示禁用心跳，此时 heartbeat_timeout_us 也必须为 0
     * - heartbeat_period_us > 0 时：
     *   - period ∈ [100000, 60000000]
     *   - timeout ≥ max(3×period, 300000)（此下限始终校验）
     *   - timeout ≥ syncTimeoutUs 时仅 warning（sync timeout 会先触发，心跳检测不生效）
     */
    void ValidateHeartbeatConfig() const
    {
        if (m_heartbeatPeriodUs == 0)
        {
            if (m_heartbeatTimeoutUs != 0)
            {
                std::ostringstream oss;
                oss << "ns3-ai message interface heartbeat configuration error: "
                    << "heartbeat_period_us=0 requires heartbeat_timeout_us=0, "
                    << "got heartbeat_timeout_us=" << m_heartbeatTimeoutUs << ".";
                throw Ns3AiRuntimeError(oss.str());
            }
            return;
        }
        if (m_heartbeatPeriodUs < 100000 || m_heartbeatPeriodUs > 60000000)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface heartbeat configuration error: "
                << "heartbeat_period_us=" << m_heartbeatPeriodUs
                << " is out of range [100000, 60000000].";
            throw Ns3AiRuntimeError(oss.str());
        }
        // 始终校验 lower bound，不依赖 syncTimeoutUs
        const uint64_t minTimeout = std::max(3ULL * m_heartbeatPeriodUs, 300000ULL);
        if (m_heartbeatTimeoutUs < minTimeout)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface heartbeat configuration error: "
                << "heartbeat_timeout_us=" << m_heartbeatTimeoutUs
                << " must be at least " << minTimeout
                << " (required: max(3×period, 300000)).";
            throw Ns3AiRuntimeError(oss.str());
        }
        if (m_syncTimeoutUs > 0 && m_heartbeatTimeoutUs >= m_syncTimeoutUs)
        {
            std::cerr << "ns3-ai [" << m_headerName
                      << "] heartbeat_timeout_us=" << m_heartbeatTimeoutUs
                      << " >= sync_timeout_us=" << m_syncTimeoutUs
                      << "; heartbeat detection is disabled during this session "
                      << "(sync timeout fires before heartbeat timeout)." << std::endl;
        }
    };

    std::unique_ptr<boost::interprocess::managed_shared_memory> m_segment;
    Cpp2PyMsgType* m_cpp2pyStruct;
    Py2CppMsgType* m_py2CppStruct;
    Cpp2PyMsgVector* m_cpp2pyVector;
    Py2CppMsgVector* m_py2cppVector;
    MailboxSyncBlock* m_sync;
    Ns3AiMsgProtocolHeader* m_header;
    const bool m_isCreator;
    const bool m_useVector;
    const bool m_handleFinish;
    const std::string m_segName;
    const std::string m_cpp2pyMsgName;
    const std::string m_py2cppMsgName;
    const std::string m_lockableName;
    const std::string m_headerName;
    const uint64_t m_syncTimeoutUs;
    const uint64_t m_heartbeatPeriodUs;
    const uint64_t m_heartbeatTimeoutUs;
    // C++ 作为等待方时追踪 Python 心跳（单向检测，仅此方向活跃）
    uint64_t m_cppObservedPyCount{0};
    std::chrono::steady_clock::time_point m_cppObservedPyTime;
    bool m_cppObservingPy{false};
    const uint64_t m_cpp2pySchemaHash;
    const uint64_t m_py2cppSchemaHash;
    const uint32_t m_cpp2pySchemaVersion;
    const uint32_t m_py2cppSchemaVersion;
    const Ns3AiSchemaValidationMode m_schemaValidationMode;
    bool m_isFinished;
    bool m_pyRecvHasCpp2PySlot;
};

/**
 * \brief Singleton factory and registry for MailboxTransportImpl instances.
 *
 * Provides MakeNames() for SHM prefix->name expansion, and GetInterface<T,U>()
 * to create or retrieve a typed mailbox channel.
 *
 * This is a convenience facade for the mailbox transport only.  Higher-level
 * transport selection (QueueTransport, BatchTransport) uses TransportFactory
 * instead.
 */
class MailboxTransport : public Singleton<MailboxTransport>
{
  public:
    static MailboxTransportNames MakeNames(const std::string& prefix)
    {
        return MailboxTransportNames{prefix + ".seg",
                                      prefix + ".cpp2py",
                                      prefix + ".py2cpp",
                                      prefix + ".lock",
                                      prefix + ".header"};
    };

    MailboxTransportConfig GetDefaultConfig() const
    {
        return m_defaultConfig;
    };

    void SetDefaultConfig(const MailboxTransportConfig& config)
    {
        m_defaultConfig = config;
    };

    void SetIsMemoryCreator(bool isMemoryCreator)
    {
        m_defaultConfig.m_isMemoryCreator = isMemoryCreator;
    };

    void SetUseVector(bool useVector)
    {
        m_defaultConfig.m_useVector = useVector;
    };

    void SetHandleFinish(bool handleFinish)
    {
        m_defaultConfig.m_handleFinish = handleFinish;
    };

    void SetMemorySize(uint32_t size)
    {
        m_defaultConfig.m_size = size;
    };

    void SetSyncTimeoutUs(uint64_t timeoutUs)
    {
        m_defaultConfig.m_syncTimeoutUs = timeoutUs;
    };

    void SetSyncTimeoutMs(uint64_t timeoutMs)
    {
        m_defaultConfig.m_syncTimeoutUs = timeoutMs * 1000;
    };

    uint64_t GetSyncTimeoutUs() const
    {
        return m_defaultConfig.m_syncTimeoutUs;
    };

    void SetSchemas(const Ns3AiMsgSchema& cpp2pySchema, const Ns3AiMsgSchema& py2cppSchema)
    {
        m_defaultConfig.SetSchemas(cpp2pySchema, py2cppSchema);
    };

    void SetNames(std::string segmentName,
                  std::string cpp2pyMsgName,
                  std::string py2cppMsgName,
                  std::string lockableName)
    {
        SetNames(segmentName,
                 cpp2pyMsgName,
                 py2cppMsgName,
                 lockableName,
                 lockableName + ".header");
    };

    void SetNames(std::string segmentName,
                  std::string cpp2pyMsgName,
                  std::string py2cppMsgName,
                  std::string lockableName,
                  std::string headerName)
    {
        m_defaultConfig.m_names = MailboxTransportNames{segmentName,
                                                         cpp2pyMsgName,
                                                         py2cppMsgName,
                                                         lockableName,
                                                         headerName};
    };

    void SetNamesFromPrefix(const std::string& prefix)
    {
        m_defaultConfig.m_names = MakeNames(prefix);
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface()
    {
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(m_defaultConfig, "default");
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(const std::string& instanceId)
    {
        MailboxTransportConfig config = m_defaultConfig;
        config.m_names = MakeNames(instanceId);
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const std::string& segmentName,
        const std::string& cpp2pyMsgName,
        const std::string& py2cppMsgName,
        const std::string& lockableName,
        const std::string& instanceId = "custom")
    {
        MailboxTransportConfig config = m_defaultConfig;
        config.m_names = MailboxTransportNames{segmentName,
                                                cpp2pyMsgName,
                                                py2cppMsgName,
                                                lockableName,
                                                lockableName + ".header"};
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const MailboxTransportNames& names,
        const std::string& instanceId = "custom")
    {
        MailboxTransportConfig config = m_defaultConfig;
        config.m_names = names;
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const MailboxTransportConfig& config,
        const std::string& instanceId = "custom")
    {
        ValidateSchemaSizes<Cpp2PyMsgType, Py2CppMsgType>(config);
        const std::string key = BuildInterfaceKey<Cpp2PyMsgType, Py2CppMsgType>(instanceId, config);
        auto iter = m_interfaces.find(key);
        if (iter != m_interfaces.end())
        {
            return static_cast<MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>*>(iter->second.get());
        }
        auto interface = std::make_unique<MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>>(
            config.m_isMemoryCreator,
            config.m_useVector,
            config.m_handleFinish,
            config.m_size,
            config.m_names.m_segmentName.c_str(),
            config.m_names.m_cpp2pyMsgName.c_str(),
            config.m_names.m_py2cppMsgName.c_str(),
            config.m_names.m_lockableName.c_str(),
            config.m_syncTimeoutUs,
            config.m_names.m_headerName.c_str(),
            config.m_cpp2pySchemaHash,
            config.m_py2cppSchemaHash,
            config.m_cpp2pySchemaVersion,
            config.m_py2cppSchemaVersion,
            config.m_schemaValidationMode,
            config.m_heartbeatPeriodUs,
            config.m_heartbeatTimeoutUs);
        auto rawInterface = interface.get();
        m_interfaces.emplace(key, std::move(interface));
        return rawInterface;
    };

  private:
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    void ValidateSchemaSizes(const MailboxTransportConfig& config) const
    {
        if (config.m_cpp2pySchemaSize != 0 && config.m_cpp2pySchemaSize != sizeof(Cpp2PyMsgType))
        {
            throw std::runtime_error("ns3-ai cpp2py schema size does not match the C++ payload type");
        }
        if (config.m_py2cppSchemaSize != 0 && config.m_py2cppSchemaSize != sizeof(Py2CppMsgType))
        {
            throw std::runtime_error("ns3-ai py2cpp schema size does not match the C++ payload type");
        }
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    std::string BuildInterfaceKey(const std::string& instanceId,
                                  const MailboxTransportConfig& config) const
    {
        std::ostringstream oss;
        oss << typeid(Cpp2PyMsgType).name() << '|' << typeid(Py2CppMsgType).name() << '|'
            << instanceId << '|' << config.m_names.m_segmentName << '|'
            << config.m_names.m_cpp2pyMsgName << '|' << config.m_names.m_py2cppMsgName << '|'
            << config.m_names.m_lockableName << '|' << config.m_names.m_headerName << '|'
            << config.m_isMemoryCreator << '|' << config.m_useVector << '|' << config.m_handleFinish
            << '|' << config.m_size << '|' << config.m_syncTimeoutUs << '|'
            << config.m_heartbeatPeriodUs << '|' << config.m_heartbeatTimeoutUs << '|'
            << config.m_cpp2pySchemaHash << '|' << config.m_py2cppSchemaHash << '|'
            << config.m_cpp2pySchemaVersion << '|' << config.m_py2cppSchemaVersion << '|'
            << static_cast<uint8_t>(config.m_schemaValidationMode);
        return oss.str();
    };

    MailboxTransportConfig m_defaultConfig{};
    std::unordered_map<std::string, std::unique_ptr<MailboxTransportBase>> m_interfaces;
};

/* ---- Backward-compatible aliases (ns3-ai < 2.0) — new code should use the new names ---- */
using Ns3AiMsgPeer = TransportPeer;
using Ns3AiMsgSessionState = TransportSessionState;
using Ns3AiMsgCloseReason = TransportCloseReason;
using Ns3AiMsgErrorReason = TransportErrorReason;
using Ns3AiMsgSync = MailboxSyncBlock;
using Ns3AiMsgInterfaceNames = MailboxTransportNames;
using Ns3AiMsgInterfaceConfig = MailboxTransportConfig;
using Ns3AiMsgInterfaceBase = MailboxTransportBase;
using Ns3AiMsgInterface = MailboxTransport;
template <typename Cpp2PyMsgType, typename Py2CppMsgType>
using Ns3AiMsgInterfaceImpl = MailboxTransportImpl<Cpp2PyMsgType, Py2CppMsgType>;

} // namespace ns3

#define NS3_AI_MSG_FIELD(StructType, FieldType, Member)                                             \
    ns3::Ns3AiMsgField                                                                               \
    {                                                                                                 \
        #Member, FieldType, offsetof(StructType, Member), sizeof(((StructType*)nullptr)->Member), 1   \
    }

#define NS3_AI_MSG_SCHEMA(StructType, Version, ...)                                                  \
    ns3::MakeNs3AiMsgSchema(#StructType, Version, sizeof(StructType), {__VA_ARGS__})

#endif // NS3_AI_MSG_INTERFACE_H
