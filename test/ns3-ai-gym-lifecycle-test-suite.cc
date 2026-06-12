/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3/ns3-ai-gym-interface.h"
#include "ns3/container.h"
#include "ns3/spaces.h"
#include "ns3/test.h"

#include "messages.pb.h"

#include <boost/interprocess/shared_memory_object.hpp>

#include <chrono>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace ns3;

namespace
{

constexpr uint32_t GYM_TEST_SHM_SIZE = 1048576;

std::string
MakeUniqueSuffix(const char* name)
{
    std::ostringstream oss;
    oss << getpid() << "-" << name;
    return oss.str();
}

void
RemoveSegment(const MailboxTransportNames& names)
{
    boost::interprocess::shared_memory_object::remove(names.m_segmentName.c_str());
}

bool
WaitForSessionActive(MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg>& iface, uint64_t timeoutUs)
{
    const auto start = std::chrono::steady_clock::now();
    while (true)
    {
        const auto state = iface.GetSessionState();
        if (state == TransportSessionState::Ready || state == TransportSessionState::Running)
        {
            return true;
        }
        if (state != TransportSessionState::Init)
        {
            return false;
        }
        if (timeoutUs > 0)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
            if (static_cast<uint64_t>(elapsed.count()) >= timeoutUs)
            {
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Ptr<OpenGymSpace>
MakeDiscreteSpace(int32_t n)
{
    return CreateObject<OpenGymDiscreteSpace>(n);
}

Ptr<OpenGymDataContainer>
MakeDiscreteData(int32_t value)
{
    auto data = CreateObject<OpenGymDiscreteContainer>();
    data->SetValue(value);
    return data;
}

Ptr<OpenGymSpace>
GetLifecycleTestObservationSpace()
{
    return MakeDiscreteSpace(2);
}

Ptr<OpenGymSpace>
GetLifecycleTestActionSpace()
{
    return MakeDiscreteSpace(2);
}

Ptr<OpenGymDataContainer>
GetLifecycleTestObservation()
{
    return MakeDiscreteData(1);
}

float
GetLifecycleTestReward()
{
    return 0.0f;
}

bool
GetLifecycleTestGameOver()
{
    return true;
}

bool
GetLifecycleTestTruncated()
{
    return false;
}

int32_t
GetLifecycleTestErrorCode()
{
    return 0;
}

std::string
GetLifecycleTestErrorMessage()
{
    return "";
}

std::string
GetLifecycleTestExtraInfo()
{
    return "";
}

bool
ExecuteLifecycleTestAction(Ptr<OpenGymDataContainer>)
{
    return true;
}

/**
 * \brief Reset() 清除无参 singleton Get()，后续 Get() 返回新实例。
 */
class ResetClearsSingletonTestCase : public TestCase
{
  public:
    ResetClearsSingletonTestCase()
        : TestCase("Reset() clears singleton Get() and returns a fresh instance")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> first = OpenGymInterface::Get();
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> second = OpenGymInterface::Get();
        NS_TEST_EXPECT_MSG_NE(first,
                              second,
                              "Reset() should clear singleton so Get() returns a new instance");
    }
};

/**
 * \brief Reset() 同时清除 named registry，同 prefix 后续 Get() 返回新实例。
 */
class ResetClearsNamedRegistryTestCase : public TestCase
{
  public:
    ResetClearsNamedRegistryTestCase()
        : TestCase("Reset() clears named registry so Get(prefix) returns a fresh instance")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> first = OpenGymInterface::Get("test-prefix-a");
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> second = OpenGymInterface::Get("test-prefix-a");
        NS_TEST_EXPECT_MSG_NE(first,
                              second,
                              "Reset() should clear named registry so Get(prefix) returns a new instance");
    }
};

/**
 * \brief ResetNamedInterfaces() 不影响无参 singleton Get()。
 */
class ResetNamedInterfacesDoesNotAffectSingletonTestCase : public TestCase
{
  public:
    ResetNamedInterfacesDoesNotAffectSingletonTestCase()
        : TestCase("ResetNamedInterfaces() does not affect the no-argument singleton Get()")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> first = OpenGymInterface::Get();
        OpenGymInterface::ResetNamedInterfaces();
        Ptr<OpenGymInterface> second = OpenGymInterface::Get();
        NS_TEST_EXPECT_MSG_EQ(first,
                              second,
                              "ResetNamedInterfaces() should not replace the singleton");
    }
};

/**
 * \brief ResetNamedInterfaces() 清除 named registry，同 prefix 后续 Get() 返回新实例。
 */
class ResetNamedInterfacesClearsRegistryTestCase : public TestCase
{
  public:
    ResetNamedInterfacesClearsRegistryTestCase()
        : TestCase("ResetNamedInterfaces() clears named registry so Get(prefix) returns a fresh instance")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> first = OpenGymInterface::Get("test-reset-named");
        OpenGymInterface::ResetNamedInterfaces();
        Ptr<OpenGymInterface> second = OpenGymInterface::Get("test-reset-named");
        NS_TEST_EXPECT_MSG_NE(first,
                              second,
                              "ResetNamedInterfaces() should clear named registry so Get(prefix) returns a fresh instance");
    }
};

/**
 * \brief 显式 CreateObject<OpenGymInterface> 不受 Reset() 影响。
 */
class ExplicitCreateObjectUnaffectedByResetTestCase : public TestCase
{
  public:
    ExplicitCreateObjectUnaffectedByResetTestCase()
        : TestCase("explicitly created OpenGymInterface is unaffected by Reset()")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> explicitPtr = CreateObject<OpenGymInterface>("test-explicit");
        OpenGymInterface::Reset();

        // 显式持有的指针在 Reset() 后仍然有效
        NS_TEST_EXPECT_MSG_NE(explicitPtr, nullptr,
                              "explicitly owned Ptr remains valid after Reset()");

        // Reset() 后 Get() 返回的是新实例，不是之前显式创建的那个
        Ptr<OpenGymInterface> fromRegistry = OpenGymInterface::Get("test-explicit");
        NS_TEST_EXPECT_MSG_NE(explicitPtr, fromRegistry,
                              "Get(prefix) after Reset() returns a fresh instance, not the old explicit one");
    }
};

/**
 * \brief 多轮 reset/get 不重用旧状态。
 */
class MultiCycleResetTestCase : public TestCase
{
  public:
    MultiCycleResetTestCase()
        : TestCase("multiple Reset/Get cycles never return stale instances")
    {
    }

  private:
    void DoRun() override
    {
        // Singleton: 三轮 reset/get, 每轮指针都不同
        Ptr<OpenGymInterface> s1 = OpenGymInterface::Get();
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> s2 = OpenGymInterface::Get();
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> s3 = OpenGymInterface::Get();
        NS_TEST_EXPECT_MSG_NE(s1, s2, "singleton round 1 vs 2 differ");
        NS_TEST_EXPECT_MSG_NE(s2, s3, "singleton round 2 vs 3 differ");
        NS_TEST_EXPECT_MSG_NE(s1, s3, "singleton round 1 vs 3 differ");

        // Named registry: 三轮 reset/get, 每轮指针都不同
        Ptr<OpenGymInterface> n1 = OpenGymInterface::Get("test-multi");
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> n2 = OpenGymInterface::Get("test-multi");
        OpenGymInterface::Reset();
        Ptr<OpenGymInterface> n3 = OpenGymInterface::Get("test-multi");
        NS_TEST_EXPECT_MSG_NE(n1, n2, "named round 1 vs 2 differ");
        NS_TEST_EXPECT_MSG_NE(n2, n3, "named round 2 vs 3 differ");
        NS_TEST_EXPECT_MSG_NE(n1, n3, "named round 1 vs 3 differ");
    }
};

/**
 * \brief 不同 prefix 的 Get(prefix) 返回不同实例。
 */
class DifferentPrefixesDoNotPolluteTestCase : public TestCase
{
  public:
    DifferentPrefixesDoNotPolluteTestCase()
        : TestCase("different Get(prefix) calls return distinct instances")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<OpenGymInterface> a = OpenGymInterface::Get("prefix-alpha");
        Ptr<OpenGymInterface> b = OpenGymInterface::Get("prefix-beta");
        NS_TEST_EXPECT_MSG_NE(a, b,
                              "different prefixes produce distinct OpenGymInterface instances");
    }
};

/**
 * \brief simulation end 不覆盖真实 task termination。
 */
class SimulationEndDoesNotOverrideTerminationTestCase : public TestCase
{
  public:
    SimulationEndDoesNotOverrideTerminationTestCase()
        : TestCase("NotifySimulationEnd preserves task termination over simulation end")
    {
    }

  private:
    void DoRun() override
    {
        const std::string prefix = "ns3-ai-gym-lifecycle-test-" + MakeUniqueSuffix("sim-end-priority");
        const auto names = MailboxTransport::MakeNames(prefix);
        RemoveSegment(names);

        MailboxTransportImpl<Ns3AiGymMsg, Ns3AiGymMsg> pythonSide(
            true,
            false,
            false,
            GYM_TEST_SHM_SIZE,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(),
            NS3_AI_GYM_MSG_SCHEMA_HASH,
            NS3_AI_GYM_MSG_SCHEMA_HASH,
            NS3_AI_GYM_MSG_SCHEMA_VERSION,
            NS3_AI_GYM_MSG_SCHEMA_VERSION,
            Ns3AiSchemaValidationMode::Strict);

        Ptr<OpenGymInterface> interface = CreateObject<OpenGymInterface>(prefix);
        interface->SetSharedMemorySize(GYM_TEST_SHM_SIZE);

        interface->SetGetObservationSpaceCb(MakeCallback(&GetLifecycleTestObservationSpace));
        interface->SetGetActionSpaceCb(MakeCallback(&GetLifecycleTestActionSpace));
        interface->SetGetObservationCb(MakeCallback(&GetLifecycleTestObservation));
        interface->SetGetRewardCb(MakeCallback(&GetLifecycleTestReward));
        interface->SetGetGameOverCb(MakeCallback(&GetLifecycleTestGameOver));
        interface->SetGetTruncatedCb(MakeCallback(&GetLifecycleTestTruncated));
        interface->SetGetErrorCodeCb(MakeCallback(&GetLifecycleTestErrorCode));
        interface->SetGetErrorMessageCb(MakeCallback(&GetLifecycleTestErrorMessage));
        interface->SetGetExtraInfoCb(MakeCallback(&GetLifecycleTestExtraInfo));
        interface->SetExecuteActionsCb(MakeCallback(&ExecuteLifecycleTestAction));

        std::thread creator([&]() {
            interface->Init();
            interface->NotifySimulationEnd();
        });

        const bool sessionActive = WaitForSessionActive(pythonSide, 1000000);
        NS_TEST_ASSERT_MSG_EQ(sessionActive,
                              true,
                              "python-side session must become active before receiving Gym messages");

        pythonSide.PyRecvBegin();
        ns3_ai_gym::SimInitMsg simInitMsg;
        simInitMsg.ParseFromArray(pythonSide.GetCpp2PyStruct()->buffer,
                                  static_cast<int>(pythonSide.GetCpp2PyStruct()->size));
        pythonSide.PyRecvEnd();

        ns3_ai_gym::SimInitAck simInitAck;
        simInitAck.set_done(true);
        simInitAck.set_stopsimreq(false);
        simInitAck.set_sequence(simInitMsg.sequence());
        pythonSide.PySendBegin();
        pythonSide.GetPy2CppStruct()->size = static_cast<uint32_t>(simInitAck.ByteSizeLong());
        simInitAck.SerializeToArray(pythonSide.GetPy2CppStruct()->buffer,
                                    static_cast<int>(pythonSide.GetPy2CppStruct()->size));
        pythonSide.PySendEnd();

        pythonSide.PyRecvBegin();
        ns3_ai_gym::EnvStateMsg envStateMsg;
        envStateMsg.ParseFromArray(pythonSide.GetCpp2PyStruct()->buffer,
                                   static_cast<int>(pythonSide.GetCpp2PyStruct()->size));
        pythonSide.PyRecvEnd();

        ns3_ai_gym::EnvActMsg envActMsg;
        envActMsg.set_sequence(envStateMsg.sequence());
        pythonSide.PySendBegin();
        pythonSide.GetPy2CppStruct()->size = static_cast<uint32_t>(envActMsg.ByteSizeLong());
        envActMsg.SerializeToArray(pythonSide.GetPy2CppStruct()->buffer,
                                   static_cast<int>(pythonSide.GetPy2CppStruct()->size));
        pythonSide.PySendEnd();

        creator.join();

        NS_TEST_EXPECT_MSG_EQ(envStateMsg.terminated(),
                              true,
                              "task termination must remain true when simulation end is also set");
        NS_TEST_EXPECT_MSG_EQ(envStateMsg.truncated(),
                              false,
                              "simulation end must not force truncation when task already terminated");
        NS_TEST_EXPECT_MSG_EQ(envStateMsg.reason(),
                              ns3_ai_gym::EnvStateMsg::GameOver,
                              "reason must remain GameOver for task termination");
    }
};

/**
 * \brief 注册测试套件。
 */
class Ns3AiGymLifecycleTestSuite : public TestSuite
{
  public:
    Ns3AiGymLifecycleTestSuite()
        : TestSuite("ns3-ai-gym-lifecycle", UNIT)
    {
        AddTestCase(new ResetClearsSingletonTestCase, TestCase::QUICK);
        AddTestCase(new ResetClearsNamedRegistryTestCase, TestCase::QUICK);
        AddTestCase(new ResetNamedInterfacesDoesNotAffectSingletonTestCase, TestCase::QUICK);
        AddTestCase(new ResetNamedInterfacesClearsRegistryTestCase, TestCase::QUICK);
        AddTestCase(new ExplicitCreateObjectUnaffectedByResetTestCase, TestCase::QUICK);
        AddTestCase(new MultiCycleResetTestCase, TestCase::QUICK);
        AddTestCase(new DifferentPrefixesDoNotPolluteTestCase, TestCase::QUICK);
        AddTestCase(new SimulationEndDoesNotOverrideTerminationTestCase, TestCase::QUICK);
    }
};

static Ns3AiGymLifecycleTestSuite g_ns3AiGymLifecycleTestSuite;

} // namespace
