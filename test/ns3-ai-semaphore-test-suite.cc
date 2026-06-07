/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3/ns3-ai-semaphore.h"

#include "ns3/test.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace ns3;

namespace
{

/**
 * \brief 信号量基本往返：sem_post 后 sem_try_wait 可获取令牌。
 */
class SemaphoreRoundTripTestCase : public TestCase
{
  public:
    SemaphoreRoundTripTestCase()
        : TestCase("binary semaphore round-trip via sem_post and sem_try_wait")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};

        Ns3AiSemaphore::sem_post(&counter);

        NS_TEST_EXPECT_MSG_EQ(counter.load(),
                              1,
                              "sem_post on empty counter sets it to 1");

        const bool acquired = Ns3AiSemaphore::sem_try_wait(&counter);

        NS_TEST_EXPECT_MSG_EQ(acquired, true, "sem_try_wait acquires the token after sem_post");
        NS_TEST_EXPECT_MSG_EQ(counter.load(), 0, "counter is back to 0 after successful wait");
    }
};

/**
 * \brief 计数器为 1 时 sem_post 饱和，不会递增到 2。
 */
class SemaphoreSaturationTestCase : public TestCase
{
  public:
    SemaphoreSaturationTestCase()
        : TestCase("sem_post on counter=1 saturates instead of incrementing to 2")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{1};

        const uint8_t old = Ns3AiSemaphore::sem_post(&counter);

        NS_TEST_EXPECT_MSG_EQ(old, 1, "sem_post on full counter returns old value 1");
        NS_TEST_EXPECT_MSG_EQ(counter.load(),
                              1,
                              "counter stays at 1 after sem_post on full counter");
    }
};

/**
 * \brief 计数器为 255 时 sem_post 不会回绕到 0。
 *
 * 这是 Issue #31 的抗回绕验收测试。
 */
class SemaphoreNoWrap255TestCase : public TestCase
{
  public:
    SemaphoreNoWrap255TestCase()
        : TestCase("sem_post on counter=255 does not wrap to 0")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{255};

        Ns3AiSemaphore::sem_post(&counter);

        NS_TEST_EXPECT_MSG_NE(counter.load(),
                              0,
                              "counter=255 after sem_post must not silently wrap to 0");
        NS_TEST_EXPECT_MSG_EQ(counter.load(),
                              1,
                              "counter=255 after sem_post is reset to 1 by exchange");
    }
};

/**
 * \brief 空计数器上 sem_try_wait 返回 false。
 */
class SemaphoreWaitEmptyTestCase : public TestCase
{
  public:
    SemaphoreWaitEmptyTestCase()
        : TestCase("sem_try_wait on empty counter returns false")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};

        const bool acquired = Ns3AiSemaphore::sem_try_wait(&counter);

        NS_TEST_EXPECT_MSG_EQ(acquired, false, "sem_try_wait on counter=0 returns false");
        NS_TEST_EXPECT_MSG_EQ(counter.load(), 0, "counter stays at 0 after failed wait");
    }
};

/**
 * \brief sem_timed_wait 在短超时后返回 WaitResult::Timeout。
 */
class SemaphoreTimeoutTestCase : public TestCase
{
  public:
    SemaphoreTimeoutTestCase()
        : TestCase("sem_timed_wait returns Timeout after the deadline expires")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};

        const auto start = std::chrono::steady_clock::now();
        const auto result = Ns3AiSemaphore::sem_timed_wait(&counter, 50000); // 50 ms
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();

        NS_TEST_EXPECT_MSG_EQ(result,
                              Ns3AiSemaphore::WaitResult::Timeout,
                              "short timeout on empty counter produces Timeout");
        NS_TEST_EXPECT_MSG_GT(static_cast<uint64_t>(elapsed_us),
                              (uint64_t)40000,
                              "wait lasted at least ~40 ms before timing out");
    }
};

/**
 * \brief sem_timed_wait 在中止标志触发后返回 WaitResult::Aborted。
 */
class SemaphoreAbortFlagTestCase : public TestCase
{
  public:
    SemaphoreAbortFlagTestCase()
        : TestCase("sem_timed_wait returns Aborted when the abort flag is set")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};
        std::atomic<bool> abort_flag{false};

        // 另一个线程在 10 ms 后设置中止标志
        std::thread trigger([&abort_flag]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            abort_flag.store(true, std::memory_order_release);
        });

        const auto result =
            Ns3AiSemaphore::sem_timed_wait(&counter, Ns3AiSemaphore::DEFAULT_SEM_WAIT_TIMEOUT_US, &abort_flag);
        trigger.join();

        NS_TEST_EXPECT_MSG_EQ(result,
                              Ns3AiSemaphore::WaitResult::Aborted,
                              "abort flag set by another thread produces Aborted");
    }
};

/**
 * \brief 带显式超时的 sem_wait 重载在超时后返回 WaitResult::Timeout。
 */
class SemaphoreWaitTimeoutOverloadTestCase : public TestCase
{
  public:
    SemaphoreWaitTimeoutOverloadTestCase()
        : TestCase("sem_wait with explicit short timeout returns Timeout")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};

        const auto result = Ns3AiSemaphore::sem_wait(&counter, 10000, nullptr); // 10 ms

        NS_TEST_EXPECT_MSG_EQ(result,
                              Ns3AiSemaphore::WaitResult::Timeout,
                              "explicit timeout overload returns Timeout when counter stays at 0");
    }
};

/**
 * \brief sem_wait 和 sem_timed_wait 在 timeout_us=0 时无限等待。
 *
 * 不实际测试无限等待——仅验证 timeout_us=0 不会立即超时，
 * 且传入 abort_flag 后可以正常退出。
 */
class SemaphoreNoTimeoutWithAbortTestCase : public TestCase
{
  public:
    SemaphoreNoTimeoutWithAbortTestCase()
        : TestCase("sem_timed_wait with timeout_us=0 waits unbounded until abort")
    {
    }

  private:
    void DoRun() override
    {
        std::atomic<uint8_t> counter{0};
        std::atomic<bool> abort_flag{false};

        std::thread trigger([&abort_flag]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            abort_flag.store(true, std::memory_order_release);
        });

        const auto result = Ns3AiSemaphore::sem_timed_wait(&counter, 0, &abort_flag);
        trigger.join();

        NS_TEST_EXPECT_MSG_EQ(result,
                              Ns3AiSemaphore::WaitResult::Aborted,
                              "timeout_us=0 with abort_flag exits via Aborted, not infinite loop");
    }
};

/**
 * \brief Ns3AiSemaphore 测试套件
 */
class Ns3AiSemaphoreTestSuite : public TestSuite
{
  public:
    Ns3AiSemaphoreTestSuite()
        : TestSuite("ns3-ai-semaphore", UNIT)
    {
        AddTestCase(new SemaphoreRoundTripTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreSaturationTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreNoWrap255TestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreWaitEmptyTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreTimeoutTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreAbortFlagTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreWaitTimeoutOverloadTestCase, TestCase::QUICK);
        AddTestCase(new SemaphoreNoTimeoutWithAbortTestCase, TestCase::QUICK);
    }
};

static Ns3AiSemaphoreTestSuite g_ns3AiSemaphoreTestSuite;

} // namespace
