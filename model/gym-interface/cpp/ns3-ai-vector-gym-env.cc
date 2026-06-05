/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "ns3-ai-vector-gym-env.h"

#include "ns3-ai-gym-interface.h"

#include <ns3/assert.h>
#include <ns3/log.h>
#include <ns3/object.h>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(OpenGymVectorEnv);
NS_LOG_COMPONENT_DEFINE("OpenGymVectorEnv");

OpenGymVectorEnv::OpenGymVectorEnv(uint32_t numEnvs)
    : m_numEnvs(numEnvs),
      m_activeEnvIndex(0)
{
    NS_LOG_FUNCTION(this << numEnvs);
    NS_ASSERT_MSG(numEnvs > 0, "OpenGymVectorEnv requires at least one environment");
    m_interfaces.resize(m_numEnvs);
}

OpenGymVectorEnv::~OpenGymVectorEnv()
{
    NS_LOG_FUNCTION(this);
}

TypeId
OpenGymVectorEnv::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OpenGymVectorEnv").SetParent<OpenGymEnv>().SetGroupName("OpenGym");
    return tid;
}

uint32_t
OpenGymVectorEnv::GetNumEnvs() const
{
    return m_numEnvs;
}

void
OpenGymVectorEnv::SetNumEnvs(uint32_t numEnvs)
{
    NS_LOG_FUNCTION(this << numEnvs);
    NS_ASSERT_MSG(numEnvs > 0, "OpenGymVectorEnv requires at least one environment");
    NS_ASSERT_MSG(m_interfaces.empty() || m_interfaces.size() == m_numEnvs,
                  "unexpected vector interface state");
    m_numEnvs = numEnvs;
    m_interfaces.clear();
    m_interfaces.resize(m_numEnvs);
    m_activeEnvIndex = 0;
}

void
OpenGymVectorEnv::SetOpenGymInterfaces(const std::string& sharedMemoryPrefixBase)
{
    NS_LOG_FUNCTION(this << sharedMemoryPrefixBase);
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        BindInterface(envIndex,
                      OpenGymInterface::Get(sharedMemoryPrefixBase + "-" + std::to_string(envIndex)));
    }
}

void
OpenGymVectorEnv::SetOpenGymInterfaces(const std::vector<Ptr<OpenGymInterface>>& interfaces)
{
    NS_LOG_FUNCTION(this << interfaces.size());
    NS_ASSERT_MSG(interfaces.size() == m_numEnvs,
                  "number of OpenGymInterface instances must match numEnvs");
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        BindInterface(envIndex, interfaces[envIndex]);
    }
}

Ptr<OpenGymInterface>
OpenGymVectorEnv::GetOpenGymInterface(uint32_t envIndex) const
{
    ValidateEnvIndex(envIndex);
    return m_interfaces[envIndex];
}

uint32_t
OpenGymVectorEnv::GetActiveEnvIndex() const
{
    return m_activeEnvIndex;
}

void
OpenGymVectorEnv::Initialize(uint32_t envIndex)
{
    NS_LOG_FUNCTION(this << envIndex);
    ValidateEnvIndex(envIndex);
    NS_ASSERT_MSG(m_interfaces[envIndex], "OpenGymVectorEnv lane is not bound to an interface");
    m_activeEnvIndex = envIndex;
    m_interfaces[envIndex]->SetGetActionSpaceCb(MakeCallback(&OpenGymEnv::GetActionSpace, this));
    m_interfaces[envIndex]->SetGetObservationSpaceCb(MakeCallback(&OpenGymEnv::GetObservationSpace, this));
    m_interfaces[envIndex]->Init();
}

void
OpenGymVectorEnv::InitializeAll()
{
    NS_LOG_FUNCTION(this);
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        Initialize(envIndex);
    }
}

void
OpenGymVectorEnv::Notify(uint32_t envIndex)
{
    NS_LOG_FUNCTION(this << envIndex);
    ValidateEnvIndex(envIndex);
    NS_ASSERT_MSG(m_interfaces[envIndex], "OpenGymVectorEnv lane is not bound to an interface");
    m_activeEnvIndex = envIndex;
    m_interfaces[envIndex]->SetGetGameOverCb(MakeCallback(&OpenGymEnv::GetGameOver, this));
    m_interfaces[envIndex]->SetGetObservationCb(MakeCallback(&OpenGymEnv::GetObservation, this));
    m_interfaces[envIndex]->SetGetRewardCb(MakeCallback(&OpenGymEnv::GetReward, this));
    m_interfaces[envIndex]->SetGetExtraInfoCb(MakeCallback(&OpenGymEnv::GetExtraInfo, this));
    m_interfaces[envIndex]->SetExecuteActionsCb(MakeCallback(&OpenGymEnv::ExecuteActions, this));
    m_interfaces[envIndex]->SendCurrentState();
    m_interfaces[envIndex]->ReceiveActions();
}

void
OpenGymVectorEnv::NotifyAll()
{
    NS_LOG_FUNCTION(this);
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        ValidateEnvIndex(envIndex);
        NS_ASSERT_MSG(m_interfaces[envIndex], "OpenGymVectorEnv lane is not bound to an interface");
        m_activeEnvIndex = envIndex;
        m_interfaces[envIndex]->SetGetGameOverCb(MakeCallback(&OpenGymEnv::GetGameOver, this));
        m_interfaces[envIndex]->SetGetObservationCb(MakeCallback(&OpenGymEnv::GetObservation, this));
        m_interfaces[envIndex]->SetGetRewardCb(MakeCallback(&OpenGymEnv::GetReward, this));
        m_interfaces[envIndex]->SetGetExtraInfoCb(MakeCallback(&OpenGymEnv::GetExtraInfo, this));
        m_interfaces[envIndex]->SetExecuteActionsCb(MakeCallback(&OpenGymEnv::ExecuteActions, this));
        m_interfaces[envIndex]->SendCurrentState();
    }
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        m_activeEnvIndex = envIndex;
        m_interfaces[envIndex]->ReceiveActions();
    }
}

void
OpenGymVectorEnv::NotifySimulationEnd(uint32_t envIndex)
{
    NS_LOG_FUNCTION(this << envIndex);
    ValidateEnvIndex(envIndex);
    NS_ASSERT_MSG(m_interfaces[envIndex], "OpenGymVectorEnv lane is not bound to an interface");
    m_interfaces[envIndex]->NotifySimulationEnd();
}

void
OpenGymVectorEnv::NotifySimulationEndAll()
{
    NS_LOG_FUNCTION(this);
    for (uint32_t envIndex = 0; envIndex < m_numEnvs; ++envIndex)
    {
        NotifySimulationEnd(envIndex);
    }
}

Ptr<OpenGymSpace>
OpenGymVectorEnv::GetActionSpace()
{
    return GetActionSpace(m_activeEnvIndex);
}

Ptr<OpenGymSpace>
OpenGymVectorEnv::GetObservationSpace()
{
    return GetObservationSpace(m_activeEnvIndex);
}

bool
OpenGymVectorEnv::GetGameOver()
{
    return GetGameOver(m_activeEnvIndex);
}

Ptr<OpenGymDataContainer>
OpenGymVectorEnv::GetObservation()
{
    return GetObservation(m_activeEnvIndex);
}

float
OpenGymVectorEnv::GetReward()
{
    return GetReward(m_activeEnvIndex);
}

std::string
OpenGymVectorEnv::GetExtraInfo()
{
    return GetExtraInfo(m_activeEnvIndex);
}

bool
OpenGymVectorEnv::ExecuteActions(Ptr<OpenGymDataContainer> action)
{
    return ExecuteActions(m_activeEnvIndex, action);
}

void
OpenGymVectorEnv::ValidateEnvIndex(uint32_t envIndex) const
{
    NS_ASSERT_MSG(envIndex < m_numEnvs, "vector environment index out of range");
}

void
OpenGymVectorEnv::BindInterface(uint32_t envIndex, Ptr<OpenGymInterface> interface)
{
    ValidateEnvIndex(envIndex);
    NS_ASSERT_MSG(interface, "cannot bind a null OpenGymInterface");
    m_interfaces[envIndex] = interface;
}

} // namespace ns3
