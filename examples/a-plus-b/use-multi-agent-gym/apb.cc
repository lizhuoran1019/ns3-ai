/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <ns3/ai-module.h>
#include <ns3/core-module.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ns3
{

class MultiAgentApbEnv : public OpenGymEnv
{
  public:
    explicit MultiAgentApbEnv(uint32_t agentId);
    ~MultiAgentApbEnv() override;
    static TypeId GetTypeId();
    void DoDispose() override;

    uint32_t GetAPlusB();
    uint32_t GetAgentId() const;

    // OpenGym interfaces:
    Ptr<OpenGymSpace> GetActionSpace() override;
    Ptr<OpenGymSpace> GetObservationSpace() override;
    bool GetGameOver() override;
    Ptr<OpenGymDataContainer> GetObservation() override;
    float GetReward() override;
    std::string GetExtraInfo() override;
    bool ExecuteActions(Ptr<OpenGymDataContainer> action) override;

    uint32_t m_a;
    uint32_t m_b;

  private:
    uint32_t m_agentId;
    uint32_t m_sum;
};

MultiAgentApbEnv::MultiAgentApbEnv(uint32_t agentId)
    : m_a(0),
      m_b(0),
      m_agentId(agentId),
      m_sum(0)
{
    SetOpenGymInterface("ns3ai-apb-agent-" + std::to_string(m_agentId));
}

MultiAgentApbEnv::~MultiAgentApbEnv()
{
}

TypeId
MultiAgentApbEnv::GetTypeId()
{
    static TypeId tid = TypeId("ns3::MultiAgentApbEnv")
                            .SetParent<OpenGymEnv>()
                            .SetGroupName("OpenGym");
    return tid;
}

void
MultiAgentApbEnv::DoDispose()
{
}

uint32_t
MultiAgentApbEnv::GetAPlusB()
{
    Notify();
    return m_sum;
}

uint32_t
MultiAgentApbEnv::GetAgentId() const
{
    return m_agentId;
}

Ptr<OpenGymSpace>
MultiAgentApbEnv::GetActionSpace()
{
    std::vector<uint32_t> shape = {1};
    std::string dtype = TypeNameGet<uint32_t>();
    Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace>(0, 20, shape, dtype);
    return box;
}

Ptr<OpenGymSpace>
MultiAgentApbEnv::GetObservationSpace()
{
    std::vector<uint32_t> shape = {3};
    std::string dtype = TypeNameGet<uint32_t>();
    Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace>(0, 10, shape, dtype);
    return box;
}

bool
MultiAgentApbEnv::GetGameOver()
{
    return false;
}

Ptr<OpenGymDataContainer>
MultiAgentApbEnv::GetObservation()
{
    std::vector<uint32_t> shape = {3};
    Ptr<OpenGymBoxContainer<uint32_t>> box = CreateObject<OpenGymBoxContainer<uint32_t>>(shape);

    box->AddValue(m_agentId);
    box->AddValue(m_a);
    box->AddValue(m_b);

    return box;
}

float
MultiAgentApbEnv::GetReward()
{
    return 0.0;
}

std::string
MultiAgentApbEnv::GetExtraInfo()
{
    return "agent=" + std::to_string(m_agentId);
}

bool
MultiAgentApbEnv::ExecuteActions(Ptr<OpenGymDataContainer> action)
{
    Ptr<OpenGymBoxContainer<uint32_t>> box = DynamicCast<OpenGymBoxContainer<uint32_t>>(action);
    m_sum = box->GetValue(0);
    return true;
}

} // namespace ns3

int
main(int argc, char* argv[])
{
    using namespace ns3;

    uint32_t numAgents = 3;
    uint32_t numSteps = 5;

    CommandLine cmd(__FILE__);
    cmd.AddValue("numAgents", "Number of independent Gym agents in one ns-3 process", numAgents);
    cmd.AddValue("numSteps", "Number of interaction rounds per agent", numSteps);
    cmd.Parse(argc, argv);

    std::vector<Ptr<MultiAgentApbEnv>> agents;
    agents.reserve(numAgents);
    for (uint32_t agentId = 0; agentId < numAgents; ++agentId)
    {
        agents.push_back(CreateObject<MultiAgentApbEnv>(agentId));
    }

    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> distrib(1, 10);

    for (uint32_t step = 0; step < numSteps; ++step)
    {
        for (auto agent : agents)
        {
            agent->m_a = distrib(gen);
            agent->m_b = distrib(gen);

            std::cout << "agent=" << agent->GetAgentId() << ";step=" << step << ";set="
                      << agent->m_a << "," << agent->m_b << ";" << std::endl;

            uint32_t sum = agent->GetAPlusB();

            std::cout << "agent=" << agent->GetAgentId() << ";step=" << step << ";get="
                      << sum << ";" << std::endl;
        }
    }

    for (auto agent : agents)
    {
        agent->NotifySimulationEnd();
    }

    return 0;
}
