/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_QUEUE_TRANSPORT_H
#define NS3_AI_QUEUE_TRANSPORT_H

#include "ns3-ai-transport.h"
#include "../msg-interface/ns3-ai-errors.h"

#include <boost/interprocess/managed_shared_memory.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ns3
{

/**
 * \brief Cache-line aligned per-direction ring-buffer control block.
 *
 * Two such control blocks exist in shared memory, one for each direction
 * (Cpp2Py and Py2Cpp).  Each block is a monotic-index SPSC queue:
 *
 *   slot = writeIndex % depth
 *
 * writeIndex is advanced by the producer; readIndex is advanced by the
 * consumer.  When writeIndex - readIndex >= depth, the queue is full.
 */
struct alignas(64) RingBufferControl
{
    /// Producer advances this after filling a slot.
    std::atomic<uint64_t> writeIndex{0};
    char pad1[64 - sizeof(std::atomic<uint64_t>)];

    /// Consumer advances this after reading a slot.
    std::atomic<uint64_t> readIndex{0};
    char pad2[64 - sizeof(std::atomic<uint64_t>)];

    /// Maximum number of slots. Must be power of two.
    uint32_t depth;
    /// Size of each slot in bytes (header + payload).
    uint32_t slotSize;
    /// Configuration flags (reserved).
    uint32_t flags{0};
};

static_assert(sizeof(RingBufferControl) % 64 == 0,
              "RingBufferControl must be a multiple of cache-line size");

/**
 * \brief Shared-memory layout header for QueueTransport.
 */
struct QueueTransportHeader
{
    uint32_t magic;
    uint16_t abiVersion;
    uint16_t pad;
    uint64_t generationId;
};

/**
 * \brief Per-slot header in the ring buffer.
 */
struct QueueSlotHeader
{
    TransportMetadata meta;
    uint32_t payloadSize;
    uint32_t flags;
};

/**
 * \brief Ring-buffer slot.
 */
struct QueueSlot
{
    QueueSlotHeader header;
    uint8_t payload[1]; // flexible array — actual size = slotSize - sizeof(QueueSlotHeader)
};

/**
 * \brief SPSC ring-buffer transport implementation.
 *
 * QueueTransport provides a multi-slot (configurable depth), lock-free
 * ring-buffer for one producer and one consumer per direction.  It is
 * the preferred transport for vector and multi-agent environments that
 * need higher throughput than MailboxTransport.
 *
 * \note This implementation uses monotonic uint64_t indices to avoid ABA
 * problems.  The actual slot is computed as index & (depth - 1).  depth
 * must be a power of two.
 */
class QueueTransport : public Transport
{
  public:
    /**
     * \brief Construct a shared-memory ring-buffer.
     * \param cfg Queue configuration.
     * \param isCreator Whether this process creates the shared memory.
     */
    explicit QueueTransport(const QueueTransportConfig& cfg, bool isCreator);

    ~QueueTransport() override;

    // Transport interface
    SendResult Send(const TransportMessage& msg, Timeout timeout) override;
    RecvResult Recv(TransportMessage& msg, Timeout timeout) override;
    TransportCapabilities GetCapabilities() const override;
    std::string GetName() const override;

  private:
    void InitializeShm(bool isCreator);
    bool SlotAvailable(uint32_t direction) const;
    void* WriteSlot(uint32_t direction, const TransportMessage& msg);
    bool ReadSlot(uint32_t direction, TransportMessage& msg);

    /// Check whether depth is a power of two.
    static bool IsPowerOfTwo(uint32_t v);

    QueueTransportConfig m_cfg;
    uint32_t m_depth;
    uint32_t m_slotSize;
    uint32_t m_payloadCapacity;

    // Shared memory
    std::unique_ptr<boost::interprocess::managed_shared_memory> m_segment;

    // Control blocks (pointers into shared memory)
    RingBufferControl* m_cpp2py{nullptr};
    RingBufferControl* m_py2cpp{nullptr};

    // Data area offsets / pointers
    uint8_t* m_cpp2pySlots{nullptr};
    uint8_t* m_py2cppSlots{nullptr};

    // Direction enum for internal use
    enum Direction : uint32_t
    {
        Cpp2Py = 0,
        Py2Cpp = 1
    };

    // Capabilities cache
    TransportCapabilities m_caps;
};

// ---- inline helpers ----

inline bool
QueueTransport::IsPowerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

inline bool
QueueTransport::SlotAvailable(uint32_t direction) const
{
    const auto* ctrl = (direction == Cpp2Py) ? m_cpp2py : m_py2cpp;
    const uint64_t written = ctrl->writeIndex.load(std::memory_order_acquire);
    const uint64_t read = ctrl->readIndex.load(std::memory_order_acquire);
    return (written - read) < m_depth;
}

} // namespace ns3

#endif // NS3_AI_QUEUE_TRANSPORT_H
