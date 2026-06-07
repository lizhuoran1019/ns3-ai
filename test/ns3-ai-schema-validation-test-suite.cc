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

struct SchemaTestCppMsg
{
    uint32_t value;
};

struct SchemaTestPyMsg
{
    uint32_t value;
};

Ns3AiMsgInterfaceNames
MakeTestNames(const std::string& suffix)
{
    return Ns3AiMsgInterface::MakeNames("ns3-ai-schema-validation-test-" + suffix);
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
 * \brief Strict 模式下，creator 携带 0 schema hash 时必须在构造阶段失败，
 *        不创建共享内存。
 */
class StrictModeCreatorZeroHashRejectedTestCase : public TestCase
{
  public:
    StrictModeCreatorZeroHashRejectedTestCase()
        : TestCase("strict mode rejects creator with zero schema hash")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("strict-creator-zero"));
        RemoveSegment(names);

        bool caught = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
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
                0,   // cpp2py_schema_hash = 0
                0,   // py2cpp_schema_hash = 0
                0,   // cpp2py_schema_version = 0
                0,   // py2cpp_schema_version = 0
                Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiSchemaError&)
        {
            caught = true;
        }

        NS_TEST_EXPECT_MSG_EQ(caught,
                              true,
                              "Strict mode creator with zero schema hash throws Ns3AiSchemaError");
    }
};

/**
 * \brief Strict 模式下，schema hash 不匹配时错误消息包含所有诊断字段。
 */
class StrictModeHashMismatchErrorDiagnosticTestCase : public TestCase
{
  public:
    StrictModeHashMismatchErrorDiagnosticTestCase()
        : TestCase("strict mode hash mismatch error includes diagnosis fields")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("hash-mismatch-diag"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0x1111,  // cpp2py_schema_hash
            0x2222,  // py2cpp_schema_hash
            1,       // cpp2py_schema_version
            1,       // py2cpp_schema_version
            Ns3AiSchemaValidationMode::Strict);

        bool caughtSchemaError = false;
        bool hasDirection = false;
        bool hasField = false;
        bool hasExpected = false;
        bool hasActual = false;
        bool hasMode = false;
        bool hasHeader = false;
        bool hasSegment = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0x9999,  // cpp2py_schema_hash differs from creator
                0x2222,  // py2cpp_schema_hash matches
                1,
                1,
                Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiSchemaError& e)
        {
            caughtSchemaError = true;
            const std::string msg = e.what();
            hasDirection = msg.find("direction=cpp2py") != std::string::npos;
            hasField = msg.find("field=schema_hash") != std::string::npos;
            hasExpected = msg.find("expected=0x") != std::string::npos;
            hasActual = msg.find("actual=0x") != std::string::npos;
            hasMode = msg.find("mode=strict") != std::string::npos;
            hasHeader = msg.find("header=") != std::string::npos;
            hasSegment = msg.find("segment=") != std::string::npos;
        }

        NS_TEST_EXPECT_MSG_EQ(caughtSchemaError, true,
                              "Hash mismatch in strict mode throws Ns3AiSchemaError");
        NS_TEST_EXPECT_MSG_EQ(hasDirection, true,
                              "error message contains direction=cpp2py");
        NS_TEST_EXPECT_MSG_EQ(hasField, true,
                              "error message contains field=schema_hash");
        NS_TEST_EXPECT_MSG_EQ(hasExpected, true,
                              "error message contains expected=0x...");
        NS_TEST_EXPECT_MSG_EQ(hasActual, true,
                              "error message contains actual=0x...");
        NS_TEST_EXPECT_MSG_EQ(hasMode, true,
                              "error message contains mode=strict");
        NS_TEST_EXPECT_MSG_EQ(hasHeader, true,
                              "error message contains header=");
        NS_TEST_EXPECT_MSG_EQ(hasSegment, true,
                              "error message contains segment=");
    }
};

/**
 * \brief Strict 模式下，schema version 不匹配时 opener 构造失败。
 */
class StrictModeVersionMismatchRejectedTestCase : public TestCase
{
  public:
    StrictModeVersionMismatchRejectedTestCase()
        : TestCase("strict mode rejects schema version mismatch")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("strict-version-mismatch"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0x1111,
            0x2222,
            1,   // cpp2py_schema_version = 1
            2,   // py2cpp_schema_version = 2
            Ns3AiSchemaValidationMode::Strict);

        // cpp2py version mismatch: creator=1, opener=3
        bool caughtCpp2Py = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0x1111,
                0x2222,
                3,   // cpp2py_schema_version differs
                2,   // py2cpp_schema_version matches
                Ns3AiSchemaValidationMode::Strict);
        }
        catch (const Ns3AiSchemaError&)
        {
            caughtCpp2Py = true;
        }
        NS_TEST_EXPECT_MSG_EQ(caughtCpp2Py, true,
                              "cpp2py schema version mismatch throws Ns3AiSchemaError");
    }
};

/**
 * \brief Compatibility 模式下 missing metadata (hash=0) 允许 opener 通过，不抛异常。
 */
class CompatibilityModeMissingMetadataAllowsOpenTestCase : public TestCase
{
  public:
    CompatibilityModeMissingMetadataAllowsOpenTestCase()
        : TestCase("compatibility mode allows open with zero schema hash")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("compat-missing"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0, 0, 0, 0,
            Ns3AiSchemaValidationMode::Compatibility);

        bool openerOk = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0, 0, 0, 0,
                Ns3AiSchemaValidationMode::Compatibility);
            openerOk = true;
        }
        catch (const std::exception&)
        {
        }

        NS_TEST_EXPECT_MSG_EQ(openerOk, true,
                              "Compatibility mode allows opener with zero metadata");
    }
};

/**
 * \brief Compatibility 模式下 non-zero metadata mismatch 仍然失败。
 */
/**
 * \brief Compatibility 模式下 creator metadata=0，opener non-zero metadata → warning 不失败。
 */
class CompatibilityModePeerMissingMetadataTestCase : public TestCase
{
  public:
    CompatibilityModePeerMissingMetadataTestCase()
        : TestCase("compatibility mode one side missing metadata still allows open")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("compat-peer-missing"));
        RemoveSegment(names);

        // creator has metadata=0
        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0, 0, 0, 0,
            Ns3AiSchemaValidationMode::Compatibility);

        // opener has non-zero expected metadata
        bool openerOk = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0x1111, 0x2222, 1, 1,
                Ns3AiSchemaValidationMode::Compatibility);
            openerOk = true;
        }
        catch (const std::exception&)
        {
        }

        NS_TEST_EXPECT_MSG_EQ(openerOk, true,
                              "Compatibility mode: opener with non-zero expected succeeds "
                              "when creator has zero metadata on wire");
    }
};

class CompatibilityModeMismatchStillFailsTestCase : public TestCase
{
  public:
    CompatibilityModeMismatchStillFailsTestCase()
        : TestCase("compatibility mode still rejects hash mismatch when metadata is present")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("compat-mismatch"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0x1111, 0x2222, 1, 1,
            Ns3AiSchemaValidationMode::Compatibility);

        bool caughtSchemaError = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0x9999, 0x2222, 1, 1,
                Ns3AiSchemaValidationMode::Compatibility);
        }
        catch (const Ns3AiSchemaError&)
        {
            caughtSchemaError = true;
        }

        NS_TEST_EXPECT_MSG_EQ(caughtSchemaError, true,
                              "Compatibility mode rejects hash mismatch when metadata is non-zero");
    }
};

/**
 * \brief Disabled 模式下 mismatch 不抛异常。
 */
class DisabledModeMismatchAllowsOpenTestCase : public TestCase
{
  public:
    DisabledModeMismatchAllowsOpenTestCase()
        : TestCase("disabled mode allows open despite hash mismatch")
    {
    }

  private:
    void DoRun() override
    {
        const auto names = MakeTestNames(MakeUniqueSuffix("disabled-mismatch"));
        RemoveSegment(names);

        Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> creator(
            true, false, true, 4096,
            names.m_segmentName.c_str(),
            names.m_cpp2pyMsgName.c_str(),
            names.m_py2cppMsgName.c_str(),
            names.m_lockableName.c_str(),
            1000,
            names.m_headerName.c_str(),
            0x1111, 0x2222, 1, 1,
            Ns3AiSchemaValidationMode::Disabled);

        bool openerOk = false;
        try
        {
            Ns3AiMsgInterfaceImpl<SchemaTestCppMsg, SchemaTestPyMsg> opener(
                false, false, true, 4096,
                names.m_segmentName.c_str(),
                names.m_cpp2pyMsgName.c_str(),
                names.m_py2cppMsgName.c_str(),
                names.m_lockableName.c_str(),
                1000,
                names.m_headerName.c_str(),
                0x9999, 0x2222, 1, 1,
                Ns3AiSchemaValidationMode::Disabled);
            openerOk = true;
        }
        catch (const std::exception&)
        {
        }

        NS_TEST_EXPECT_MSG_EQ(openerOk, true,
                              "Disabled mode allows opener with hash mismatch");
    }
};

class Ns3AiSchemaValidationTestSuite : public TestSuite
{
  public:
    Ns3AiSchemaValidationTestSuite()
        : TestSuite("ns3-ai-schema-validation", UNIT)
    {
        AddTestCase(new StrictModeCreatorZeroHashRejectedTestCase, TestCase::QUICK);
        AddTestCase(new StrictModeHashMismatchErrorDiagnosticTestCase, TestCase::QUICK);
        AddTestCase(new StrictModeVersionMismatchRejectedTestCase, TestCase::QUICK);
        AddTestCase(new CompatibilityModeMissingMetadataAllowsOpenTestCase, TestCase::QUICK);
        AddTestCase(new CompatibilityModePeerMissingMetadataTestCase, TestCase::QUICK);
        AddTestCase(new CompatibilityModeMismatchStillFailsTestCase, TestCase::QUICK);
        AddTestCase(new DisabledModeMismatchAllowsOpenTestCase, TestCase::QUICK);
    }
};

static Ns3AiSchemaValidationTestSuite g_ns3AiSchemaValidationTestSuite;

} // namespace
