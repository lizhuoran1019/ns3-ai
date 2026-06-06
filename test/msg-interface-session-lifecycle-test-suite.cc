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

class MsgInterfaceSessionLifecycleTestSuite : public TestSuite
{
  public:
    MsgInterfaceSessionLifecycleTestSuite()
        : TestSuite("ns3-ai-msg-interface-session-lifecycle", UNIT)
    {
        AddTestCase(new SessionReadyHandshakeTestCase, TestCase::QUICK);
        AddTestCase(new SessionCloseHandshakeTestCase, TestCase::QUICK);
        AddTestCase(new SessionTimeoutErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionPeerDeathErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionStaleGenerationErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionProtocolMismatchErrorTestCase, TestCase::QUICK);
        AddTestCase(new SessionUserInterruptedCloseIdempotentTestCase, TestCase::QUICK);
    }
};

static MsgInterfaceSessionLifecycleTestSuite g_msgInterfaceSessionLifecycleTestSuite;

} // namespace
