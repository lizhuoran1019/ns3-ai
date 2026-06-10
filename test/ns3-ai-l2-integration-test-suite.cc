/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3/ns3-ai-msg-interface.h"
#include "ns3/test.h"

#include <boost/interprocess/shared_memory_object.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <exception>
#include <sstream>
#include <string>
#include <thread>

using namespace ns3;

namespace
{

struct L2CppMsg
{
    uint32_t value;
};

struct L2PyMsg
{
    uint32_t value;
};

Ns3AiMsgInterfaceNames
MakeTestNames(const std::string& suffix)
{
    return Ns3AiMsgInterface::MakeNames("ns3-ai-l2-integration-" + suffix);
}

std::string
MakeUniqueSuffix(const char* name)
{
    std::ostringstream oss;
    oss << getpid() << "-" << name;
    return oss.str();
}

void
RemoveSegment(const Ns3AiMsgInterfaceNames& names)
{
    boost::interprocess::shared_memory_object::remove(names.m_segmentName.c_str());
}

/**
 * \brief 轮询 session state，直到达到或超过期望状态，或超时。
 *
 * 使用 >= 比较（Init < Ready < Running < Closing < Closed < Error），
 * 避免细粒度轮询错过中间状态跃迁。
 *
 * \return 超时前达到期望状态返回 true，否则 false。
 */
bool
WaitForSessionState(Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg>& iface,
                    Ns3AiMsgSessionState minExpected,
                    uint64_t timeoutUs)
{
    const auto start = std::chrono::steady_clock::now();
    while (static_cast<uint8_t>(iface.GetSessionState()) < static_cast<uint8_t>(minExpected))
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        if (timeoutUs > 0 && static_cast<uint64_t>(elapsed.count()) >= timeoutUs)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/**
 * \brief L2 IPC timeout real path。
 *
 * Creator 创建共享内存，opener 打开后立即退出（不交换数据），
 * Creator TryCppRecvBegin 超时 -> Timeout。
 */
class L2IpcTimeoutRealPathTestCase : public TestCase
{
  public:
    L2IpcTimeoutRealPathTestCase()
        : TestCase("L2 IPC timeout: subprocess opener exits without data exchange")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;  // 100 ms
        constexpr uint64_t POLL_TIMEOUT_US = 5000000; // 5 s

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-timeout"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            0, // heartbeat_period_us = 0 (禁用)
            0  // heartbeat_timeout_us = 0 (禁用)
        );

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            // Child: 以 opener 身份打开共享内存段，然后立即退出
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                0, 0);
            _exit(0);
        }

        // Parent: 等待 child 打开（session -> Ready）
        const bool ready =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(ready,
                              true,
                              "Session must reach Ready after child opens the segment");

        // child 已退出，没有数据到来 -> 超时
        const bool result = creator.TryCppRecvBegin();

        NS_TEST_EXPECT_MSG_EQ(result, false, "TryCppRecvBegin returns false on timeout");

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Timeout moves session to Error");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::Timeout,
                              "Error reason is Timeout when no data arrives");

        int status;
        waitpid(pid, &status, 0);
    }
};

/**
 * \brief L2 Running state reachable。
 *
 * 完整 struct payload 往返：CppSend → PyRecv → PySend → CppRecv，
 * 验证跨越进程边界的双工数据交换。
 */
class L2RunningStateReachableTestCase : public TestCase
{
  public:
    L2RunningStateReachableTestCase()
        : TestCase("L2 Running state: full data exchange across subprocess boundary")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 5000000; // 5 s — 足够完成往返
        constexpr uint64_t POLL_TIMEOUT_US = 5000000; // 5 s

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-running"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            0, 0);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                0, 0);

            // 接收 C++ 发送的数据
            const bool recvOk = opener.TryPyRecvBegin();
            if (!recvOk)
            {
                _exit(2);
            }
            const uint32_t received = opener.GetCpp2PyStruct()->value;
            opener.PyRecvEnd();

            // 发送回复
            opener.PySendBegin();
            opener.GetPy2CppStruct()->value = received + 1;
            opener.PySendEnd();

            _exit(0);
        }

        // Parent: 等待 child 打开（>= Ready，包括 Running 子状态）
        const bool ready =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(ready,
                              true,
                              "Session must reach Ready before data exchange");

        // 发送数据
        constexpr uint32_t SENT_VALUE = 42;
        creator.CppSendBegin();
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Running,
                              "First CppSendBegin moves session to Running");
        creator.GetCpp2PyStruct()->value = SENT_VALUE;
        creator.CppSendEnd();

        // 接收 child 的回复
        const bool recvOk = creator.TryCppRecvBegin();
        NS_TEST_EXPECT_MSG_EQ(recvOk, true, "CppRecvBegin must succeed after child sends response");
        if (recvOk)
        {
            NS_TEST_EXPECT_MSG_EQ(creator.GetPy2CppStruct()->value,
                                  SENT_VALUE + 1,
                                  "Child echoed back value+1");
            creator.CppRecvEnd();
        }

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Running,
                              "Session remains Running after full data exchange");

        int status;
        waitpid(pid, &status, 0);
    }
};

/**
 * \brief L2 关闭握手（从各侧发起）。
 *
 * Test A: Cpp 请求关闭 → Py 确认。
 * Test B: Py 请求关闭 → Cpp 确认。
 */
class L2CloseHandshakeCppToPyTestCase : public TestCase
{
  public:
    L2CloseHandshakeCppToPyTestCase()
        : TestCase("L2 close handshake: Cpp requests close, Py acknowledges")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 5000000;
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-close-cpp2py"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            0, 0);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                0, 0);

            // 等待 Cpp 发起关闭
            while (opener.GetSessionState() != Ns3AiMsgSessionState::Closing)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            NS_TEST_EXPECT_MSG_EQ(opener.GetCloseReason(),
                                  Ns3AiMsgCloseReason::Normal,
                                  "Opener sees Normal close reason");
            opener.AcknowledgeClose(Ns3AiMsgPeer::Py);
            _exit(0);
        }

        // Parent: 等待 Ready
        const bool ready =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(ready, true, "Session must reach Ready");

        creator.RequestClose(Ns3AiMsgPeer::Cpp, Ns3AiMsgCloseReason::Normal);

        int status;
        waitpid(pid, &status, 0);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Creator observes Closed after peer acknowledges");
        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::Normal,
                              "Closed session keeps the Normal close reason");
    }
};

class L2CloseHandshakePyToCppTestCase : public TestCase
{
  public:
    L2CloseHandshakePyToCppTestCase()
        : TestCase("L2 close handshake: Py requests close, Cpp acknowledges")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 5000000;
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-close-py2cpp"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            0, 0);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                0, 0);

            opener.RequestClose(Ns3AiMsgPeer::Py, Ns3AiMsgCloseReason::UserInterrupted);

            // 等待 Cpp 确认
            while (opener.GetSessionState() != Ns3AiMsgSessionState::Closed)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            _exit(0);
        }

        // Parent: 等待 Ready，再等待 Closing（child 发起的关闭）
        const bool ready =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(ready, true, "Session must reach Ready");

        const bool closing =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Closing, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(closing, true, "Session must reach Closing (from child RequestClose)");

        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::UserInterrupted,
                              "Creator observes UserInterrupted close reason");
        creator.AcknowledgeClose(Ns3AiMsgPeer::Cpp);

        int status;
        waitpid(pid, &status, 0);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Creator observes Closed after acknowledging");
    }
};

/**
 * \brief L2 Schema Strict mode end-to-end。
 *
 * 匹配的 metadata → opener 构造成功；
 * 不匹配的 hash → opener 构造失败（ProtocolMismatch）。
 */
class L2SchemaStrictMatchTestCase : public TestCase
{
  public:
    L2SchemaStrictMatchTestCase()
        : TestCase("L2 schema strict: matching metadata allows opener to join")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-schema-match"));
        RemoveSegment(names);

        // Creator: Strict 模式，non-zero metadata
        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(),
            0xAAAA, // cpp2py_schema_hash
            0xBBBB, // py2cpp_schema_hash
            1,      // cpp2py_schema_version
            1,      // py2cpp_schema_version
            Ns3AiSchemaValidationMode::Strict);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            // Opener: 匹配的 metadata → 应成功
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(),
                0xAAAA, 0xBBBB, 1, 1,
                Ns3AiSchemaValidationMode::Strict);
            _exit(0);
        }

        const bool ready =
            WaitForSessionState(creator, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_EXPECT_MSG_EQ(ready,
                              true,
                              "Matching metadata allows opener to join (session reaches Ready)");

        int status;
        waitpid(pid, &status, 0);
    }
};

class L2SchemaStrictMismatchTestCase : public TestCase
{
  public:
    L2SchemaStrictMismatchTestCase()
        : TestCase("L2 schema strict: mismatched hash rejects opener with ProtocolMismatch")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-schema-mismatch"));
        RemoveSegment(names);

        // Creator: Strict 模式，cpp2py_schema_hash=0xAAAA
        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(),
            0xAAAA, 0xBBBB, 1, 1,
            Ns3AiSchemaValidationMode::Strict);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            // Opener: 不匹配的 cpp2py hash → 构造应抛出异常
            bool caughtSchemaError = false;
            try
            {
                Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                    false, false, true, 4096,
                    names.m_segmentName.c_str(),
                    names.m_cpp2pyMsgName.c_str(),
                    names.m_py2cppMsgName.c_str(),
                    names.m_lockableName.c_str(),
                    SYNC_TIMEOUT_US,
                    names.m_headerName.c_str(),
                    0x9999, 0xBBBB, 1, 1,
                    Ns3AiSchemaValidationMode::Strict);
            }
            catch (const Ns3AiSchemaError&)
            {
                caughtSchemaError = true;
            }
            _exit(caughtSchemaError ? 0 : 1);
        }

        int status;
        waitpid(pid, &status, 0);

        NS_TEST_EXPECT_MSG_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                              true,
                              "Child must exit with code 0 (schema error caught)");

        // 子进程 catch 后 waitpid 确保 shared memory 写入已完成
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Schema mismatch moves creator session to Error");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::ProtocolMismatch,
                              "Error reason is ProtocolMismatch when schema hash differs");
    }
};

/**
 * \brief L2 Peer early exit（ns-3 subprocess 在 session Ready 前退出）。
 *
 * Child 作为 creator 创建共享内存后立即 _exit(0)（模拟 ns-3 进程崩溃），
 * Parent 作为 opener 打开后检测 Child 已死（heartbeat 检测到 PeerDeath）。
 */
class L2PeerEarlyExitTestCase : public TestCase
{
  public:
    L2PeerEarlyExitTestCase()
        : TestCase("L2 peer early exit: creator exits before session Ready, opener detects PeerDeath")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 600000;   // 600 ms
        constexpr uint64_t HB_PERIOD_US = 100000;      // 100 ms
        constexpr uint64_t HB_TIMEOUT_US = 300000;     // 300 ms
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;  // 5 s

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-early-exit"));
        RemoveSegment(names);

        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_NE(pid, -1, "fork() must succeed");

        if (pid == 0)
        {
            // Child: 作为 creator 创建共享内存后立即退出
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
                true, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                HB_PERIOD_US, HB_TIMEOUT_US);
            _exit(0);
        }

        // Parent: 等待 child 退出后打开已创建的共享内存
        int status;
        waitpid(pid, &status, 0);

        // Parent 作为 opener 打开 segment。child 已完成初始化，session 可达 Ready。
        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> parent(
            false, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            HB_PERIOD_US, HB_TIMEOUT_US);

        const bool ready =
            WaitForSessionState(parent, Ns3AiMsgSessionState::Ready, POLL_TIMEOUT_US);
        NS_TEST_EXPECT_MSG_EQ(ready,
                              true,
                              "Session reaches Ready despite creator early exit");

        // 尝试接收数据 → child 已死，heartbeat 应检测到 PeerDeath
        const bool result = parent.TryCppRecvBegin();

        NS_TEST_EXPECT_MSG_EQ(result, false,
                              "TryCppRecvBegin returns false on PeerDeath");
        NS_TEST_EXPECT_MSG_EQ(parent.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "PeerDeath detection moves session to Error");
        NS_TEST_EXPECT_MSG_EQ(parent.GetErrorReason(),
                              Ns3AiMsgErrorReason::PeerDeath,
                              "Error reason is PeerDeath when creator is gone");
    }
};

class L2IntegrationTestSuite : public TestSuite
{
  public:
    L2IntegrationTestSuite()
        : TestSuite("ns3-ai-l2-integration", UNIT)
    {
        AddTestCase(new L2IpcTimeoutRealPathTestCase, TestCase::QUICK);
        AddTestCase(new L2RunningStateReachableTestCase, TestCase::QUICK);
        AddTestCase(new L2CloseHandshakeCppToPyTestCase, TestCase::QUICK);
        AddTestCase(new L2CloseHandshakePyToCppTestCase, TestCase::QUICK);
        AddTestCase(new L2SchemaStrictMatchTestCase, TestCase::QUICK);
        AddTestCase(new L2SchemaStrictMismatchTestCase, TestCase::QUICK);
        AddTestCase(new L2PeerEarlyExitTestCase, TestCase::QUICK);
    }
};

static L2IntegrationTestSuite g_l2IntegrationTestSuite;

} // namespace
