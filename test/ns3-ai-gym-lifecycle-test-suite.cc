/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3/ns3-ai-gym-interface.h"
#include "ns3/test.h"

using namespace ns3;

namespace
{

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
        AddTestCase(new ExplicitCreateObjectUnaffectedByResetTestCase, TestCase::QUICK);
        AddTestCase(new MultiCycleResetTestCase, TestCase::QUICK);
        AddTestCase(new DifferentPrefixesDoNotPolluteTestCase, TestCase::QUICK);
    }
};

static Ns3AiGymLifecycleTestSuite g_ns3AiGymLifecycleTestSuite;

} // namespace
