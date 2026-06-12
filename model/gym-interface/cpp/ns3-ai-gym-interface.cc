/*
 * Copyright (c) 2018 Piotr Gawlowicz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Piotr Gawlowicz <gawlowicz.p@gmail.com>
 * Modify: Muyuan Shen <muyuan_shen@hust.edu.cn>
 *
 */

/*
 * Note: The Gym interface class is only for C++ side. Do not create Python binding
 *       for this interface.
 */

#include "ns3-ai-gym-interface.h"

#include "container.h"
#include "messages.pb.h"
#include "ns3-ai-gym-env.h"
#include "spaces.h"

#include <ns3/config.h>
#include <ns3/log.h>
#include <ns3/simulator.h>
#include <ns3/ns3-ai-errors.h>

#include <cassert>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OpenGymInterface");
NS_OBJECT_ENSURE_REGISTERED(OpenGymInterface);

namespace
{

void
SerializeGymMessageOrThrow(const google::protobuf::MessageLite& message,
                           Ns3AiGymMsg* target,
                           const char* messageName)
{
    const auto messageSize = message.ByteSizeLong();
    if (messageSize > MSG_BUFFER_SIZE)
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym " << messageName << " message is " << messageSize
            << " bytes, which exceeds the configured shared-memory buffer of " << MSG_BUFFER_SIZE
            << " bytes. Increase NS3AI_GYM_MSG_BUFFER_SIZE when configuring ns-3.";
        throw Ns3AiRuntimeError(oss.str());
    }
    if (messageSize > std::numeric_limits<uint32_t>::max())
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym " << messageName
            << " message is too large to encode in the message header.";
        throw Ns3AiRuntimeError(oss.str());
    }

    target->size = static_cast<uint32_t>(messageSize);
    if (!message.SerializeToArray(target->buffer, static_cast<int>(target->size)))
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym failed to serialize " << messageName << " message.";
        throw Ns3AiRuntimeError(oss.str());
    }
}

void
ParseGymMessageOrThrow(google::protobuf::MessageLite* message,
                       const Ns3AiGymMsg* source,
                       const char* messageName)
{
    if (source->size > MSG_BUFFER_SIZE)
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym received " << messageName << " message with declared size "
            << source->size << " bytes, which exceeds the configured shared-memory buffer of "
            << MSG_BUFFER_SIZE << " bytes.";
        throw Ns3AiRuntimeError(oss.str());
    }
    if (!message->ParseFromArray(source->buffer, static_cast<int>(source->size)))
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym failed to parse " << messageName << " message.";
        throw Ns3AiRuntimeError(oss.str());
    }
}

void
ValidateSequenceOrThrow(uint64_t actualSequence, uint64_t expectedSequence, const char* messageName)
{
    if (actualSequence != expectedSequence)
    {
        std::ostringstream oss;
        oss << "ns3-ai Gym " << messageName << " sequence mismatch: expected " << expectedSequence
            << ", got " << actualSequence << ".";
        throw Ns3AiProtocolError(oss.str());
    }
}

void
HandleStopRequest()
{
    NS_LOG_DEBUG("---Stop requested: true");
    Simulator::Stop();
    Simulator::Destroy();
}

} // namespace

Ptr<OpenGymInterface>
OpenGymInterface::Get()
{
    NS_LOG_FUNCTION_NOARGS();
    return *DoGet();
}

Ptr<OpenGymInterface>
OpenGymInterface::Get(uint32_t envId)
{
    NS_LOG_FUNCTION(envId);
    return Get("ns3ai-gym-env-" + std::to_string(envId));
}

Ptr<OpenGymInterface>
OpenGymInterface::Get(const std::string& sharedMemoryPrefix)
{
    NS_LOG_FUNCTION(sharedMemoryPrefix);
    auto interfaces = DoGetNamedInterfaces();
    auto iter = interfaces->find(sharedMemoryPrefix);
    if (iter != interfaces->end())
    {
        return iter->second;
    }

    Ptr<OpenGymInterface> interface = CreateObject<OpenGymInterface>();
    interface->SetSharedMemoryPrefix(sharedMemoryPrefix);
    interfaces->emplace(sharedMemoryPrefix, interface);
    return interface;
}

OpenGymInterface::OpenGymInterface()
    : m_simEnd(false),
      m_stopEnvRequested(false),
      m_initSimMsgSent(false),
      m_stateAwaitingAction(false),
      m_memorySize(4096),
      m_nextSequence(1),
      m_pendingInitSequence(0),
      m_pendingStateSequence(0),
      m_msgNames{"My Seg", "My Cpp to Python Msg", "My Python to Cpp Msg", "My Lockable", "My Header"}
{
}

OpenGymInterface::OpenGymInterface(const std::string& sharedMemoryPrefix)
    : OpenGymInterface()
{
    SetSharedMemoryPrefix(sharedMemoryPrefix);
}

OpenGymInterface::~OpenGymInterface()
{
    m_msgInterface.reset();
}

TypeId
OpenGymInterface::GetTypeId()
{
    static TypeId tid = TypeId("OpenGymInterface")
                            .SetParent<Object>()
                            .SetGroupName("OpenGym")
                            .AddConstructor<OpenGymInterface>();
    return tid;
}

uint64_t
OpenGymInterface::NextSequence()
{
    return m_nextSequence++;
}

void
OpenGymInterface::SetSharedMemoryPrefix(const std::string& sharedMemoryPrefix)
{
    NS_LOG_FUNCTION(this << sharedMemoryPrefix);
    if (m_msgInterface)
    {
        throw Ns3AiRuntimeError(
            "ns3-ai Gym interface shared-memory prefix must be set "
            "before the message interface is opened");
    }
    m_msgNames = MailboxTransport::MakeNames(sharedMemoryPrefix);
}

void
OpenGymInterface::SetSharedMemoryNames(std::string segmentName,
                                       std::string cpp2pyMsgName,
                                       std::string py2cppMsgName,
                                       std::string lockableName)
{
    NS_LOG_FUNCTION(this << segmentName << cpp2pyMsgName << py2cppMsgName << lockableName);
    if (m_msgInterface)
    {
        throw Ns3AiRuntimeError(
            "ns3-ai Gym interface shared-memory names must be set "
            "before the message interface is opened");
    }
    m_msgNames = MailboxTransportNames{segmentName, cpp2pyMsgName, py2cppMsgName, lockableName, "My Header"};
}

void
OpenGymInterface::SetSharedMemorySize(uint32_t size)
{
    NS_LOG_FUNCTION(this << size);
    if (m_msgInterface)
    {
        throw Ns3AiRuntimeError(
            "ns3-ai Gym interface shared-memory size must be set "
            "before the message interface is opened");
    }
    m_memorySize = size;
}

MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>*
OpenGymInterface::GetMsgInterface()
{
    if (!m_msgInterface)
    {
        m_msgInterface = std::make_unique<MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>>(
            false,
            false,
            false,
            m_memorySize,
            m_msgNames.m_segmentName.c_str(),
            m_msgNames.m_cpp2pyMsgName.c_str(),
            m_msgNames.m_py2cppMsgName.c_str(),
            m_msgNames.m_lockableName.c_str(),
            MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>::DEFAULT_SYNC_TIMEOUT_US,
            m_msgNames.m_headerName.c_str());
    }
    return m_msgInterface.get();
}

void
OpenGymInterface::Init()
{
    if (m_initSimMsgSent)
    {
        return;
    }
    m_initSimMsgSent = true;

    Ptr<OpenGymSpace> obsSpace = GetObservationSpace();
    Ptr<OpenGymSpace> actionSpace = GetActionSpace();

    ns3_ai_gym::SimInitMsg simInitMsg;
    m_pendingInitSequence = NextSequence();
    simInitMsg.set_sequence(m_pendingInitSequence);
    if (obsSpace)
    {
        ns3_ai_gym::SpaceDescription spaceDesc = obsSpace->GetSpaceDescription();
        simInitMsg.mutable_obsspace()->CopyFrom(spaceDesc);
    }
    if (actionSpace)
    {
        ns3_ai_gym::SpaceDescription spaceDesc = actionSpace->GetSpaceDescription();
        simInitMsg.mutable_actspace()->CopyFrom(spaceDesc);
    }

    MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>* msgInterface = GetMsgInterface();

    msgInterface->CppSendBegin();
    SerializeGymMessageOrThrow(simInitMsg, msgInterface->GetCpp2PyStruct(), "SimInitMsg");
    msgInterface->CppSendEnd();

    ns3_ai_gym::SimInitAck simInitAck;
    msgInterface->CppRecvBegin();
    ParseGymMessageOrThrow(&simInitAck, msgInterface->GetPy2CppStruct(), "SimInitAck");
    msgInterface->CppRecvEnd();
    ValidateSequenceOrThrow(simInitAck.sequence(), m_pendingInitSequence, "SimInitAck");
    m_pendingInitSequence = 0;

    bool done = simInitAck.done();
    NS_LOG_DEBUG("Sim Init Ack: " << done);
    bool stopSim = simInitAck.stopsimreq();
    if (stopSim)
    {
        m_stopEnvRequested = true;
        HandleStopRequest();
        return;
    }
}

void
OpenGymInterface::SendCurrentState()
{
    if (!m_initSimMsgSent)
    {
        Init();
    }
    if (m_stopEnvRequested)
    {
        return;
    }
    if (m_stateAwaitingAction)
    {
        throw Ns3AiProtocolError(
            "ns3-ai Gym interface protocol violation in SendCurrentState: "
            "previous Gym state is still awaiting an action. "
            "Call ReceiveActions() before sending the next state.");
    }

    Ptr<OpenGymDataContainer> obsDataContainer = GetObservation();
    float reward = GetReward();
    bool terminated = IsGameOver();
    bool truncated = IsTruncated();
    std::string extraInfo = GetExtraInfo();
    int32_t errorCode = GetErrorCode();
    std::string errorMessage = GetErrorMessage();
    ns3_ai_gym::EnvStateMsg envStateMsg;
    m_pendingStateSequence = NextSequence();
    envStateMsg.set_sequence(m_pendingStateSequence);

    ns3_ai_gym::DataContainer obsDataContainerPbMsg;
    if (obsDataContainer)
    {
        obsDataContainerPbMsg = obsDataContainer->GetDataContainerPbMsg();
        envStateMsg.mutable_obsdata()->CopyFrom(obsDataContainerPbMsg);
    }
    if (m_simEnd)
    {
        terminated = false;
        truncated = true;
    }

    envStateMsg.set_reward(reward);
    envStateMsg.set_terminated(terminated);
    envStateMsg.set_truncated(truncated);
    envStateMsg.set_isgameover(terminated || truncated || errorCode != 0 || !errorMessage.empty());
    envStateMsg.set_errorcode(errorCode);
    envStateMsg.set_errormessage(errorMessage);
    envStateMsg.set_reason(ns3_ai_gym::EnvStateMsg::ReasonUnspecified);
    if (errorCode != 0 || !errorMessage.empty())
    {
        envStateMsg.set_reason(ns3_ai_gym::EnvStateMsg::EnvironmentError);
    }
    else if (m_simEnd)
    {
        envStateMsg.set_reason(ns3_ai_gym::EnvStateMsg::SimulationEnd);
    }
    else if (truncated)
    {
        envStateMsg.set_reason(ns3_ai_gym::EnvStateMsg::EpisodeTruncated);
    }
    else if (terminated)
    {
        envStateMsg.set_reason(ns3_ai_gym::EnvStateMsg::EpisodeTerminated);
    }
    envStateMsg.set_info(extraInfo);

    MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>* msgInterface = GetMsgInterface();
    msgInterface->CppSendBegin();
    SerializeGymMessageOrThrow(envStateMsg, msgInterface->GetCpp2PyStruct(), "EnvStateMsg");
    msgInterface->CppSendEnd();
    m_stateAwaitingAction = true;
}

void
OpenGymInterface::ReceiveActions()
{
    if (m_stopEnvRequested || !m_stateAwaitingAction)
    {
        return;
    }

    MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>* msgInterface = GetMsgInterface();
    ns3_ai_gym::EnvActMsg envActMsg;
    msgInterface->CppRecvBegin();
    ParseGymMessageOrThrow(&envActMsg, msgInterface->GetPy2CppStruct(), "EnvActMsg");
    msgInterface->CppRecvEnd();
    ValidateSequenceOrThrow(envActMsg.sequence(), m_pendingStateSequence, "EnvActMsg");
    m_pendingStateSequence = 0;
    m_stateAwaitingAction = false;

    if (m_simEnd)
    {
        return;
    }

    bool stopSim = envActMsg.stopsimreq();
    if (stopSim)
    {
        m_stopEnvRequested = true;
        HandleStopRequest();
        return;
    }

    ns3_ai_gym::DataContainer actDataContainerPbMsg = envActMsg.actdata();
    Ptr<OpenGymDataContainer> actDataContainer =
        OpenGymDataContainer::CreateFromDataContainerPbMsg(actDataContainerPbMsg);
    ExecuteActions(actDataContainer);
}

void
OpenGymInterface::NotifyCurrentState()
{
    SendCurrentState();
    ReceiveActions();
}

void
OpenGymInterface::WaitForStop()
{
    NS_LOG_FUNCTION(this);
    NotifyCurrentState();
}

void
OpenGymInterface::NotifySimulationEnd()
{
    NS_LOG_FUNCTION(this);
    m_simEnd = true;
    if (m_initSimMsgSent)
    {
        WaitForStop();
    }
}

Ptr<OpenGymSpace>
OpenGymInterface::GetActionSpace()
{
    NS_LOG_FUNCTION(this);
    Ptr<OpenGymSpace> actionSpace;
    if (!m_actionSpaceCb.IsNull())
    {
        actionSpace = m_actionSpaceCb();
    }
    return actionSpace;
}

Ptr<OpenGymSpace>
OpenGymInterface::GetObservationSpace()
{
    NS_LOG_FUNCTION(this);
    Ptr<OpenGymSpace> obsSpace;
    if (!m_observationSpaceCb.IsNull())
    {
        obsSpace = m_observationSpaceCb();
    }
    return obsSpace;
}

Ptr<OpenGymDataContainer>
OpenGymInterface::GetObservation()
{
    NS_LOG_FUNCTION(this);
    Ptr<OpenGymDataContainer> obs;
    if (!m_obsCb.IsNull())
    {
        obs = m_obsCb();
    }
    return obs;
}

float
OpenGymInterface::GetReward()
{
    NS_LOG_FUNCTION(this);
    float reward = 0.0;
    if (!m_rewardCb.IsNull())
    {
        reward = m_rewardCb();
    }
    return reward;
}

bool
OpenGymInterface::IsTruncated()
{
    NS_LOG_FUNCTION(this);
    bool truncated = false;
    if (!m_truncatedCb.IsNull())
    {
        truncated = m_truncatedCb();
    }
    return truncated;
}

int32_t
OpenGymInterface::GetErrorCode()
{
    NS_LOG_FUNCTION(this);
    int32_t errorCode = 0;
    if (!m_errorCodeCb.IsNull())
    {
        errorCode = m_errorCodeCb();
    }
    return errorCode;
}

std::string
OpenGymInterface::GetErrorMessage()
{
    NS_LOG_FUNCTION(this);
    std::string errorMessage;
    if (!m_errorMessageCb.IsNull())
    {
        errorMessage = m_errorMessageCb();
    }
    return errorMessage;
}

bool
OpenGymInterface::IsGameOver()
{
    NS_LOG_FUNCTION(this);
    bool gameOver = false;
    if (!m_gameOverCb.IsNull())
    {
        gameOver = m_gameOverCb();
    }
    return gameOver;
}

std::string
OpenGymInterface::GetExtraInfo()
{
    NS_LOG_FUNCTION(this);
    std::string info;
    if (!m_extraInfoCb.IsNull())
    {
        info = m_extraInfoCb();
    }
    return info;
}

bool
OpenGymInterface::ExecuteActions(Ptr<OpenGymDataContainer> action)
{
    NS_LOG_FUNCTION(this);
    bool reply = false;
    if (!m_actionCb.IsNull())
    {
        reply = m_actionCb(action);
    }
    return reply;
}

void
OpenGymInterface::SetGetActionSpaceCb(Callback<Ptr<OpenGymSpace>> cb)
{
    m_actionSpaceCb = cb;
}

void
OpenGymInterface::SetGetObservationSpaceCb(Callback<Ptr<OpenGymSpace>> cb)
{
    m_observationSpaceCb = cb;
}

void
OpenGymInterface::SetGetGameOverCb(Callback<bool> cb)
{
    m_gameOverCb = cb;
}

void
OpenGymInterface::SetGetObservationCb(Callback<Ptr<OpenGymDataContainer>> cb)
{
    m_obsCb = cb;
}

void
OpenGymInterface::SetGetRewardCb(Callback<float> cb)
{
    m_rewardCb = cb;
}

void
OpenGymInterface::SetGetTruncatedCb(Callback<bool> cb)
{
    m_truncatedCb = cb;
}

void
OpenGymInterface::SetGetErrorCodeCb(Callback<int32_t> cb)
{
    m_errorCodeCb = cb;
}

void
OpenGymInterface::SetGetErrorMessageCb(Callback<std::string> cb)
{
    m_errorMessageCb = cb;
}

void
OpenGymInterface::SetGetExtraInfoCb(Callback<std::string> cb)
{
    m_extraInfoCb = cb;
}

void
OpenGymInterface::SetExecuteActionsCb(Callback<bool, Ptr<OpenGymDataContainer>> cb)
{
    m_actionCb = cb;
}

void
OpenGymInterface::DoInitialize()
{
    NS_LOG_FUNCTION(this);
}

void
OpenGymInterface::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_msgInterface.reset();
    m_actionSpaceCb = Callback<Ptr<OpenGymSpace>>();
    m_observationSpaceCb = Callback<Ptr<OpenGymSpace>>();
    m_gameOverCb = Callback<bool>();
    m_obsCb = Callback<Ptr<OpenGymDataContainer>>();
    m_rewardCb = Callback<float>();
    m_extraInfoCb = Callback<std::string>();
    m_actionCb = Callback<bool, Ptr<OpenGymDataContainer>>();
    Object::DoDispose();
}

void
OpenGymInterface::Notify(Ptr<OpenGymEnv> entity)
{
    NS_LOG_FUNCTION(this);

    SetGetGameOverCb(MakeCallback(&OpenGymEnv::GetGameOver, entity));
    SetGetObservationCb(MakeCallback(&OpenGymEnv::GetObservation, entity));
    SetGetRewardCb(MakeCallback(&OpenGymEnv::GetReward, entity));
    SetGetExtraInfoCb(MakeCallback(&OpenGymEnv::GetExtraInfo, entity));
    SetExecuteActionsCb(MakeCallback(&OpenGymEnv::ExecuteActions, entity));

    NotifyCurrentState();
}

Ptr<OpenGymInterface>*
OpenGymInterface::DoGet()
{
    static Ptr<OpenGymInterface> ptr = CreateObject<OpenGymInterface>();
    return &ptr;
}

std::unordered_map<std::string, Ptr<OpenGymInterface>>*
OpenGymInterface::DoGetNamedInterfaces()
{
    static std::unordered_map<std::string, Ptr<OpenGymInterface>> interfaces;
    return &interfaces;
}

void
OpenGymInterface::Reset()
{
    NS_LOG_FUNCTION_NOARGS();
    *DoGet() = CreateObject<OpenGymInterface>();
    DoGetNamedInterfaces()->clear();
}

void
OpenGymInterface::ResetNamedInterfaces()
{
    NS_LOG_FUNCTION_NOARGS();
    DoGetNamedInterfaces()->clear();
}

} // namespace ns3
