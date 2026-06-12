/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_TRANSPORT_H
#define NS3_AI_TRANSPORT_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ns3
{

/**
 * \brief Per-message envelope metadata.
 *
 * Transport layer preserves these fields but does not interpret their
 * semantics beyond laneId routing.  Unused fields default to 0.
 */
struct TransportMetadata
{
    uint32_t laneId = 0;      //!< Lane / sub-environment identifier
    uint64_t messageId = 0;   //!< Monotonic message sequence number
    uint64_t episodeId = 0;   //!< RL episode identifier (opaque to Transport)
    uint64_t stepId = 0;      //!< RL step counter (opaque to Transport)
    uint32_t flags = 0;       //!< User-defined flags
};

/**
 * \brief A transport-layer message: envelope + raw bytes payload.
 *
 * The payload is a copy of the underlying shared-memory slot or ring-buffer
 * entry.  Zero-copy access is a future extension.
 */
struct TransportMessage
{
    TransportMetadata meta;         //!< Message envelope
    std::vector<uint8_t> payload;   //!< Raw bytes payload
};

/**
 * \brief Concurrency model of a Transport instance.
 */
enum class ConcurrencyModel : uint8_t
{
    SPSC,  //!< Single producer, single consumer
    MPSC,  //!< Multiple producers, single consumer
    SPMC,  //!< Single producer, multiple consumers
    MPMC   //!< Multiple producers, multiple consumers
};

/**
 * \brief Lane isolation guarantee provided by a Transport or MultiLaneTransport.
 */
enum class LaneGuarantee : uint8_t
{
    None,                          //!< No lane isolation (e.g. MailboxTransport)
    SharedQueueWithLaneMetadata,   //!< Single queue carries laneId in metadata
    PhysicalPerLaneTransport       //!< Each lane has its own Transport instance
};

/**
 * \brief Message ordering guarantee.
 */
enum class OrderingGuarantee : uint8_t
{
    None,                //!< No ordering guarantees
    StrictPerDirection,  //!< Strict FIFO within each direction
    TotalOrder           //!< Global total order across all directions
};

/**
 * \brief Bitmask flags for backpressure capabilities.
 */
enum BackpressureCapability : uint32_t
{
    BackpressureBlock      = 1 << 0,
    BackpressureDropNewest = 1 << 1,
    BackpressureDropOldest = 1 << 2,
    BackpressureDropAll    = 1 << 3,
    BackpressureFailFast   = 1 << 4
};

/**
 * \brief Describes a Transport implementation's capabilities.
 *
 * Consumers check guarantee levels via enum comparison rather than
 * individual boolean flags.  For example:
 * \code
 * if (caps.laneGuarantee >= LaneGuarantee::PhysicalPerLaneTransport) { ... }
 * \endcode
 */
struct TransportCapabilities
{
    uint32_t maxDepth = 0;             //!< 0 = single-slot (mailbox), > 0 = queue depth
    uint32_t maxMessageSize = 0;       //!< Maximum payload size in bytes, 0 = unlimited

    ConcurrencyModel sendConcurrency = ConcurrencyModel::SPSC;
    ConcurrencyModel recvConcurrency = ConcurrencyModel::SPSC;

    LaneGuarantee laneGuarantee = LaneGuarantee::None;
    OrderingGuarantee ordering = OrderingGuarantee::StrictPerDirection;

    bool supportsNonBlockingSend = false;
    bool supportsNonBlockingRecv = false;
    bool supportsBatch = false;
    bool preservesMetadata = true;
    bool preservesLaneMetadata = true;
    bool supportsGapDetection = false;
    bool reportsPerLaneErrors = false;
    bool supportsSessionSurvivalAfterLaneError = false;

    uint32_t backpressureCaps = 0;  //!< OR-ed \c BackpressureCapability flags
};

/**
 * \brief Result status of a Send operation.
 */
enum class SendStatus : uint8_t
{
    Ok,
    QueueFull,
    DroppedSelf,
    DroppedOther,
    Timeout,
    LaneRejected,
    MessageTooLarge,
    SessionError
};

/**
 * \brief Result status of a Recv operation.
 */
enum class RecvStatus : uint8_t
{
    Ok,
    Empty,
    Timeout,
    MessageGap,
    StaleMessage,
    SessionError
};

/**
 * \brief Information about a message that was dropped (e.g. backpressure drop).
 */
struct DropInfo
{
    uint32_t laneId = 0;
    uint64_t droppedMessageId = 0;
    std::string reason;
};

/**
 * \brief Result of a Send call.
 */
struct SendResult
{
    SendStatus status = SendStatus::Ok;
    uint32_t laneId = 0;
    uint64_t messageId = 0;
    std::vector<DropInfo> dropped;
    std::string errorDescription;
};

/**
 * \brief Result of a Recv call.
 */
struct RecvResult
{
    RecvStatus status = RecvStatus::Ok;
    TransportMessage msg;
    uint32_t laneId = 0;
    std::string errorDescription;
};

using Timeout = std::chrono::microseconds;

/**
 * \brief Abstract transport layer for ns3-ai IPC.
 *
 * Transports raw bytes + envelope metadata.  The concrete implementations
 * (MailboxTransport, QueueTransport, BatchTransport) provide different
 * throughput/latency/ordering trade-offs.
 *
 * \todo Add TransportFactory::Create for runtime selection (planned).
 */
class Transport
{
  public:
    virtual ~Transport() = default;

    /**
     * \brief Send a message.
     * \param msg The message (metadata + payload).
     * \param timeout Per-operation timeout.
     * \return SendResult with status and optional drop information.
     */
    virtual SendResult Send(const TransportMessage& msg, Timeout timeout) = 0;

    /**
     * \brief Receive a message.
     * \param msg [out] Filled with received message on success.
     * \param timeout Per-operation timeout.
     * \return RecvResult with status.
     */
    virtual RecvResult Recv(TransportMessage& msg, Timeout timeout) = 0;

    /**
     * \brief Query transport capabilities.
     * \return Capability descriptor.
     */
    virtual TransportCapabilities GetCapabilities() const = 0;

    /**
     * \brief Human-readable transport name.
     */
    virtual std::string GetName() const = 0;
};

/**
 * \brief Multi-lane transport interface.
 *
 * Groups multiple transport lanes under a single API.  VecEnv and similar
 * multi-agent consumers depend on this interface rather than managing a
 * bare vector of Transport pointers.
 */
class MultiLaneTransport
{
  public:
    virtual ~MultiLaneTransport() = default;

    /**
     * \brief Send on a specific lane.
     */
    virtual SendResult Send(uint32_t laneId, const TransportMessage& msg) = 0;

    /**
     * \brief Receive from a specific lane.
     */
    virtual RecvResult Recv(uint32_t laneId, TransportMessage& msg) = 0;

    /**
     * \brief Receive from any ready lane (VecEnv "recv_any" semantic).
     */
    virtual RecvResult RecvAny(TransportMessage& msg) = 0;

    /**
     * \brief Number of lanes.
     */
    virtual uint32_t GetNumLanes() const = 0;

    /**
     * \brief Aggregate capabilities across all lanes.
     */
    virtual TransportCapabilities GetCapabilities() const = 0;

    /**
     * \brief Capabilities for a specific lane.
     */
    virtual TransportCapabilities GetLaneCapabilities(uint32_t laneId) const = 0;
};

/**
 * \brief Transport kind enum for factory selection.
 */
enum class TransportKind : uint8_t
{
    Mailbox,
    Queue,
    Batch
};

/**
 * \brief Configuration struct for MailboxTransport is defined in
 * ns3-ai-msg-interface.h (MailboxTransportConfig).
 */

/**
 * \brief Backpressure policy for QueueTransport.
 */
enum class QueueBackpressurePolicy : uint8_t
{
    Block,
    DropNewest,
    DropOldest,
    DropAll,
    FailFast
};

/**
 * \brief Configuration for QueueTransport.
 */
struct QueueTransportConfig
{
    std::string shmPrefix;
    uint32_t shmSize = 65536;
    uint32_t queueDepth = 1024;
    uint32_t maxMessageSize = 65536;
    QueueBackpressurePolicy backpressure = QueueBackpressurePolicy::Block;
};

} // namespace ns3

#endif // NS3_AI_TRANSPORT_H
