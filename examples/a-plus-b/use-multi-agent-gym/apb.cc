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
#include <random>
#include <string>
#include <vector>

namespace ns3
{

class MultiAgentApbVecEnv : public OpenGymVectorEnv
{
  public:
    explicit MultiAgentApbVecEnv(uint32_t numAgents);
    ~MultiAgentApbVecEnv() override;
    static TypeId GetTypeId();
    void DoDispose() override;

    void SetOperands(uint32_t agentId, uint32_t a, uint32_t b);
    uint32_t GetAPlusB(uint32_t agentId) const;

    // OpenGymVectorEnv indexed interfaces:
    Ptr<OpenGymSpace> GetActionSpace(uint32_t envIndex) override;
    Ptr<OpenGymSpace> GetObservationSpace(uint32_t envIndex) override;
    bool GetGameOver(uint32_t envIndex) override;
    Ptr<OpenGymDataContainer> GetObservation(uint32_t envIndex) override;
    float GetReward(uint32_t envIndex) override;
    std::string GetExtraInfo(uint32_t envIndex) override;
    bool ExecuteActions(uint32_t envIndex, Ptr<OpenGymDataContainer> action) override;

  private:
    std::vector<uint32_t> m_a;
    std::vector<uint32_t> m_b;
    std::vector<uint32_t> m_sum;
};

MultiAgentApbVecEnv::MultiAgentApbVecEnv(uint32_t numAgents)
    : OpenGymVectorEnv(numAgents),
      m_a(numAgents, 0),
      m_b(numAgents, 0),
      m_sum(numAgents, 0)
{
    SetOpenGymInterfaces("ns3ai-apb-agent");
}

MultiAgentApbVecEnv::~MultiAgentApbVecEnv()
{
}

TypeId
MultiAgentApbVecEnv::GetTypeId()
{
    static TypeId tid = TypeId("ns3::MultiAgentApbVecEnv")
                            .SetParent<OpenGymVectorEnv>()
                            .SetGroupName("OpenGym");
    return tid;
}

void
MultiAgentApbVecEnv::DoDispose()
{
}

void
MultiAgentApbVecEnv::SetOperands(uint32_t agentId, uint32_t a, uint32_t b)
{
    m_a.at(agentId) = a;
    m_b.at(agentId) = b;
}

uint32_t
MultiAgentApbVecEnv::GetAPlusB(uint32_t agentId) const
{
    return m_sum.at(agentId);
}

Ptr<OpenGymSpace>
MultiAgentApbVecEnv::GetActionSpace(uint32_t envIndex)
{
    std::vector<uint32_t> shape = {1};
    std::string dtype = TypeNameGet<uint32_t>();
    Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace>(0, 20, shape, dtype);
    return box;
}

Ptr<OpenGymSpace>
MultiAgentApbVecEnv::GetObservationSpace(uint32_t envIndex)
{
    std::vector<uint32_t> shape = {3};
    std::string dtype = TypeNameGet<uint32_t>();
    Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace>(0, 10, shape, dtype);
    return box;
}

bool
MultiAgentApbVecEnv::GetGameOver(uint32_t envIndex)
{
    return false;
}

Ptr<OpenGymDataContainer>
MultiAgentApbVecEnv::GetObservation(uint32_t envIndex)
{
    std::vector<uint32_t> shape = {3};
    Ptr<OpenGymBoxContainer<uint32_t>> box = CreateObject<OpenGymBoxContainer<uint32_t>>(shape);

    box->AddValue(envIndex);
    box->AddValue(m_a.at(envIndex));
    box->AddValue(m_b.at(envIndex));

    return box;
}

float
MultiAgentApbVecEnv::GetReward(uint32_t envIndex)
{
    return 0.0;
}

std::string
MultiAgentApbVecEnv::GetExtraInfo(uint32_t envIndex)
{
    return "agent=" + std::to_string(envIndex);
}

bool
MultiAgentApbVecEnv::ExecuteActions(uint32_t envIndex, Ptr<OpenGymDataContainer> action)
{
    Ptr<OpenGymBoxContainer<uint32_t>> box = DynamicCast<OpenGymBoxContainer<uint32_t>>(action);
    m_sum.at(envIndex) = box->GetValue(0);
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
    cmd.AddValue("numAgents", "Number of vectorized Gym agents in one ns-3 process", numAgents);
    cmd.AddValue("numSteps", "Number of interaction rounds per agent", numSteps);
    cmd.Parse(argc, argv);

    Ptr<MultiAgentApbVecEnv> agents = CreateObject<MultiAgentApbVecEnv>(numAgents);
    agents->InitializeAll();

    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> distrib(1, 10);

    for (uint32_t step = 0; step < numSteps; ++step)
    {
        for (uint32_t agentId = 0; agentId < numAgents; ++agentId)
        {
            uint32_t a = distrib(gen);
            uint32_t b = distrib(gen);
            agents->SetOperands(agentId, a, b);

            std::cout << "agent=" << agentId << ";step=" << step << ";set=" << a << "," << b << ";"
                      << std::endl;
        }

        agents->NotifyAll();

        for (uint32_t agentId = 0; agentId < numAgents; ++agentId)
        {
            std::cout << "agent=" << agentId << ";step=" << step << ";get="
                      << agents->GetAPlusB(agentId) << ";" << std::endl;
        }
    }

    agents->NotifySimulationEndAll();

    return 0;
}
