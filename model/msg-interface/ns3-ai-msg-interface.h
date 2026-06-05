/*
 * Copyright (c) 2023 Huazhong University of Science and Technology
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
 * Authors:  Muyuan Shen <muyuan_shen@hust.edu.cn>
 */

#ifndef NS3_AI_MSG_INTERFACE_H
#define NS3_AI_MSG_INTERFACE_H

#include "ns3-ai-semaphore.h"

#include <ns3/singleton.h>

#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ns3
{

static constexpr uint32_t NS3_AI_MSG_HEADER_MAGIC = 0x4E334149; // "N3AI"
static constexpr uint16_t NS3_AI_MSG_ABI_VERSION = 1;

/**
 * \brief Structure containing semaphores used in msg interface
 */
struct Ns3AiMsgSync
{
    volatile uint8_t m_cpp2pyEmptyCount{1};
    volatile uint8_t m_cpp2pyFullCount{0};
    volatile uint8_t m_py2cppEmptyCount{1};
    volatile uint8_t m_py2cppFullCount{0};
    volatile bool m_isFinished{false};
};

/**
 * \brief Fixed protocol metadata stored in shared memory next to payload objects.
 */
struct Ns3AiMsgProtocolHeader
{
    volatile uint32_t m_magic{NS3_AI_MSG_HEADER_MAGIC};
    volatile uint16_t m_abiVersion{NS3_AI_MSG_ABI_VERSION};
    volatile uint16_t m_headerSize{sizeof(Ns3AiMsgProtocolHeader)};
    volatile uint32_t m_cpp2pyPayloadSize{0};
    volatile uint32_t m_py2cppPayloadSize{0};
    volatile uint32_t m_cpp2pySchemaVersion{0};
    volatile uint32_t m_py2cppSchemaVersion{0};
    volatile uint64_t m_cpp2pySchemaHash{0};
    volatile uint64_t m_py2cppSchemaHash{0};
};

/**
 * \brief Runtime field types used by schema descriptors and generated bindings.
 */
enum class Ns3AiMsgFieldType : uint16_t
{
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    RawBytes
};

/**
 * \brief One field in a message schema.
 */
struct Ns3AiMsgField
{
    std::string m_name;
    Ns3AiMsgFieldType m_type;
    std::size_t m_offset{0};
    std::size_t m_size{0};
    std::size_t m_count{1};
};

/**
 * \brief Runtime schema descriptor for one payload type.
 */
struct Ns3AiMsgSchema
{
    std::string m_name;
    uint32_t m_version{1};
    uint64_t m_schemaHash{0};
    uint32_t m_size{0};
    std::vector<Ns3AiMsgField> m_fields;
};

inline uint64_t
Ns3AiMsgHashBytes(uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t
Ns3AiMsgHashString(uint64_t hash, const std::string& value)
{
    return Ns3AiMsgHashBytes(hash, value.data(), value.size());
}

inline uint64_t
ComputeNs3AiMsgSchemaHash(const Ns3AiMsgSchema& schema)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = Ns3AiMsgHashString(hash, schema.m_name);
    hash = Ns3AiMsgHashBytes(hash, &schema.m_version, sizeof(schema.m_version));
    hash = Ns3AiMsgHashBytes(hash, &schema.m_size, sizeof(schema.m_size));

    for (const auto& field : schema.m_fields)
    {
        const uint16_t type = static_cast<uint16_t>(field.m_type);
        hash = Ns3AiMsgHashString(hash, field.m_name);
        hash = Ns3AiMsgHashBytes(hash, &type, sizeof(type));
        hash = Ns3AiMsgHashBytes(hash, &field.m_offset, sizeof(field.m_offset));
        hash = Ns3AiMsgHashBytes(hash, &field.m_size, sizeof(field.m_size));
        hash = Ns3AiMsgHashBytes(hash, &field.m_count, sizeof(field.m_count));
    }
    return hash;
}

inline Ns3AiMsgSchema
MakeNs3AiMsgSchema(std::string name,
                   uint32_t version,
                   uint32_t size,
                   std::initializer_list<Ns3AiMsgField> fields)
{
    Ns3AiMsgSchema schema{std::move(name), version, 0, size, std::vector<Ns3AiMsgField>(fields)};
    schema.m_schemaHash = ComputeNs3AiMsgSchemaHash(schema);
    return schema;
}

/**
 * \brief Names of the shared-memory objects used by one msg interface instance.
 */
struct Ns3AiMsgInterfaceNames
{
    std::string m_segmentName;
    std::string m_cpp2pyMsgName;
    std::string m_py2cppMsgName;
    std::string m_lockableName;
    std::string m_headerName{"My Header"};
};

/**
 * \brief Complete configuration for one message interface instance.
 *
 * Passing this object to GetInterface decouples per-instance settings from the
 * singleton's legacy mutable defaults, so one process can create interfaces
 * with different creator/vector/finish/size/timeout/name settings.
 */
struct Ns3AiMsgInterfaceConfig
{
    bool m_isMemoryCreator{false};
    bool m_useVector{false};
    bool m_handleFinish{false};
    uint32_t m_size{4096};
    uint64_t m_syncTimeoutUs{300000000};
    Ns3AiMsgInterfaceNames m_names{"My Seg",
                                    "My Cpp to Python Msg",
                                    "My Python to Cpp Msg",
                                    "My Lockable",
                                    "My Header"};
    uint64_t m_cpp2pySchemaHash{0};
    uint64_t m_py2cppSchemaHash{0};
    uint32_t m_cpp2pySchemaVersion{0};
    uint32_t m_py2cppSchemaVersion{0};
    uint32_t m_cpp2pySchemaSize{0};
    uint32_t m_py2cppSchemaSize{0};

    Ns3AiMsgInterfaceConfig& SetSchemas(const Ns3AiMsgSchema& cpp2pySchema,
                                        const Ns3AiMsgSchema& py2cppSchema)
    {
        m_cpp2pySchemaHash = cpp2pySchema.m_schemaHash;
        m_py2cppSchemaHash = py2cppSchema.m_schemaHash;
        m_cpp2pySchemaVersion = cpp2pySchema.m_version;
        m_py2cppSchemaVersion = py2cppSchema.m_version;
        m_cpp2pySchemaSize = cpp2pySchema.m_size;
        m_py2cppSchemaSize = py2cppSchema.m_size;
        return *this;
    }
};

/**
 * \brief Type-erased base class for keeping typed message interfaces in one registry.
 */
class Ns3AiMsgInterfaceBase
{
  public:
    virtual ~Ns3AiMsgInterfaceBase() = default;
};

/**
 * \brief A template class implementation of the message interface
 */
template <typename Cpp2PyMsgType, typename Py2CppMsgType>
class Ns3AiMsgInterfaceImpl : public Ns3AiMsgInterfaceBase
{
  public:
    static constexpr uint64_t DEFAULT_SYNC_TIMEOUT_US = 300000000;

    Ns3AiMsgInterfaceImpl() = delete;

    explicit Ns3AiMsgInterfaceImpl(bool is_memory_creator,
                                   bool use_vector,
                                   bool handle_finish,
                                   uint32_t size = 4096,
                                   const char* segment_name = "My Seg",
                                   const char* cpp2py_msg_name = "My Cpp to Python Msg",
                                   const char* py2cpp_msg_name = "My Python to Cpp Msg",
                                   const char* lockable_name = "My Lockable",
                                   uint64_t sync_timeout_us = DEFAULT_SYNC_TIMEOUT_US,
                                   const char* header_name = "My Header",
                                   uint64_t cpp2py_schema_hash = 0,
                                   uint64_t py2cpp_schema_hash = 0,
                                   uint32_t cpp2py_schema_version = 0,
                                   uint32_t py2cpp_schema_version = 0)
        : m_cpp2pyStruct(nullptr),
          m_py2CppStruct(nullptr),
          m_cpp2pyVector(nullptr),
          m_py2cppVector(nullptr),
          m_sync(nullptr),
          m_header(nullptr),
          m_isCreator(is_memory_creator),
          m_useVector(use_vector),
          m_handleFinish(handle_finish),
          m_segName(segment_name),
          m_headerName(header_name),
          m_syncTimeoutUs(sync_timeout_us),
          m_cpp2pySchemaHash(cpp2py_schema_hash),
          m_py2cppSchemaHash(py2cpp_schema_hash),
          m_cpp2pySchemaVersion(cpp2py_schema_version),
          m_py2cppSchemaVersion(py2cpp_schema_version),
          m_isFinished(false),
          m_pyRecvHasCpp2PySlot(false)
    {
        using namespace boost::interprocess;
        if (m_isCreator)
        {
            shared_memory_object::remove(m_segName.c_str());
            m_segment = std::make_unique<managed_shared_memory>(create_only, m_segName.c_str(), size);
            if (m_useVector)
            {
                const Cpp2PyMsgAllocator alloc_env(m_segment->get_segment_manager());
                const Py2CppMsgAllocator alloc_act(m_segment->get_segment_manager());
                m_cpp2pyVector = m_segment->construct<Cpp2PyMsgVector>(cpp2py_msg_name)(alloc_env);
                m_py2cppVector = m_segment->construct<Py2CppMsgVector>(py2cpp_msg_name)(alloc_act);
            }
            else
            {
                m_cpp2pyStruct = m_segment->construct<Cpp2PyMsgType>(cpp2py_msg_name)();
                m_py2CppStruct = m_segment->construct<Py2CppMsgType>(py2cpp_msg_name)();
            }
            m_sync = m_segment->construct<Ns3AiMsgSync>(lockable_name)();
            m_header = m_segment->construct<Ns3AiMsgProtocolHeader>(header_name)();
            InitializeProtocolHeader();
        }
        else
        {
            m_segment = std::make_unique<managed_shared_memory>(open_only, segment_name);
            if (m_useVector)
            {
                m_cpp2pyVector = m_segment->find<Cpp2PyMsgVector>(cpp2py_msg_name).first;
                m_py2cppVector = m_segment->find<Py2CppMsgVector>(py2cpp_msg_name).first;
                assert(m_cpp2pyVector != nullptr);
                assert(m_py2cppVector != nullptr);
            }
            else
            {
                m_cpp2pyStruct = m_segment->find<Cpp2PyMsgType>(cpp2py_msg_name).first;
                m_py2CppStruct = m_segment->find<Py2CppMsgType>(py2cpp_msg_name).first;
                assert(m_cpp2pyStruct != nullptr);
                assert(m_py2CppStruct != nullptr);
            }
            m_sync = m_segment->find<Ns3AiMsgSync>(lockable_name).first;
            m_header = m_segment->find<Ns3AiMsgProtocolHeader>(header_name).first;
            assert(m_sync != nullptr);
            ValidateProtocolHeader();
        }
    };

    ~Ns3AiMsgInterfaceImpl() override
    {
        if (m_isCreator)
        {
            boost::interprocess::shared_memory_object::remove(m_segName.c_str());
        }
        else
        {
            if (m_handleFinish && !m_isFinished)
            {
                TryCppSetFinished();
            }
        }
    };

    typedef boost::interprocess::
        allocator<Cpp2PyMsgType, boost::interprocess::managed_shared_memory::segment_manager>
            Cpp2PyMsgAllocator;
    typedef boost::interprocess::vector<Cpp2PyMsgType, Cpp2PyMsgAllocator> Cpp2PyMsgVector;
    typedef boost::interprocess::
        allocator<Py2CppMsgType, boost::interprocess::managed_shared_memory::segment_manager>
            Py2CppMsgAllocator;
    typedef boost::interprocess::vector<Py2CppMsgType, Py2CppMsgAllocator> Py2CppMsgVector;

    /**
     * Get the struct used in C++ to Python transmission in
     * struct-based message interface
     */
    Cpp2PyMsgType* GetCpp2PyStruct()
    {
        assert(!m_useVector);
        return m_cpp2pyStruct;
    };

    /**
     * Get the struct used in Python to C++ transmission in
     * struct-based message interface
     */
    Py2CppMsgType* GetPy2CppStruct()
    {
        assert(!m_useVector);
        return m_py2CppStruct;
    };

    /**
     * Get the vector used in C++ to Python transmission in
     * vector-based message interface
     */
    Cpp2PyMsgVector* GetCpp2PyVector()
    {
        assert(m_useVector);
        return m_cpp2pyVector;
    };

    /**
     * Get the vector used in Python to C++ transmission in
     * vector-based message interface
     */
    Py2CppMsgVector* GetPy2CppVector()
    {
        assert(m_useVector);
        return m_py2cppVector;
    };

    /**
     * Gets the protocol header stored in shared memory.
     */
    const Ns3AiMsgProtocolHeader* GetProtocolHeader() const
    {
        return m_header;
    };

    /**
     * Gets the per-operation synchronization timeout in microseconds.
     * A value of 0 disables timeout and restores the historical unbounded wait.
     */
    uint64_t GetSyncTimeoutUs() const
    {
        return m_syncTimeoutUs;
    };

    /**
     * C++ side starts writing into shared memory, struct-based
     * or vector-based
     */
    void CppSendBegin()
    {
        WaitOrThrow(&m_sync->m_cpp2pyEmptyCount, "CppSendBegin", "cpp2py empty slot", true);
    };

    /**
     * Non-throwing version of CppSendBegin.
     */
    bool TryCppSendBegin()
    {
        return WaitForSync(&m_sync->m_cpp2pyEmptyCount, true);
    };

    /**
     * C++ side stops writing into shared memory, struct-based
     * or vector-based
     */
    void CppSendEnd()
    {
        Ns3AiSemaphore::sem_post(&m_sync->m_cpp2pyFullCount);
    };

    /**
     * C++ side starts reading from shared memory, struct-based
     * or vector-based
     */
    void CppRecvBegin()
    {
        WaitOrThrow(&m_sync->m_py2cppFullCount, "CppRecvBegin", "py2cpp full slot", true);
    };

    /**
     * Non-throwing version of CppRecvBegin.
     */
    bool TryCppRecvBegin()
    {
        return WaitForSync(&m_sync->m_py2cppFullCount, true);
    };

    /**
     * C++ side stops reading from shared memory, struct-based
     * or vector-based
     */
    void CppRecvEnd()
    {
        Ns3AiSemaphore::sem_post(&m_sync->m_py2cppEmptyCount);
    };

    /**
     * C++ side sets the overall status to finished when
     * the simulation is over.
     *
     * Finish is an out-of-band control flag. It must not wait for or publish
     * a cpp2py data slot, otherwise cleanup can deadlock behind unread data.
     */
    void CppSetFinished()
    {
        assert(m_handleFinish);
        TryCppSetFinished();
    };

    /**
     * Non-throwing version of CppSetFinished.
     */
    bool TryCppSetFinished()
    {
        assert(m_handleFinish);
        if (m_isFinished || m_sync->m_isFinished)
        {
            m_isFinished = true;
            return true;
        }

        m_isFinished = true;
        m_sync->m_isFinished = true;
        __sync_synchronize();
        return true;
    };

    /**
     * Python side starts reading from shared memory, struct-based
     * or vector-based
     */
    void PyRecvBegin()
    {
        if (!TryPyRecvBegin())
        {
            ThrowSyncFailure("PyRecvBegin", "cpp2py full slot or finish flag");
        }
    };

    /**
     * Non-throwing version of PyRecvBegin.
     */
    bool TryPyRecvBegin()
    {
        m_pyRecvHasCpp2PySlot = false;
        if (!WaitForCpp2PyDataOrFinish())
        {
            return false;
        }
        if (m_handleFinish)
        {
            m_isFinished = m_sync->m_isFinished;
        }
        return true;
    };

    /**
     * Python side stops reading from shared memory, struct-based
     * or vector-based
     */
    void PyRecvEnd()
    {
        if (m_pyRecvHasCpp2PySlot)
        {
            Ns3AiSemaphore::sem_post(&m_sync->m_cpp2pyEmptyCount);
            m_pyRecvHasCpp2PySlot = false;
        }
    };

    /**
     * Python side starts writing into shared memory, struct-based
     * or vector-based
     */
    void PySendBegin()
    {
        WaitOrThrow(&m_sync->m_py2cppEmptyCount, "PySendBegin", "py2cpp empty slot", true);
    };

    /**
     * Non-throwing version of PySendBegin.
     */
    bool TryPySendBegin()
    {
        if (!WaitForSync(&m_sync->m_py2cppEmptyCount, true))
        {
            if (m_handleFinish)
            {
                m_isFinished = m_sync->m_isFinished;
            }
            return false;
        }
        return true;
    };

    /**
     * Python side stops writing into shared memory, struct-based
     * or vector-based
     */
    void PySendEnd()
    {
        Ns3AiSemaphore::sem_post(&m_sync->m_py2cppFullCount);
    };

    /**
     * Python side gets whether the simulation is over
     */
    bool PyGetFinished()
    {
        assert(m_handleFinish);
        m_isFinished = m_sync->m_isFinished;
        return m_isFinished;
    };

  private:
    bool WaitForSync(volatile uint8_t* counter, bool abortOnFinished)
    {
        const volatile bool* abortFlag =
            (abortOnFinished && m_handleFinish) ? &m_sync->m_isFinished : nullptr;
        return Ns3AiSemaphore::sem_wait(counter, m_syncTimeoutUs, abortFlag);
    };

    bool WaitForCpp2PyDataOrFinish()
    {
        const auto start = std::chrono::steady_clock::now();
        uint32_t attempts = 0;

        while (true)
        {
            if (Ns3AiSemaphore::sem_try_wait(&m_sync->m_cpp2pyFullCount))
            {
                m_pyRecvHasCpp2PySlot = true;
                return true;
            }

            if (m_handleFinish && m_sync->m_isFinished)
            {
                m_isFinished = true;
                m_pyRecvHasCpp2PySlot = false;
                return true;
            }

            if (m_syncTimeoutUs > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
                if (static_cast<uint64_t>(elapsed.count()) >= m_syncTimeoutUs)
                {
                    return false;
                }
            }

            Backoff(attempts++);
        }
    };

    void InitializeProtocolHeader()
    {
        m_header->m_magic = NS3_AI_MSG_HEADER_MAGIC;
        m_header->m_abiVersion = NS3_AI_MSG_ABI_VERSION;
        m_header->m_headerSize = sizeof(Ns3AiMsgProtocolHeader);
        m_header->m_cpp2pyPayloadSize = sizeof(Cpp2PyMsgType);
        m_header->m_py2cppPayloadSize = sizeof(Py2CppMsgType);
        m_header->m_cpp2pySchemaHash = m_cpp2pySchemaHash;
        m_header->m_py2cppSchemaHash = m_py2cppSchemaHash;
        m_header->m_cpp2pySchemaVersion = m_cpp2pySchemaVersion;
        m_header->m_py2cppSchemaVersion = m_py2cppSchemaVersion;
        __sync_synchronize();
    };

    void ValidateProtocolHeader() const
    {
        if (m_header == nullptr)
        {
            std::ostringstream oss;
            oss << "ns3-ai message interface could not find protocol header '" << m_headerName << "'.";
            throw std::runtime_error(oss.str());
        }
        if (m_header->m_magic != NS3_AI_MSG_HEADER_MAGIC ||
            m_header->m_abiVersion != NS3_AI_MSG_ABI_VERSION ||
            m_header->m_headerSize != sizeof(Ns3AiMsgProtocolHeader))
        {
            ThrowProtocolHeaderFailure("header magic, ABI version, or size mismatch");
        }
        if (m_header->m_cpp2pyPayloadSize != sizeof(Cpp2PyMsgType) ||
            m_header->m_py2cppPayloadSize != sizeof(Py2CppMsgType))
        {
            ThrowProtocolHeaderFailure("payload size mismatch");
        }
        if (m_cpp2pySchemaHash != 0 && m_header->m_cpp2pySchemaHash != m_cpp2pySchemaHash)
        {
            ThrowProtocolHeaderFailure("cpp2py schema hash mismatch");
        }
        if (m_py2cppSchemaHash != 0 && m_header->m_py2cppSchemaHash != m_py2cppSchemaHash)
        {
            ThrowProtocolHeaderFailure("py2cpp schema hash mismatch");
        }
        if (m_cpp2pySchemaVersion != 0 && m_header->m_cpp2pySchemaVersion != m_cpp2pySchemaVersion)
        {
            ThrowProtocolHeaderFailure("cpp2py schema version mismatch");
        }
        if (m_py2cppSchemaVersion != 0 && m_header->m_py2cppSchemaVersion != m_py2cppSchemaVersion)
        {
            ThrowProtocolHeaderFailure("py2cpp schema version mismatch");
        }
    };

    [[noreturn]] void ThrowProtocolHeaderFailure(const char* reason) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface protocol header validation failed for '" << m_headerName
            << "': " << reason << ". Check that both peers use the same message schema and ABI.";
        throw std::runtime_error(oss.str());
    };

    void WaitOrThrow(volatile uint8_t* counter,
                     const char* operation,
                     const char* waitTarget,
                     bool abortOnFinished)
    {
        if (!WaitForSync(counter, abortOnFinished))
        {
            ThrowSyncFailure(operation, waitTarget);
        }
    };

    [[noreturn]] void ThrowSyncFailure(const char* operation, const char* waitTarget) const
    {
        std::ostringstream oss;
        oss << "ns3-ai message interface synchronization failed in " << operation
            << " while waiting for " << waitTarget << ". ";
        if (m_handleFinish && m_sync->m_isFinished)
        {
            oss << "The peer has already marked the shared session as finished. ";
        }
        else if (m_syncTimeoutUs > 0)
        {
            oss << "Timed out after " << m_syncTimeoutUs << " us. ";
        }
        else
        {
            oss << "The wait was aborted. ";
        }
        oss << "Check that C++ and Python send/recv calls are paired in the same order. "
            << "Increase the timeout with Ns3AiMsgInterface::SetSyncTimeoutUs(), "
            << "or set it to 0 to restore unbounded waiting for intentionally long inference.";
        throw std::runtime_error(oss.str());
    };

    static void Backoff(uint32_t attempts)
    {
        if (attempts < 64)
        {
            std::this_thread::yield();
            return;
        }
        if (attempts < 1024)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    std::unique_ptr<boost::interprocess::managed_shared_memory> m_segment;

    Cpp2PyMsgType* m_cpp2pyStruct;
    Py2CppMsgType* m_py2CppStruct;
    Cpp2PyMsgVector* m_cpp2pyVector;
    Py2CppMsgVector* m_py2cppVector;

    Ns3AiMsgSync* m_sync;
    Ns3AiMsgProtocolHeader* m_header;
    const bool m_isCreator;
    const bool m_useVector;
    const bool m_handleFinish;
    const std::string m_segName;
    const std::string m_headerName;
    const uint64_t m_syncTimeoutUs;
    const uint64_t m_cpp2pySchemaHash;
    const uint64_t m_py2cppSchemaHash;
    const uint32_t m_cpp2pySchemaVersion;
    const uint32_t m_py2cppSchemaVersion;
    bool m_isFinished;
    bool m_pyRecvHasCpp2PySlot;
};

/**
 * \brief The message interface, a singleton class with named interface instances.
 */
class Ns3AiMsgInterface : public Singleton<Ns3AiMsgInterface>
{
  public:
    /**
     * Builds deterministic object names for one independent shared-memory namespace.
     */
    static Ns3AiMsgInterfaceNames MakeNames(const std::string& prefix)
    {
        return Ns3AiMsgInterfaceNames{prefix + ".seg",
                                      prefix + ".cpp2py",
                                      prefix + ".py2cpp",
                                      prefix + ".lock",
                                      prefix + ".header"};
    };

    /**
     * Returns the singleton's legacy default configuration.
     */
    Ns3AiMsgInterfaceConfig GetDefaultConfig() const
    {
        return m_defaultConfig;
    };

    /**
     * Replaces the singleton's legacy default configuration.
     */
    void SetDefaultConfig(const Ns3AiMsgInterfaceConfig& config)
    {
        m_defaultConfig = config;
    };

    /**
     * Sets if this side (C++ or Python) is the memory creator.
     * Configuration on two sides must be different
     */
    void SetIsMemoryCreator(bool isMemoryCreator)
    {
        m_defaultConfig.m_isMemoryCreator = isMemoryCreator;
    };

    /**
     * Sets if both C++ and Python sides use vector. Configuration on
     * two sides must be same
     */
    void SetUseVector(bool useVector)
    {
        m_defaultConfig.m_useVector = useVector;
    };

    /**
     * Sets if both C++ and Python sides handle finish. Configuration on
     * two sides must be same
     */
    void SetHandleFinish(bool handleFinish)
    {
        m_defaultConfig.m_handleFinish = handleFinish;
    };

    /**
     * Sets shared memory segment size, only valid for
     * the shared memory creator. Normally the default
     * size is OK.
     */
    void SetMemorySize(uint32_t size)
    {
        m_defaultConfig.m_size = size;
    };

    /**
     * Sets per-operation synchronization timeout in microseconds.
     * A value of 0 disables timeout and restores the historical unbounded wait.
     */
    void SetSyncTimeoutUs(uint64_t timeoutUs)
    {
        m_defaultConfig.m_syncTimeoutUs = timeoutUs;
    };

    /**
     * Sets per-operation synchronization timeout in milliseconds.
     * A value of 0 disables timeout and restores the historical unbounded wait.
     */
    void SetSyncTimeoutMs(uint64_t timeoutMs)
    {
        m_defaultConfig.m_syncTimeoutUs = timeoutMs * 1000;
    };

    /**
     * Gets per-operation synchronization timeout in microseconds.
     */
    uint64_t GetSyncTimeoutUs() const
    {
        return m_defaultConfig.m_syncTimeoutUs;
    };

    /**
     * Sets the payload schemas used for subsequent legacy GetInterface calls.
     */
    void SetSchemas(const Ns3AiMsgSchema& cpp2pySchema, const Ns3AiMsgSchema& py2cppSchema)
    {
        m_defaultConfig.SetSchemas(cpp2pySchema, py2cppSchema);
    };

    /**
     * Sets the names of the named objects. See Boost's
     * documentation for details. Normally the default
     * names are OK.
     */
    void SetNames(std::string segmentName,
                  std::string cpp2pyMsgName,
                  std::string py2cppMsgName,
                  std::string lockableName)
    {
        SetNames(segmentName,
                 cpp2pyMsgName,
                 py2cppMsgName,
                 lockableName,
                 lockableName + ".header");
    };

    /**
     * Sets the names of the named objects, including the protocol header.
     */
    void SetNames(std::string segmentName,
                  std::string cpp2pyMsgName,
                  std::string py2cppMsgName,
                  std::string lockableName,
                  std::string headerName)
    {
        m_defaultConfig.m_names = Ns3AiMsgInterfaceNames{segmentName,
                                                         cpp2pyMsgName,
                                                         py2cppMsgName,
                                                         lockableName,
                                                         headerName};
    };

    /**
     * Sets all names from one namespace prefix. Useful for multiple agents or
     * parallel ns-3 subprocesses, where each instance must use isolated names.
     */
    void SetNamesFromPrefix(const std::string& prefix)
    {
        m_defaultConfig.m_names = MakeNames(prefix);
    };

    /**
     * Gets the impl which has semaphore (synchronization) methods.
     * The old no-argument API remains available and uses the current default config.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface()
    {
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(m_defaultConfig, "default");
    };

    /**
     * Gets an interface under deterministic names produced from an instance id,
     * using the current default config for non-name settings.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(const std::string& instanceId)
    {
        Ns3AiMsgInterfaceConfig config = m_defaultConfig;
        config.m_names = MakeNames(instanceId);
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    /**
     * Gets an interface under explicitly supplied shared-memory object names,
     * using the current default config for non-name settings.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const std::string& segmentName,
        const std::string& cpp2pyMsgName,
        const std::string& py2cppMsgName,
        const std::string& lockableName,
        const std::string& instanceId = "custom")
    {
        Ns3AiMsgInterfaceConfig config = m_defaultConfig;
        config.m_names = Ns3AiMsgInterfaceNames{segmentName,
                                                cpp2pyMsgName,
                                                py2cppMsgName,
                                                lockableName,
                                                lockableName + ".header"};
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    /**
     * Gets an interface under explicitly supplied shared-memory object names,
     * including the protocol header name.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const Ns3AiMsgInterfaceNames& names,
        const std::string& instanceId = "custom")
    {
        Ns3AiMsgInterfaceConfig config = m_defaultConfig;
        config.m_names = names;
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(config, instanceId);
    };

    /**
     * Gets an interface from an explicit per-instance config.
     * This avoids mutating singleton defaults when different interfaces in one
     * process need different settings.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const Ns3AiMsgInterfaceConfig& config,
        const std::string& instanceId = "custom")
    {
        ValidateSchemaSizes<Cpp2PyMsgType, Py2CppMsgType>(config);
        const std::string key = BuildInterfaceKey<Cpp2PyMsgType, Py2CppMsgType>(instanceId, config);
        auto iter = m_interfaces.find(key);
        if (iter != m_interfaces.end())
        {
            return static_cast<Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>*>(iter->second.get());
        }

        auto interface = std::make_unique<Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>>(
            config.m_isMemoryCreator,
            config.m_useVector,
            config.m_handleFinish,
            config.m_size,
            config.m_names.m_segmentName.c_str(),
            config.m_names.m_cpp2pyMsgName.c_str(),
            config.m_names.m_py2cppMsgName.c_str(),
            config.m_names.m_lockableName.c_str(),
            config.m_syncTimeoutUs,
            config.m_names.m_headerName.c_str(),
            config.m_cpp2pySchemaHash,
            config.m_py2cppSchemaHash,
            config.m_cpp2pySchemaVersion,
            config.m_py2cppSchemaVersion);
        auto rawInterface = interface.get();
        m_interfaces.emplace(key, std::move(interface));
        return rawInterface;
    };

  private:
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    void ValidateSchemaSizes(const Ns3AiMsgInterfaceConfig& config) const
    {
        if (config.m_cpp2pySchemaSize != 0 && config.m_cpp2pySchemaSize != sizeof(Cpp2PyMsgType))
        {
            throw std::runtime_error("ns3-ai cpp2py schema size does not match the C++ payload type");
        }
        if (config.m_py2cppSchemaSize != 0 && config.m_py2cppSchemaSize != sizeof(Py2CppMsgType))
        {
            throw std::runtime_error("ns3-ai py2cpp schema size does not match the C++ payload type");
        }
    };

    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    std::string BuildInterfaceKey(const std::string& instanceId,
                                  const Ns3AiMsgInterfaceConfig& config) const
    {
        std::ostringstream oss;
        oss << typeid(Cpp2PyMsgType).name() << '|' << typeid(Py2CppMsgType).name() << '|'
            << instanceId << '|' << config.m_names.m_segmentName << '|'
            << config.m_names.m_cpp2pyMsgName << '|' << config.m_names.m_py2cppMsgName << '|'
            << config.m_names.m_lockableName << '|' << config.m_names.m_headerName << '|'
            << config.m_isMemoryCreator << '|' << config.m_useVector << '|' << config.m_handleFinish
            << '|' << config.m_size << '|' << config.m_syncTimeoutUs << '|'
            << config.m_cpp2pySchemaHash << '|' << config.m_py2cppSchemaHash << '|'
            << config.m_cpp2pySchemaVersion << '|' << config.m_py2cppSchemaVersion;
        return oss.str();
    };

    Ns3AiMsgInterfaceConfig m_defaultConfig{};
    std::unordered_map<std::string, std::unique_ptr<Ns3AiMsgInterfaceBase>> m_interfaces;
};

} // namespace ns3

#define NS3_AI_MSG_FIELD(StructType, FieldType, Member)                                             \
    ns3::Ns3AiMsgField                                                                               \
    {                                                                                                 \
        #Member, FieldType, offsetof(StructType, Member), sizeof(((StructType*)nullptr)->Member), 1   \
    }

#define NS3_AI_MSG_SCHEMA(StructType, Version, ...)                                                  \
    ns3::MakeNs3AiMsgSchema(#StructType, Version, sizeof(StructType), {__VA_ARGS__})

#endif // NS3_AI_MSG_INTERFACE_H
