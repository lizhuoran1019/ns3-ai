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

#include <unistd.h>

#include <exception>
#include <sstream>
#include <string>

using namespace ns3;

namespace
{

struct SessionLifecycleCppMsg
{
    uint32_t value;
};

struct SessionLifecyclePyMsg
{
    uint32_t value;
};

Ns3AiMsgInterfaceNames
MakeTestNames(const std::string& suffix)
{
    return Ns3AiMsgInterface::MakeNames("ns3-ai-session-lifecycle-test-" + suffix);
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

class SessionReadyHandshakeTestCase : public TestCase
{
  public:
    SessionReadyHandshakeTestCase()
        : TestCase("shared-memory session reaches READY after both peers join")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("ready"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Init,
                              "A single created peer has not completed the session handshake");

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Ready,
                              "Creator observes READY after the second peer joins");
        NS_TEST_EXPECT_MSG_EQ(opener.GetSessionState(),
                              Ns3AiMsgSessionState::Ready,
                              "Opener observes READY after joining the session");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionId(),
                              opener.GetSessionId(),
                              "Both peers observe the same session id");
        NS_TEST_EXPECT_MSG_EQ(creator.GetGenerationId(),
                              opener.GetGenerationId(),
                              "Both peers observe the same generation id");
        NS_TEST_EXPECT_MSG_NE(creator.GetSessionId(), 0, "Session id is explicit");
        NS_TEST_EXPECT_MSG_NE(creator.GetGenerationId(), 0, "Generation id is explicit");
    }
};

class SessionCloseHandshakeTestCase : public TestCase
{
  public:
    SessionCloseHandshakeTestCase()
        : TestCase("shared-memory session normal close requires peer acknowledgement")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("close"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        // 通过一次数据交换将会话状态推进到 RUNNING
        creator.CppSendBegin();
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Running,
                              "First data exchange moves the session to RUNNING");
        creator.CppSendEnd();

        opener.PyRecvBegin();
        opener.PyRecvEnd();

        // 从 RUNNING 状态发起关闭
        creator.RequestClose(Ns3AiMsgPeer::Cpp, Ns3AiMsgCloseReason::Normal);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closing,
                              "Close request moves the session to CLOSING");
        NS_TEST_EXPECT_MSG_EQ(opener.GetCloseReason(),
                              Ns3AiMsgCloseReason::Normal,
                              "Close reason is visible before acknowledgement");

        opener.AcknowledgeClose(Ns3AiMsgPeer::Py);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Creator observes CLOSED after peer acknowledgement");
        NS_TEST_EXPECT_MSG_EQ(opener.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Acknowledging peer observes CLOSED");
        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::Normal,
                              "Closed session keeps the structured close reason");
    }
};

class SessionTimeoutErrorTestCase : public TestCase
{
  public:
    SessionTimeoutErrorTestCase()
        : TestCase("shared-memory session timeout records structured error")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("timeout"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        NS_TEST_EXPECT_MSG_EQ(creator.TryCppRecvBegin(),
                              false,
                              "Receiving without matching Python send times out");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Timeout moves the session to ERROR");
        NS_TEST_EXPECT_MSG_EQ(opener.GetErrorReason(),
                              Ns3AiMsgErrorReason::Timeout,
                              "Peer observes timeout as structured error reason");
        NS_TEST_EXPECT_MSG_EQ(opener.GetLastErrorPeer(),
                              Ns3AiMsgPeer::Cpp,
                              "Peer observes which side reported the timeout");
    }
};

class SessionClosingBlocksDataExchangeTestCase : public TestCase
{
  public:
    SessionClosingBlocksDataExchangeTestCase()
        : TestCase("shared-memory session blocks data exchange while closing")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("closing-blocks-data"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        creator.RequestClose(Ns3AiMsgPeer::Cpp, Ns3AiMsgCloseReason::Normal);

        NS_TEST_EXPECT_MSG_EQ(creator.TryCppSendBegin(),
                              false,
                              "C++ try-send is rejected while the session is closing");
        NS_TEST_EXPECT_MSG_EQ(creator.TryCppRecvBegin(),
                              false,
                              "C++ try-recv is rejected while the session is closing");
        NS_TEST_EXPECT_MSG_EQ(opener.TryPySendBegin(),
                              false,
                              "Python try-send is rejected while the session is closing");
        NS_TEST_EXPECT_MSG_EQ(opener.TryPyRecvBegin(),
                              false,
                              "Python try-recv is rejected while the session is closing");

        bool blocked = false;
        try
        {
            creator.CppSendBegin();
        }
        catch (const std::runtime_error&)
        {
            blocked = true;
        }

        NS_TEST_EXPECT_MSG_EQ(blocked,
                              true,
                              "Blocking send is rejected immediately while the session is closing");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closing,
                              "Rejected data exchange does not overwrite the close state");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::None,
                              "Rejecting data exchange during close is not a peer error");
    }
};

class SessionPeerDeathErrorTestCase : public TestCase
{
  public:
    SessionPeerDeathErrorTestCase()
        : TestCase("shared-memory session peer death records structured error")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("peer-death"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        opener.ReportPeerDeath(Ns3AiMsgPeer::Cpp);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Peer death moves the session to ERROR");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::PeerDeath,
                              "Peer death is distinguishable from timeout");
        NS_TEST_EXPECT_MSG_EQ(creator.GetLastErrorPeer(),
                              Ns3AiMsgPeer::Cpp,
                              "The dead peer is recorded");
    }
};

class SessionStaleGenerationErrorTestCase : public TestCase
{
  public:
    SessionStaleGenerationErrorTestCase()
        : TestCase("shared-memory session stale generation records structured error")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("stale-generation"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        const auto staleGeneration = opener.GetGenerationId() + 1;

        NS_TEST_EXPECT_MSG_EQ(opener.CheckGenerationId(staleGeneration, Ns3AiMsgPeer::Py),
                              false,
                              "Generation mismatch is rejected");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Stale generation moves the session to ERROR");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::StaleGeneration,
                              "Stale generation has a distinct error reason");
        NS_TEST_EXPECT_MSG_EQ(creator.GetLastErrorPeer(),
                              Ns3AiMsgPeer::Py,
                              "The peer with the stale generation is recorded");
    }
};

class SessionProtocolMismatchErrorTestCase : public TestCase
{
  public:
    SessionProtocolMismatchErrorTestCase()
        : TestCase("shared-memory session protocol mismatch records structured error")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("protocol-mismatch"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            11,
            22);

        bool rejected = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
                false,
                false,
                true,
                4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                99,
                22);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }

        NS_TEST_EXPECT_MSG_EQ(rejected, true, "Mismatched protocol header is rejected");
        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Protocol mismatch moves the session to ERROR");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::ProtocolMismatch,
                              "Protocol mismatch has a distinct error reason");
        NS_TEST_EXPECT_MSG_EQ(creator.GetLastErrorPeer(),
                              Ns3AiMsgPeer::Py,
                              "The opening peer is recorded as the protocol mismatch reporter");
    }
};

class SessionUserInterruptedCloseIdempotentTestCase : public TestCase
{
  public:
    SessionUserInterruptedCloseIdempotentTestCase()
        : TestCase("shared-memory session user interruption close is idempotent")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("user-interrupted"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false,
            false,
            true,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str());

        opener.RequestClose(Ns3AiMsgPeer::Py, Ns3AiMsgCloseReason::UserInterrupted);
        creator.AcknowledgeClose(Ns3AiMsgPeer::Cpp);
        opener.RequestClose(Ns3AiMsgPeer::Py, Ns3AiMsgCloseReason::UserInterrupted);
        creator.AcknowledgeClose(Ns3AiMsgPeer::Cpp);

        NS_TEST_EXPECT_MSG_EQ(creator.GetSessionState(),
                              Ns3AiMsgSessionState::Closed,
                              "Repeated close operations keep the session CLOSED");
        NS_TEST_EXPECT_MSG_EQ(creator.GetCloseReason(),
                              Ns3AiMsgCloseReason::UserInterrupted,
                              "User interruption is distinguishable from normal close");
        NS_TEST_EXPECT_MSG_EQ(opener.GetErrorReason(),
                              Ns3AiMsgErrorReason::None,
                              "Completed user interruption close is not an error");
    }
};

/**
 * \brief TryCppRecvBegin 在对端标记 finish 后返回 false 且不标记 Timeout。
 *
 * TryCppRecvBegin 等待 m_py2cppFullCount（初始 0）时会检测 abort_flag。
 * 对端调用 CppSetFinished 后，TryCppRecvBegin 应返回 false（Aborted）
 * 而非标记 Timeout。
 */
class SessionTryBeginAbortNotTimeoutTestCase : public TestCase
{
  public:
    SessionTryBeginAbortNotTimeoutTestCase()
        : TestCase("TryCppRecvBegin returns false on finish without marking Timeout")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("try-abort"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(), names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(), names.m_lockableName.c_str(),
            1000000, names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false, false, true, 4096,
            names.m_segmentName.c_str(), names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(), names.m_lockableName.c_str(),
            1000000, names.m_headerName.c_str());

        // 对端标记 finished，m_isFinished=true 且 m_cppState=Finished
        creator.CppSetFinished();

        // TryCppRecvBegin:
        // TransitionPeer(Cpp, Ready, Receiving) 失败（m_cppState=Finished）
        // → 返回 false，不进入 WaitForSync
        // 这验证：当 peer state 因 finish 改变后，Try*Begin 不会假成功
        const bool result = opener.TryCppRecvBegin();
        NS_TEST_EXPECT_MSG_EQ(result, false,
                              "TryCppRecvBegin returns false when peer state is Finished");

        // 不应标记为 Timeout
        NS_TEST_EXPECT_MSG_NE(opener.GetErrorReason(),
                              Ns3AiMsgErrorReason::Timeout,
                              "Aborted by finish must not be recorded as Timeout");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::None,
                              "Finishing peer has no error");

        // 验证 opener 没有进入 Error 状态
        NS_TEST_EXPECT_MSG_NE(opener.GetSessionState(),
                              Ns3AiMsgSessionState::Error,
                              "Opener session is not Error after finish abort");
    }
};

/**
 * \brief 超时异常消息包含 operation、wait target、counter 值、timeout us、对端状态。
 */
class SessionTimeoutDiagnosticTextTestCase : public TestCase
{
  public:
    SessionTimeoutDiagnosticTextTestCase()
        : TestCase("timeout exception includes diagnostic fields")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("timeout-diag"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(), names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(), names.m_lockableName.c_str(),
            1000, names.m_headerName.c_str());
        Ns3AiMsgInterfaceImpl<SessionLifecycleCppMsg, SessionLifecyclePyMsg> opener(
            false, false, true, 4096,
            names.m_segmentName.c_str(), names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(), names.m_lockableName.c_str(),
            1000, names.m_headerName.c_str());

        bool caughtTimeout = false;
        bool hasOperation = false;
        bool hasWaitTarget = false;
        bool hasCounter = false;
        bool hasTimeout = false;
        bool hasCppState = false;
        bool hasPyState = false;
        try
        {
            // 发送端没写数据，接收端 CppRecvBegin 应超时
            creator.CppRecvBegin();
        }
        catch (const std::runtime_error& e)
        {
            caughtTimeout = true;
            const std::string msg = e.what();

            hasOperation = msg.find("CppRecvBegin") != std::string::npos;
            hasWaitTarget = msg.find("py2cpp full slot") != std::string::npos;
            hasCounter = msg.find("counter=") != std::string::npos;
            hasTimeout = msg.find("timeout=1000") != std::string::npos;
            hasCppState = msg.find("C++=") != std::string::npos;
            hasPyState = msg.find("Python=") != std::string::npos;
        }

        NS_TEST_EXPECT_MSG_EQ(hasOperation, true, "message contains the operation name");
        NS_TEST_EXPECT_MSG_EQ(hasWaitTarget, true, "message contains the wait target name");
        NS_TEST_EXPECT_MSG_EQ(hasCounter, true, "message contains counter value");
        NS_TEST_EXPECT_MSG_EQ(hasTimeout, true, "message contains timeout value in us");
        NS_TEST_EXPECT_MSG_EQ(hasCppState, true, "message contains C++ peer state");
        NS_TEST_EXPECT_MSG_EQ(hasPyState, true, "message contains Python peer state");

        NS_TEST_EXPECT_MSG_EQ(caughtTimeout, true,
                              "CppRecvBegin throws on timeout");
        NS_TEST_EXPECT_MSG_EQ(creator.GetErrorReason(),
                              Ns3AiMsgErrorReason::Timeout,
                              "Timeout is recorded as Timeout error");
    }
};

class MsgInterfaceSessionLifecycleTestSuite : public TestSuite
{
  public:
    MsgInterfaceSessionLifecycleTestSuite()
        : TestSuite("ns3-ai-msg-interface-session-lifecycle", UNIT)
    {
        AddTestCase(new SessionReadyHandshakeTestCase, TestCase::QUICK);
        AddTestCase(new SessionCloseHandshakeTestCase, TestCase::QUICK);
        AddTestCase(new SessionTimeoutErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionClosingBlocksDataExchangeTestCase, TestCase::QUICK);
        AddTestCase(new SessionPeerDeathErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionStaleGenerationErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionProtocolMismatchErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionUserInterruptedCloseIdempotentTestCase, TestCase::QUICK);
        AddTestCase(new SessionTryBeginAbortNotTimeoutTestCase, TestCase::QUICK);
        AddTestCase(new SessionTimeoutDiagnosticTextTestCase, TestCase::QUICK);
    }
};

static MsgInterfaceSessionLifecycleTestSuite g_msgInterfaceSessionLifecycleTestSuite;

} // namespace
