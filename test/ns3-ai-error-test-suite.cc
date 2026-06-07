/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include "ns3/ns3-ai-errors.h"
#include "ns3/ns3-ai-msg-interface.h"
#include "ns3/ns3-ai-vector-gym-env.h"
#include "ns3/spaces.h"
#include "ns3/container.h"
#include "ns3/test.h"

#include <boost/interprocess/shared_memory_object.hpp>

#include <exception>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace ns3;

namespace
{

/**
 * \brief 唯一化共享内名称段，避免并行运行冲突。
 */
std::string
MakeUniqueSuffix(const char* name)
{
    std::ostringstream oss;
    oss << getpid() << "-" << name;
    return oss.str();
}

Ns3AiMsgInterfaceNames
MakeTestNames(const std::string& suffix)
{
    return Ns3AiMsgInterface::MakeNames("ns3-ai-error-test-" + suffix);
}

void
RemoveSegment(const Ns3AiMsgInterfaceNames& names)
{
    boost::interprocess::shared_memory_object::remove(names.m_segmentName.c_str());
}

struct ErrorTestCppMsg
{
    uint32_t value;
};

struct ErrorTestPyMsg
{
    uint32_t value;
};

/**
 * \brief Ns3AiError 可构造并携带消息。
 */
class Ns3AiErrorConstructTestCase : public TestCase
{
  public:
    Ns3AiErrorConstructTestCase()
        : TestCase("Ns3AiError and subclasses carry messages and are caught by base type")
    {
    }

  private:
    void DoRun() override
    {
        // Ns3AiError 本身
        Ns3AiError baseErr("base error");
        NS_TEST_EXPECT_MSG_EQ(std::string(baseErr.what()),
                              "base error",
                              "Ns3AiError carries the message");

        // 子类
        Ns3AiRuntimeError runtimeErr("runtime failure");
        NS_TEST_EXPECT_MSG_EQ(std::string(runtimeErr.what()),
                              "runtime failure",
                              "Ns3AiRuntimeError carries the message");

        Ns3AiTimeoutError timeoutErr("timeout");
        NS_TEST_EXPECT_MSG_EQ(std::string(timeoutErr.what()),
                              "timeout",
                              "Ns3AiTimeoutError carries the message");

        Ns3AiProtocolError protocolErr("protocol violation");
        NS_TEST_EXPECT_MSG_EQ(std::string(protocolErr.what()),
                              "protocol violation",
                              "Ns3AiProtocolError carries the message");

        Ns3AiSchemaError schemaErr("schema mismatch");
        NS_TEST_EXPECT_MSG_EQ(std::string(schemaErr.what()),
                              "schema mismatch",
                              "Ns3AiSchemaError carries the message");
    }
};

/**
 * \brief 子类可被 std::runtime_error 和 Ns3AiError 捕获。
 */
class Ns3AiErrorCatchByBaseTestCase : public TestCase
{
  public:
    Ns3AiErrorCatchByBaseTestCase()
        : TestCase("exception subclasses are caught by std::runtime_error and Ns3AiError")
    {
    }

  private:
    void DoRun() override
    {
        // 被 std::runtime_error 捕获
        bool caughtByRuntimeError = false;
        try
        {
            throw Ns3AiRuntimeError("test");
        }
        catch (const std::runtime_error&)
        {
            caughtByRuntimeError = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtByRuntimeError,
                              true,
                              "Ns3AiRuntimeError is caught by std::runtime_error");

        // 被 Ns3AiError 捕获
        bool caughtByNs3AiError = false;
        try
        {
            throw Ns3AiProtocolError("test");
        }
        catch (const Ns3AiError&)
        {
            caughtByNs3AiError = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtByNs3AiError,
                              true,
                              "Ns3AiProtocolError is caught by Ns3AiError");
    }
};

/**
 * \brief 在 vector-mode 接口上调用 struct getter 抛 Ns3AiProtocolError。
 */
class AccessModeVectorCallStructGetterTestCase : public TestCase
{
  public:
    AccessModeVectorCallStructGetterTestCase()
        : TestCase("vector-mode interface throws on struct getter access")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("access-vec-struct"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, true, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
            false, true, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        // vector 模式掉用 struct getter → Ns3AiProtocolError
        bool caughtProtocolError = false;
        try
        {
            creator.GetCpp2PyStruct();
        }
        catch (const Ns3AiProtocolError& e)
        {
            caughtProtocolError = true;
            const std::string msg = e.what();
            NS_TEST_EXPECT_MSG_NE(msg.find("GetCpp2PyStruct"), std::string::npos,
                                  "error message mentions the getter name");
            NS_TEST_EXPECT_MSG_NE(msg.find("vector"), std::string::npos,
                                  "error message mentions the configured mode");
        }
        NS_TEST_EXPECT_MSG_EQ(caughtProtocolError, true,
                              "GetCpp2PyStruct throws Ns3AiProtocolError in vector mode");

        bool caughtProtocolError2 = false;
        try
        {
            creator.GetPy2CppStruct();
        }
        catch (const Ns3AiProtocolError&)
        {
            caughtProtocolError2 = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtProtocolError2, true,
                              "GetPy2CppStruct throws Ns3AiProtocolError in vector mode");
    }
};

/**
 * \brief 在 struct-mode 接口上调用 vector getter 抛 Ns3AiProtocolError。
 */
class AccessModeStructCallVectorGetterTestCase : public TestCase
{
  public:
    AccessModeStructCallVectorGetterTestCase()
        : TestCase("struct-mode interface throws on vector getter access")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("access-struct-vec"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
            false, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        // struct 模式下调 vector getter → Ns3AiProtocolError
        bool caughtProtocolError = false;
        try
        {
            creator.GetCpp2PyVector();
        }
        catch (const Ns3AiProtocolError&)
        {
            caughtProtocolError = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtProtocolError, true,
                              "GetCpp2PyVector throws Ns3AiProtocolError in struct mode");

        bool caughtProtocolError2 = false;
        try
        {
            creator.GetPy2CppVector();
        }
        catch (const Ns3AiProtocolError&)
        {
            caughtProtocolError2 = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtProtocolError2, true,
                              "GetPy2CppVector throws Ns3AiProtocolError in struct mode");
    }
};

/**
 * \brief open-only 构造中缺失 vector 共享内存对象抛 Ns3AiRuntimeError。
 *
 * Creator 用 vector 模式、msg name "vec-msg" 构建对象，
 * Opener 用 vector 模式、msg name "other-msg"（不同名称）→ vector 找不到。
 */
class OpenOnlyMissingVectorObjectTestCase : public TestCase
{
  public:
    OpenOnlyMissingVectorObjectTestCase()
        : TestCase("open-only constructor throws on missing vector objects")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("missing-vec"));
        RemoveSegment(names);

        // Creator: vector 模式，msg 名称 "vec-cpp2py"/"vec-py2cpp"
        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, true, false,
            4096,
            names.m_segmentName.c_str(),
            "vec-cpp2py",
            "vec-py2cpp",
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        // Opener: vector 模式，msg 名称 "other-cpp2py"/"other-py2cpp"（不同）→ 找不到
        bool caught = false;
        try
        {
            Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
                false, true, false,
                4096,
                names.m_segmentName.c_str(),
                "other-cpp2py",
                "other-py2cpp",
                names.m_lockableName.c_str(),
                1000000,
                names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "Missing vector objects in open-only constructor throws Ns3AiRuntimeError");
    }
};

/**
 * \brief open-only 构造中缺失 struct 共享内存对象抛 Ns3AiRuntimeError。
 *
 * Creator 用 struct 模式、msg name "stru-msg" 构建对象，
 * Opener 用 struct 模式、msg name "other-msg" → struct 找不到。
 */
class OpenOnlyMissingStructObjectTestCase : public TestCase
{
  public:
    OpenOnlyMissingStructObjectTestCase()
        : TestCase("open-only constructor throws on missing struct objects")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("missing-stru"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, false, false,
            4096,
            names.m_segmentName.c_str(),
            "stru-cpp2py",
            "stru-py2cpp",
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        bool caught = false;
        try
        {
            Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
                false, false, false,
                4096,
                names.m_segmentName.c_str(),
                "other-cpp2py",
                "other-py2cpp",
                names.m_lockableName.c_str(),
                1000000,
                names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "Missing struct objects in open-only constructor throws Ns3AiRuntimeError");
    }
};

/**
 * \brief open-only 构造中缺失 sync 对象抛 Ns3AiRuntimeError。
 *
 * Creator 用 lockable "my-lock"，Opener 用 "other-lock" → sync 找不到。
 */
class OpenOnlyMissingSyncObjectTestCase : public TestCase
{
  public:
    OpenOnlyMissingSyncObjectTestCase()
        : TestCase("open-only constructor throws on missing sync object")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("missing-sync"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            "my-lock",
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        bool caught = false;
        try
        {
            Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
                false, false, false,
                4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                "other-lock",
                1000000,
                names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "Missing sync object in open-only constructor throws Ns3AiRuntimeError");
    }
};

/**
 * \brief open-only 构造中缺失 header 对象抛 Ns3AiRuntimeError。
 *
 * Creator 用 header "my-hdr"，Opener 用 "other-hdr" → header 找不到。
 * msg name 和 lockable name 保持不变，确保先通过之前的 null 检查。
 */
class OpenOnlyMissingHeaderObjectTestCase : public TestCase
{
  public:
    OpenOnlyMissingHeaderObjectTestCase()
        : TestCase("open-only constructor throws on missing header object")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("missing-header"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            "my-hdr",
            0x1111, 0x2222, 1, 1,
            Ns3AiSchemaValidationMode::Strict);

        bool caught = false;
        try
        {
            Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
                false, false, false,
                4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000000,
                "other-hdr",
                0x1111, 0x2222, 1, 1,
                Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "Missing header object in open-only constructor throws Ns3AiRuntimeError");
    }
};

/**
 * \brief 每个异常子类可被其自身类型捕获（不仅是基类）。
 */
class Ns3AiErrorCatchBySpecificTypeTestCase : public TestCase
{
  public:
    Ns3AiErrorCatchBySpecificTypeTestCase()
        : TestCase("each exception subclass is caught by its own type")
    {
    }

  private:
    void DoRun() override
    {
        bool caught = false;
        try { throw Ns3AiRuntimeError("test"); }
        catch (const Ns3AiRuntimeError&) { caught = true; }
        NS_TEST_EXPECT_MSG_EQ(caught, true, "Ns3AiRuntimeError caught by Ns3AiRuntimeError");

        caught = false;
        try { throw Ns3AiTimeoutError("test"); }
        catch (const Ns3AiTimeoutError&) { caught = true; }
        NS_TEST_EXPECT_MSG_EQ(caught, true, "Ns3AiTimeoutError caught by Ns3AiTimeoutError");

        caught = false;
        try { throw Ns3AiProtocolError("test"); }
        catch (const Ns3AiProtocolError&) { caught = true; }
        NS_TEST_EXPECT_MSG_EQ(caught, true, "Ns3AiProtocolError caught by Ns3AiProtocolError");

        caught = false;
        try { throw Ns3AiSchemaError("test"); }
        catch (const Ns3AiSchemaError&) { caught = true; }
        NS_TEST_EXPECT_MSG_EQ(caught, true, "Ns3AiSchemaError caught by Ns3AiSchemaError");
    }
};

/**
 * \brief OpenGymVectorEnv 构造时 numEnvs=0 抛 Ns3AiRuntimeError。
 *
 * 使用派生自 OpenGymVectorEnv 的最小测试桩，绕过纯虚方法约束。
 */
class TestVectorEnv : public OpenGymVectorEnv
{
  public:
    TestVectorEnv(uint32_t numEnvs)
        : OpenGymVectorEnv(numEnvs)
    {
    }

    Ptr<OpenGymSpace> GetActionSpace(uint32_t) override { return nullptr; }
    Ptr<OpenGymSpace> GetObservationSpace(uint32_t) override { return nullptr; }
    bool GetGameOver(uint32_t) override { return false; }
    Ptr<OpenGymDataContainer> GetObservation(uint32_t) override { return nullptr; }
    float GetReward(uint32_t) override { return 0.0f; }
    std::string GetExtraInfo(uint32_t) override { return ""; }
    bool ExecuteActions(uint32_t, Ptr<OpenGymDataContainer>) override { return false; }
};

class VectorEnvZeroEnvsTestCase : public TestCase
{
  public:
    VectorEnvZeroEnvsTestCase()
        : TestCase("OpenGymVectorEnv with zero envs throws Ns3AiRuntimeError")
    {
    }

  private:
    void DoRun() override
    {
        bool caught = false;
        try
        {
            TestVectorEnv env(0);
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "OpenGymVectorEnv(0) throws Ns3AiRuntimeError");
    }
};

/**
 * \brief 未开启 finish-handling 时调用 CppSetFinished 抛 Ns3AiRuntimeError。
 */
class FinishHandlingNotConfiguredTestCase : public TestCase
{
  public:
    FinishHandlingNotConfiguredTestCase()
        : TestCase("finish operations throw when handle_finish is not configured")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("no-finish"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> creator(
            true, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);
        Ns3AiMsgInterfaceImpl<ErrorTestCppMsg, ErrorTestPyMsg> opener(
            false, false, false,
            4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000000,
            names.m_headerName.c_str(), 0x1111, 0x2222, 1, 1, Ns3AiSchemaValidationMode::Strict);

        // CppSetFinished (void) → throw
        bool caught = false;
        try
        {
            creator.CppSetFinished();
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught, true,
                              "CppSetFinished throws Ns3AiRuntimeError when handle_finish is false");

        // TryCppSetFinished (bool) → throw, not return false
        bool caught2 = false;
        try
        {
            creator.TryCppSetFinished();
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught2 = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught2, true,
                              "TryCppSetFinished throws Ns3AiRuntimeError when handle_finish is false");

        // PyGetFinished (bool) → throw, not return false
        bool caught3 = false;
        try
        {
            opener.PyGetFinished();
        }
        catch (const Ns3AiRuntimeError&)
        {
            caught3 = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caught3, true,
                              "PyGetFinished throws Ns3AiRuntimeError when handle_finish is false");
    }
};

class Ns3AiErrorTestSuite : public TestSuite
{
  public:
    Ns3AiErrorTestSuite()
        : TestSuite("ns3-ai-error", UNIT)
    {
        AddTestCase(new Ns3AiErrorConstructTestCase, TestCase::QUICK);
        AddTestCase(new Ns3AiErrorCatchByBaseTestCase, TestCase::QUICK);
        AddTestCase(new AccessModeVectorCallStructGetterTestCase, TestCase::QUICK);
        AddTestCase(new AccessModeStructCallVectorGetterTestCase, TestCase::QUICK);
        AddTestCase(new OpenOnlyMissingVectorObjectTestCase, TestCase::QUICK);
        AddTestCase(new OpenOnlyMissingStructObjectTestCase, TestCase::QUICK);
        AddTestCase(new OpenOnlyMissingSyncObjectTestCase, TestCase::QUICK);
        AddTestCase(new OpenOnlyMissingHeaderObjectTestCase, TestCase::QUICK);
        AddTestCase(new Ns3AiErrorCatchBySpecificTypeTestCase, TestCase::QUICK);
        AddTestCase(new VectorEnvZeroEnvsTestCase, TestCase::QUICK);
        AddTestCase(new FinishHandlingNotConfiguredTestCase, TestCase::QUICK);
    }
};

static Ns3AiErrorTestSuite g_ns3AiErrorTestSuite;

} // namespace
