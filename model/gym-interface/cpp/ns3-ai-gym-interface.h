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

#ifndef NS3_NS3_AI_GYM_INTERFACE_H
#define NS3_NS3_AI_GYM_INTERFACE_H

#include "../ns3-ai-gym-msg.h"

#include <ns3/ai-module.h>
#include <ns3/callback.h>
#include <ns3/object.h>
#include <ns3/ptr.h>
#include <ns3/type-id.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace ns3
{

class OpenGymSpace;
class OpenGymDataContainer;
class OpenGymEnv;

class OpenGymInterface : public Object
{
  public:
    static Ptr<OpenGymInterface> Get();
    static Ptr<OpenGymInterface> Get(uint32_t envId);
    static Ptr<OpenGymInterface> Get(const std::string& sharedMemoryPrefix);

    /**
     * \brief Clear the static singleton and named-interface registry.
     *
     * After calling Reset(), subsequent Get() and Get(prefix) calls create
     * fresh OpenGymInterface instances. Explicitly-owned Ptr<OpenGymInterface>
     * obtained before Reset() remain valid.
     *
     * Get() and Get(prefix) are legacy compatibility shims backed by
     * process-wide static storage. New code should prefer explicit
     * injection via OpenGymEnv::SetOpenGymInterface(Ptr<OpenGymInterface>).
     */
    static void Reset();

    /**
     * \brief Clear only the named-interface registry.
     *
     * The no-argument singleton Get() is unaffected.  After calling
     * ResetNamedInterfaces(), a subsequent Get(prefix) with the same
     * prefix creates a fresh interface.
     */
    static void ResetNamedInterfaces();

    OpenGymInterface();
    explicit OpenGymInterface(const std::string& sharedMemoryPrefix);
    ~OpenGymInterface() override;
    static TypeId GetTypeId();

    void SetSharedMemoryPrefix(const std::string& sharedMemoryPrefix);
    void SetSharedMemoryNames(std::string segmentName,
                              std::string cpp2pyMsgName,
                              std::string py2cppMsgName,
                              std::string lockableName);
    void SetSharedMemorySize(uint32_t size);

    void Init();
    void NotifyCurrentState();
    void SendCurrentState();
    void ReceiveActions();
    void WaitForStop();
    void NotifySimulationEnd();

    Ptr<OpenGymSpace> GetActionSpace();
    Ptr<OpenGymSpace> GetObservationSpace();
    Ptr<OpenGymDataContainer> GetObservation();
    float GetReward();
    bool IsTruncated();
    int32_t GetErrorCode();
    std::string GetErrorMessage();
    bool IsGameOver();
    std::string GetExtraInfo();
    bool ExecuteActions(Ptr<OpenGymDataContainer> action);

    void SetGetActionSpaceCb(Callback<Ptr<OpenGymSpace>> cb);
    void SetGetObservationSpaceCb(Callback<Ptr<OpenGymSpace>> cb);
    void SetGetObservationCb(Callback<Ptr<OpenGymDataContainer>> cb);
    void SetGetRewardCb(Callback<float> cb);
    void SetGetTruncatedCb(Callback<bool> cb);
    void SetGetErrorCodeCb(Callback<int32_t> cb);
    void SetGetErrorMessageCb(Callback<std::string> cb);
    void SetGetGameOverCb(Callback<bool> cb);
    void SetGetExtraInfoCb(Callback<std::string> cb);
    void SetExecuteActionsCb(Callback<bool, Ptr<OpenGymDataContainer>> cb);

    void Notify(Ptr<OpenGymEnv> entity);

  protected:
    // Inherited
    void DoInitialize() override;
    void DoDispose() override;

  private:
    static Ptr<OpenGymInterface>* DoGet();
    static std::unordered_map<std::string, Ptr<OpenGymInterface>>* DoGetNamedInterfaces();

    MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>* GetMsgInterface();
    uint64_t NextSequence();

    bool m_simEnd;
    bool m_stopEnvRequested;
    bool m_initSimMsgSent;
    bool m_stateAwaitingAction;
    uint32_t m_memorySize;
    uint64_t m_nextSequence;
    uint64_t m_pendingInitSequence;
    uint64_t m_pendingStateSequence;
    MailboxTransportNames m_msgNames;
    std::unique_ptr<MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>> m_msgInterface;

    Callback<Ptr<OpenGymSpace>> m_actionSpaceCb;
    Callback<Ptr<OpenGymSpace>> m_observationSpaceCb;
    Callback<bool> m_gameOverCb;
    Callback<Ptr<OpenGymDataContainer>> m_obsCb;
    Callback<float> m_rewardCb;
    Callback<bool> m_truncatedCb;
    Callback<int32_t> m_errorCodeCb;
    Callback<std::string> m_errorMessageCb;
    Callback<std::string> m_extraInfoCb;
    Callback<bool, Ptr<OpenGymDataContainer>> m_actionCb;
};

} // end of namespace ns3

#endif // NS3_NS3_AI_GYM_INTERFACE_H
