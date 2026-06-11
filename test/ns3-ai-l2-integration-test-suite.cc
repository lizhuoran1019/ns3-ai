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

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
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

// ---------- Fork 与子进程退出码检查 ----------

/**
 * \brief fork 并执行子进程逻辑，子进程结束时应 _exit(code)。
 * \return 子进程 pid（仅在父进程中返回）。
 *
 * 注意：不要在 ForkChild 内使用 NS_TEST_* 宏（它们包含 return/continue，
 * 不能在独立函数中正确工作）。
 */
pid_t
ForkChild()
{
    pid_t pid = fork();
    if (pid == -1)
    {
        std::cerr << "L2 test: fork() failed" << std::endl;
        std::abort();
    }
    return pid;
}

/**
 * \brief 等待子进程退出并检查退出码。
 * \return 子进程正常退出且退出码匹配 expectedCode 时返回 true。
 */
bool
WaitChild(pid_t pid, int expectedCode = 0)
{
    int status;
    const pid_t ret = waitpid(pid, &status, 0);
    if (ret == -1)
    {
        std::cerr << "L2 test: waitpid(" << pid << ") failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    if (!WIFEXITED(status))
    {
        std::cerr << "L2 test: child pid=" << pid << " terminated by signal "
                  << WTERMSIG(status) << " (expected code " << expectedCode << ")"
                  << std::endl;
        return false;
    }
    if (WEXITSTATUS(status) != expectedCode)
    {
        std::cerr << "L2 test: child pid=" << pid << " exited with code "
                  << WEXITSTATUS(status) << " (expected " << expectedCode << ")"
                  << std::endl;
        return false;
    }
    return true;
}

/**
 * \brief RAII guard：父进程析构时确保子进程被 kill + wait，避免僵尸。
 *
 * 当父进程因断言提前返回时，子进程可能变成孤儿/僵尸。
 * 策略：SIGTERM → 2 秒内轮询 waitpid(WNOHANG) → 未退出则 SIGKILL → blocking waitpid。
 */
class ChildGuard
{
  public:
    explicit ChildGuard(pid_t pid)
        : m_pid(pid)
    {
    }

    ~ChildGuard()
    {
        if (m_pid <= 0)
        {
            return;
        }
        // 先 SIGTERM 请求正常退出
        kill(m_pid, SIGTERM);
        // 轮询等待子进程退出，最多 2 秒
        constexpr int MAX_RETRIES = 200;
        int status;
        for (int i = 0; i < MAX_RETRIES; i++)
        {
            pid_t ret;
            do
            {
                ret = waitpid(m_pid, &status, WNOHANG);
            } while (ret == -1 && errno == EINTR);
            if (ret == m_pid)
            {
                return; // 已退出
            }
            if (ret == -1)
            {
                return; // waitpid 异常
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // 超时仍未退出 → 强制 SIGKILL
        kill(m_pid, SIGKILL);
        pid_t ret;
        do
        {
            ret = waitpid(m_pid, &status, 0);
        } while (ret == -1 && errno == EINTR);
    }

    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;

    pid_t GetPid() const
    {
        return m_pid;
    }

    void Release()
    {
        m_pid = -1;
    }

  private:
    pid_t m_pid;
};


// ---------- 会话状态等待 ----------

/**
 * \brief 等待会话达到 Ready 或 Running（即可以正常交换数据的状态）。
 *
 * 显式枚举成功状态：只接受 Ready / Running。
 * Error、Closed、Closing 均视为异常并立即返回 false，
 * 避免将异常路径误判为活跃。
 *
 * \return 达到 Ready 或 Running 返回 true；进入异常状态或超时返回 false。
 */
bool
WaitForSessionActive(Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg>& iface,
                     uint64_t timeoutUs)
{
    const auto start = std::chrono::steady_clock::now();
    while (true)
    {
        const auto state = iface.GetSessionState();
        if (state == Ns3AiMsgSessionState::Ready ||
            state == Ns3AiMsgSessionState::Running)
        {
            return true;
        }
        // Error / Closed / Closing 均视为失败
        if (state != Ns3AiMsgSessionState::Init)
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

/**
 * \brief 等待精确目标状态，或检测到 Error 后立即返回失败。
 *
 * 用于等待稳定的终态（Closing、Closed）。不适用于等待 Ready，
 * 因为子进程可能快速越过 Ready。
 *
 * \return target 状态达到返回 true；进入 Error 或超时返回 false。
 */
bool
WaitForExactStateOrError(Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg>& iface,
                         Ns3AiMsgSessionState target,
                         uint64_t timeoutUs)
{
    const auto start = std::chrono::steady_clock::now();
    while (true)
    {
        const auto state = iface.GetSessionState();
        if (state == target)
        {
            return true;
        }
        // Error 永远是异常终态，不应视为成功
        if (state == Ns3AiMsgSessionState::Error)
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

// ================================================================
// 测试用例
// ================================================================

/**
 * \brief 无 opener 加入时 TryCppSendBegin 立即返回 false。
 *
 * 对应 #62"no opener joins"语义。当前实现中 session 处于 Init
 * 无法进入数据交换，TryCppSendBegin 在 TryBeginDataExchange 阶段
 * 直接返回 false（不进入 WaitForSync）。
 */
class NoOpenerJoinRejectedTestCase : public TestCase
{
  public:
    NoOpenerJoinRejectedTestCase()
        : TestCase("no opener joins: TryCppSendBegin returns false (session Init)")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-no-opener"));
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

        // 没有 fork，没有 opener
        const bool result = creator.TryCppSendBegin();

        NS_TEST_EXPECT_MSG_EQ(result,
                              false,
                              "TryCppSendBegin returns false when session is Init");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Init,
                              "Session stays Init (no Error, no timeout)");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::None,
                              "No error reason recorded for Init rejection");
    }
};

/**
 * \brief Opener 加入后立即退出的 recv timeout。
 *
 * Creator 创建共享内存，opener 加入后立即退出（不交换数据），
 * Creator TryCppRecvBegin 等待 py2cpp 数据超时 -> Timeout。
 */
class IpcRecvTimeoutAfterOpenerExitTestCase : public TestCase
{
  public:
    IpcRecvTimeoutAfterOpenerExitTestCase()
        : TestCase("IPC recv timeout: opener joins then exits, creator recv times out")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-recv-timeout"));
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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

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
            _exit(0);
        }

        // Parent: 等待 session 脱离 Init（child 打开后可能直接到 Running）
        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(active,
                              true,
                              "Session must leave Init after child opens the segment");

        const bool result = creator.TryCppRecvBegin();

        NS_TEST_EXPECT_MSG_EQ(result, false, "TryCppRecvBegin returns false on timeout");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Timeout moves session to Error");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::Timeout,
                              "Error reason is Timeout when no data arrives");

        guard.Release();
        NS_TEST_EXPECT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly");
    }
};

/**
 * \brief 完整 struct payload 跨进程往返。
 *
 * CppSendBegin → CppSendEnd → PyRecvBegin → PyRecvEnd → PySendBegin → PySendEnd → CppRecvBegin → CppRecvEnd
 * 验证 Running 状态在往返全程保持。
 */
class RunningStateReachableTestCase : public TestCase
{
  public:
    RunningStateReachableTestCase()
        : TestCase("L2 Running state: full data exchange across subprocess boundary")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 5000000;
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

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

            const bool recvOk = opener.TryPyRecvBegin();
            if (!recvOk)
            {
                _exit(2);
            }
            const uint32_t received = opener.GetCpp2PyStruct()->value;
            opener.PyRecvEnd();

            opener.PySendBegin();
            opener.GetPy2CppStruct()->value = received + 1;
            opener.PySendEnd();

            _exit(0);
        }

        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(active,
                              true,
                              "Session must leave Init after child opens");

        constexpr uint32_t SENT_VALUE = 42;
        creator.CppSendBegin();
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Running,
                              "First CppSendBegin moves session to Running");
        creator.GetCpp2PyStruct()->value = SENT_VALUE;
        creator.CppSendEnd();

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

        guard.Release();
        NS_TEST_EXPECT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit with code 0");
    }
};

/**
 * \brief 关闭握手：Cpp 请求关闭，Py 确认。
 *
 * 子进程使用带 timeout 的轮询等待 Closing 状态，避免无限等待。
 */
class CloseHandshakeCppToPyTestCase : public TestCase
{
  public:
    CloseHandshakeCppToPyTestCase()
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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

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

            // 等待 Cpp 发起关闭（带 timeout）
            const bool closing =
                WaitForExactStateOrError(opener, Ns3AiMsgSessionState::Closing, POLL_TIMEOUT_US);
            if (!closing)
            {
                _exit(2);
            }
            if (opener.GetCloseReason() != Ns3AiMsgCloseReason::Normal)
            {
                _exit(3); // 意外 close reason
            }
            opener.AcknowledgeClose(Ns3AiMsgPeer::Py);
            _exit(0);
        }

        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(active,
                              true,
                              "Session must leave Init before close");

        creator.RequestClose(Ns3AiMsgPeer::Cpp, Ns3AiMsgCloseReason::Normal);

        guard.Release();
        NS_TEST_ASSERT_MSG_EQ(WaitChild(pid, 0), true, "Child must ack close and exit cleanly");

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Creator observes Closed after peer acknowledges");
        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::Normal,
                              "Closed session keeps the Normal close reason");
    }
};

/**
 * \brief 关闭握手：Py 请求关闭，Cpp 确认。
 *
 * 子进程使用带 timeout 的轮询等待 Closed 状态。
 */
class CloseHandshakePyToCppTestCase : public TestCase
{
  public:
    CloseHandshakePyToCppTestCase()
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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

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

            // 等待 Cpp 确认（带 timeout）
            const bool closed =
                WaitForExactStateOrError(opener, Ns3AiMsgSessionState::Closed, POLL_TIMEOUT_US);
            if (!closed)
            {
                _exit(2);
            }
            _exit(0);
        }

        // 子进程打开后立即 RequestClose，session 直接从 Init→Ready→Closing，
        // 因此不能等 Ready/Running——直接等 Closing。
        const bool closing =
            WaitForExactStateOrError(creator, Ns3AiMsgSessionState::Closing, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(closing,
                              true,
                              "Session must reach Closing (from child RequestClose)");

        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::UserInterrupted,
                              "Creator observes UserInterrupted close reason");
        creator.AcknowledgeClose(Ns3AiMsgPeer::Cpp);

        guard.Release();
        NS_TEST_ASSERT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly after close");

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Creator observes Closed after acknowledging");
    }
};

/**
 * \brief Schema Strict 模式：匹配 metadata → opener 成功加入。
 */
class SchemaStrictMatchTestCase : public TestCase
{
  public:
    SchemaStrictMatchTestCase()
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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

        if (pid == 0)
        {
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

        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_EXPECT_MSG_EQ(active,
                              true,
                              "Matching metadata allows opener to join");

        guard.Release();
        NS_TEST_EXPECT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly");
    }
};

/**
 * \brief Schema Strict 模式：不匹配的 cpp2py hash → opener 构造抛出 Ns3AiSchemaError。
 *
 * 子进程 catch Ns3AiSchemaError 后以退出码 0 表示验证通过。
 * 父进程验证 creator 侧 session 进入 Error + reason = ProtocolMismatch。
 */
class SchemaStrictMismatchTestCase : public TestCase
{
  public:
    SchemaStrictMismatchTestCase()
        : TestCase("L2 schema strict: mismatched hash rejects opener with ProtocolMismatch")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-schema-mismatch"));
        RemoveSegment(names);

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

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

        if (pid == 0)
        {
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

        guard.Release();
        NS_TEST_ASSERT_MSG_EQ(WaitChild(pid, 0),
                              true,
                              "Child must catch Ns3AiSchemaError (exit code 0)");

        // 子进程的 ThrowSchemaHeaderFailure → MarkPeerError(Py, ProtocolMismatch)
        // 已在子进程 catch 前写入共享内存，waitpid 后父进程可见
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Schema mismatch moves creator session to Error");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::ProtocolMismatch,
                              "Error reason is ProtocolMismatch when schema hash differs");
    }
};

/**
 * \brief Baseline peer early exit：creator 在 session Ready 前退出。
 *
 * 子进程创建裸共享内存段（仅 Boost managed_shared_memory，无 ns3-ai 对象），
 * 然后 _exit() 模拟崩溃。父进程作为 opener 打开同一段时找不到
 * 协议对象 → RequireNonNull 抛出 Ns3AiRuntimeError。
 *
 * 测试结束时父进程显式 RemoveSegment 防止泄漏。
 */
class PeerEarlyExitBaselineTestCase : public TestCase
{
  public:
    PeerEarlyExitBaselineTestCase()
        : TestCase("L2 peer early exit baseline: creator exits before init, opener throws")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 100000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-early-exit-base"));
        RemoveSegment(names);

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

        if (pid == 0)
        {
            // Child: 只创建裸 Boost 共享内存段，不初始化任何 ns3-ai 协议对象
            boost::interprocess::shared_memory_object::remove(
                names.m_segmentName.c_str());
            boost::interprocess::managed_shared_memory seg(
                boost::interprocess::create_only,
                names.m_segmentName.c_str(),
                4096);
            // seg 构造时创建了共享内存但内部为空（无 header/sync）
            // _exit 不运行析构函数 → 段在 /dev/shm 中保留
            _exit(0);
        }

        guard.Release();
        NS_TEST_ASSERT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly");

        // Parent: 尝试作为 opener 打开 → 段存在但找不到对象
        bool caughtRuntimeError = false;
        try
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
        }
        catch (const Ns3AiRuntimeError&)
        {
            caughtRuntimeError = true;
        }

        NS_TEST_EXPECT_MSG_EQ(caughtRuntimeError,
                              true,
                              "Opener must throw Ns3AiRuntimeError when shared-memory objects are missing");

        // 显式清理共享内存
        RemoveSegment(names);
    }
};

/**
 * \brief Silent peer death：真实 Python heartbeat publisher 停止后 C++ WaitForSync 检测 PeerDeath。
 *
 * 子进程以 50ms 间隔发布 heartbeat，持续 600ms 后退出。
 * 父进程在子进程发布 heartbeat 期间进入 TryCppRecvBegin → WaitForSync，
 * 观察到 Python heartbeat 活跃，然后子进程退出 → heartbeat 停止 →
 * CheckPeerHeartbeat 检测 stall → PeerDeath。
 */
class HeartbeatSilentPeerDeathTestCase : public TestCase
{
  public:
    HeartbeatSilentPeerDeathTestCase()
        : TestCase("L2 heartbeat silent peer death: publisher stops, C++ detects PeerDeath")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 2000000;  // 2s — > 心跳检测
        constexpr uint64_t HB_PERIOD_US = 100000;      // 100ms
        constexpr uint64_t HB_TIMEOUT_US = 300000;     // 300ms
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-hb-peerdeath"));
        RemoveSegment(names);

        // Parent 作为 creator / C++ 等待方
        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            HB_PERIOD_US, HB_TIMEOUT_US);

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

        if (pid == 0)
        {
            // Child 作为 opener / Python heartbeat publisher
            Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                SYNC_TIMEOUT_US,
                names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
                HB_PERIOD_US, HB_TIMEOUT_US);

            // 以 50ms 间隔发布 12 次 heartbeat（共 600ms），然后静默退出
            for (int i = 0; i < 12; i++)
            {
                opener.HeartbeatPublish();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            _exit(0);
        }

        // Parent: 等待 child 打开
        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_EXPECT_MSG_EQ(active,
                              true,
                              "Session must be Ready or Running before heartbeat test");

        // 进入 C++ wait 模式：WaitForSync 会检查 Python heartbeat counter
        // child 在前 600ms 活跃地发布 heartbeat，然后退出
        // CheckPeerHeartbeat 在 child 退出后经 HB_TIMEOUT_US (300ms) 检测 stall
        const bool result = creator.TryCppRecvBegin();

        NS_TEST_EXPECT_MSG_EQ(result,
                              false,
                              "TryCppRecvBegin returns false on PeerDeath");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "PeerDeath moves session to Error");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::PeerDeath,
                              "Error reason is PeerDeath when heartbeat publisher stops");

        guard.Release();
        NS_TEST_EXPECT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly");
    }
};

/**
 * \brief Heartbeat normal maintenance：长时间正常会话窗口内验证无 PeerDeath 误报。
 *
 * 子进程先以 50ms 间隔显式 HeartbeatPublish() 持续 800ms（远超 2×HB_TIMEOUT_US=600ms），
 * 然后进入 TryPyRecvBegin 接收数据并回复。
 * 父进程在子进程 heartbeat 活跃期间进入 TryCppRecvBegin → WaitForSync，
 * 持续观察 Python heartbeat 保持活跃 → 不应触发 PeerDeath。
 * 数据交换完成后验证 ErrorReason == None。
 */
class HeartbeatNormalMaintenanceTestCase : public TestCase
{
  public:
    HeartbeatNormalMaintenanceTestCase()
        : TestCase("L2 heartbeat normal maintenance: >2×HB_TIMEOUT window without false PeerDeath")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint64_t SYNC_TIMEOUT_US = 2000000; // 2s
        constexpr uint64_t HB_PERIOD_US = 100000;     // 100ms
        constexpr uint64_t HB_TIMEOUT_US = 300000;    // 300ms
        constexpr uint64_t POLL_TIMEOUT_US = 5000000;

        const auto names = MakeTestNames(MakeUniqueSuffix("l2-hb-maintenance"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<L2CppMsg, L2PyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            SYNC_TIMEOUT_US,
            names.m_headerName.c_str(), 0, 0, 0, 0, Ns3AiSchemaValidationMode::Compatibility,
            HB_PERIOD_US, HB_TIMEOUT_US);

        const pid_t pid = ForkChild();
        ChildGuard guard(pid);

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
                HB_PERIOD_US, HB_TIMEOUT_US);

            // 发布 heartbeat 800ms（> 2×HB_TIMEOUT_US），模拟正常长期活跃
            for (int i = 0; i < 16; i++)
            {
                opener.HeartbeatPublish();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // 接收父进程发送的数据
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

        const bool active =
            WaitForSessionActive(creator, POLL_TIMEOUT_US);
        NS_TEST_ASSERT_MSG_EQ(active,
                              true,
                              "Session must become active before data exchange");

        // 先发送数据给子进程
        constexpr uint32_t SENT_VALUE = 42;
        creator.CppSendBegin();
        creator.GetCpp2PyStruct()->value = SENT_VALUE;
        creator.CppSendEnd();

        // 进入 WaitForSync 接收回复
        // 此时子进程仍在 heartbeat 循环（800ms 还未结束），Python heartbeat 持续活跃
        // WaitForSync 中的 CheckPeerHeartbeat 应反复观察到增量的 m_pyHeartbeatCounter
        const bool recvOk = creator.TryCppRecvBegin();
        NS_TEST_EXPECT_MSG_EQ(recvOk, true, "CppRecvBegin must succeed after child sends response");
        if (recvOk)
        {
            NS_TEST_EXPECT_MSG_EQ(creator.GetPy2CppStruct()->value,
                                  SENT_VALUE + 1,
                                  "Child echoed back value+1");
            creator.CppRecvEnd();
        }

        // 验证整个正常维护窗口内没有触发 PeerDeath
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Running,
                              "Session remains Running after sustained heartbeat window");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::None,
                              "No error reason (no false PeerDeath during >2×HB_TIMEOUT window)");

        guard.Release();
        NS_TEST_EXPECT_MSG_EQ(WaitChild(pid, 0), true, "Child must exit cleanly");
    }
};


// ================================================================
// TestSuite 注册
// ================================================================

class L2IntegrationTestSuite : public TestSuite
{
  public:
    L2IntegrationTestSuite()
        : TestSuite("ns3-ai-l2-integration", UNIT)
    {
        AddTestCase(new NoOpenerJoinRejectedTestCase, TestCase::QUICK);
        AddTestCase(new IpcRecvTimeoutAfterOpenerExitTestCase, TestCase::QUICK);
        AddTestCase(new RunningStateReachableTestCase, TestCase::QUICK);
        AddTestCase(new CloseHandshakeCppToPyTestCase, TestCase::QUICK);
        AddTestCase(new CloseHandshakePyToCppTestCase, TestCase::QUICK);
        AddTestCase(new SchemaStrictMatchTestCase, TestCase::QUICK);
        AddTestCase(new SchemaStrictMismatchTestCase, TestCase::QUICK);
        AddTestCase(new PeerEarlyExitBaselineTestCase, TestCase::QUICK);
        AddTestCase(new HeartbeatSilentPeerDeathTestCase, TestCase::QUICK);
        AddTestCase(new HeartbeatNormalMaintenanceTestCase, TestCase::QUICK);
    }
};

static L2IntegrationTestSuite g_l2IntegrationTestSuite;

} // namespace
