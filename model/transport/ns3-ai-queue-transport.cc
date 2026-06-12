/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3-ai-queue-transport.h"

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstring>
#include <stdexcept>

namespace ns3
{

namespace
{

constexpr uint32_t QUEUE_TRANSPORT_MAGIC = 0x514E3341; // "QN3A"
constexpr uint16_t QUEUE_TRANSPORT_ABI_VERSION = 1;

} // anonymous namespace

QueueTransport::QueueTransport(const QueueTransportConfig& cfg, bool isCreator)
    : m_cfg(cfg)
{
    if (!IsPowerOfTwo(cfg.queueDepth))
    {
        throw Ns3AiRuntimeError("QueueTransport: queueDepth must be a power of two");
    }

    m_depth = cfg.queueDepth;
    m_payloadCapacity = cfg.maxMessageSize;
    m_slotSize = sizeof(QueueSlotHeader) + m_payloadCapacity;

    InitializeShm(isCreator);

    // Build capability descriptor — conservative skeleton (no ring-buffer impl yet)
    m_caps.maxDepth = 0;
    m_caps.maxMessageSize = m_payloadCapacity;
    m_caps.sendConcurrency = ConcurrencyModel::SPSC;
    m_caps.recvConcurrency = ConcurrencyModel::SPSC;
    m_caps.laneGuarantee = LaneGuarantee::SharedQueueWithLaneMetadata;
    m_caps.ordering = OrderingGuarantee::StrictPerDirection;
    m_caps.supportsNonBlockingSend = false;
    m_caps.supportsNonBlockingRecv = false;
    m_caps.preservesMetadata = true;
    m_caps.preservesLaneMetadata = true;
    m_caps.supportsGapDetection = false;
    m_caps.reportsPerLaneErrors = false;
    m_caps.supportsSessionSurvivalAfterLaneError = false;
    m_caps.backpressureCaps = 0;
}

QueueTransport::~QueueTransport() = default;

void
QueueTransport::InitializeShm(bool isCreator)
{
    // Implementation sketched — full SHM segment creation and object
    // placement will reuse boost::interprocess primitives.
    //
    // Layout:
    //   [QueueTransportHeader]
    //   [RingBufferControl cpp2py]
    //   [RingBufferControl py2cpp]
    //   [Slot[0]..Slot[depth-1] for Cpp2Py]
    //   [Slot[0]..Slot[depth-1] for Py2Cpp]
}

SendResult
QueueTransport::Send(const TransportMessage& msg, Timeout timeout)
{
    (void)msg;
    (void)timeout;
    SendResult result;
    result.status = SendStatus::SessionError;
    result.errorDescription = "QueueTransport: skeleton — full ring-buffer implementation pending";
    return result;
}

RecvResult
QueueTransport::Recv(TransportMessage& msg, Timeout timeout)
{
    (void)msg;
    (void)timeout;
    RecvResult result;
    result.status = RecvStatus::SessionError;
    result.errorDescription = "QueueTransport: skeleton — full ring-buffer implementation pending";
    return result;
}

TransportCapabilities
QueueTransport::GetCapabilities() const
{
    return m_caps;
}

std::string
QueueTransport::GetName() const
{
    return "QueueTransport";
}

} // namespace ns3
