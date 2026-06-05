/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef NS3_NS3_AI_VECTOR_GYM_ENV_H
#define NS3_NS3_AI_VECTOR_GYM_ENV_H

#include "ns3-ai-gym-env.h"

#include <ns3/ptr.h>

#include <string>
#include <vector>

namespace ns3
{

class OpenGymDataContainer;
class OpenGymInterface;
class OpenGymSpace;

/**
 * \brief Base class for a synchronous vectorized Gym environment on the C++ side.
 *
 * The class exposes per-index virtual hooks and adapts them to the legacy
 * OpenGymEnv callback surface. A simulation can bind each vector lane to an
 * isolated OpenGymInterface while keeping one object responsible for vector
 * lifecycle and lane validation.
 */
class OpenGymVectorEnv : public OpenGymEnv
{
  public:
    explicit OpenGymVectorEnv(uint32_t numEnvs = 1);
    ~OpenGymVectorEnv() override;

    static TypeId GetTypeId();

    uint32_t GetNumEnvs() const;
    void SetNumEnvs(uint32_t numEnvs);

    void SetOpenGymInterfaces(const std::string& sharedMemoryPrefixBase);
    void SetOpenGymInterfaces(const std::vector<Ptr<OpenGymInterface>>& interfaces);

    Ptr<OpenGymInterface> GetOpenGymInterface(uint32_t envIndex) const;

    void Notify(uint32_t envIndex);
    void NotifyAll();
    void NotifySimulationEnd(uint32_t envIndex);
    void NotifySimulationEndAll();

    Ptr<OpenGymSpace> GetActionSpace() override;
    Ptr<OpenGymSpace> GetObservationSpace() override;
    bool GetGameOver() override;
    Ptr<OpenGymDataContainer> GetObservation() override;
    float GetReward() override;
    std::string GetExtraInfo() override;
    bool ExecuteActions(Ptr<OpenGymDataContainer> action) override;

    virtual Ptr<OpenGymSpace> GetActionSpace(uint32_t envIndex) = 0;
    virtual Ptr<OpenGymSpace> GetObservationSpace(uint32_t envIndex) = 0;
    virtual bool GetGameOver(uint32_t envIndex) = 0;
    virtual Ptr<OpenGymDataContainer> GetObservation(uint32_t envIndex) = 0;
    virtual float GetReward(uint32_t envIndex) = 0;
    virtual std::string GetExtraInfo(uint32_t envIndex) = 0;
    virtual bool ExecuteActions(uint32_t envIndex, Ptr<OpenGymDataContainer> action) = 0;

  protected:
    uint32_t GetActiveEnvIndex() const;

  private:
    void ValidateEnvIndex(uint32_t envIndex) const;
    void BindInterface(uint32_t envIndex, Ptr<OpenGymInterface> interface);

    uint32_t m_numEnvs;
    uint32_t m_activeEnvIndex;
    std::vector<Ptr<OpenGymInterface>> m_interfaces;
};

} // namespace ns3

#endif // NS3_NS3_AI_VECTOR_GYM_ENV_H
