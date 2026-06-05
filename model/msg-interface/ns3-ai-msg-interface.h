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
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace ns3
{

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
 * \brief Names of the shared-memory objects used by one msg interface instance.
 */
struct Ns3AiMsgInterfaceNames
{
    std::string m_segmentName;
    std::string m_cpp2pyMsgName;
    std::string m_py2cppMsgName;
    std::string m_lockableName;
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
                                   uint64_t sync_timeout_us = DEFAULT_SYNC_TIMEOUT_US)
        : m_cpp2pyStruct(nullptr),
          m_py2CppStruct(nullptr),
          m_cpp2pyVector(nullptr),
          m_py2cppVector(nullptr),
          m_sync(nullptr),
          m_isCreator(is_memory_creator),
          m_useVector(use_vector),
          m_handleFinish(handle_finish),
          m_segName(segment_name),
          m_syncTimeoutUs(sync_timeout_us),
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
            assert(m_sync != nullptr);
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
    const bool m_isCreator;
    const bool m_useVector;
    const bool m_handleFinish;
    const std::string m_segName;
    const uint64_t m_syncTimeoutUs;
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
                                      prefix + ".lock"};
    };

    /**
     * Sets if this side (C++ or Python) is the memory creator.
     * Configuration on two sides must be different
     */
    void SetIsMemoryCreator(bool isMemoryCreator)
    {
        this->m_isMemoryCreator = isMemoryCreator;
    };

    /**
     * Sets if both C++ and Python sides use vector. Configuration on
     * two sides must be same
     */
    void SetUseVector(bool useVector)
    {
        this->m_useVector = useVector;
    };

    /**
     * Sets if both C++ and Python sides handle finish. Configuration on
     * two sides must be same
     */
    void SetHandleFinish(bool handleFinish)
    {
        this->m_handleFinish = handleFinish;
    };

    /**
     * Sets shared memory segment size, only valid for
     * the shared memory creator. Normally the default
     * size is OK.
     */
    void SetMemorySize(uint32_t size)
    {
        this->m_size = size;
    };

    /**
     * Sets per-operation synchronization timeout in microseconds.
     * A value of 0 disables timeout and restores the historical unbounded wait.
     */
    void SetSyncTimeoutUs(uint64_t timeoutUs)
    {
        this->m_syncTimeoutUs = timeoutUs;
    };

    /**
     * Sets per-operation synchronization timeout in milliseconds.
     * A value of 0 disables timeout and restores the historical unbounded wait.
     */
    void SetSyncTimeoutMs(uint64_t timeoutMs)
    {
        this->m_syncTimeoutUs = timeoutMs * 1000;
    };

    /**
     * Gets per-operation synchronization timeout in microseconds.
     */
    uint64_t GetSyncTimeoutUs() const
    {
        return this->m_syncTimeoutUs;
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
        this->m_segmentName = segmentName;
        this->m_cpp2pyMsgName = cpp2pyMsgName;
        this->m_py2cppMsgName = py2cppMsgName;
        this->m_lockableName = lockableName;
    };

    /**
     * Sets all names from one namespace prefix. Useful for multiple agents or
     * parallel ns-3 subprocesses, where each instance must use isolated names.
     */
    void SetNamesFromPrefix(const std::string& prefix)
    {
        Ns3AiMsgInterfaceNames names = MakeNames(prefix);
        SetNames(names.m_segmentName, names.m_cpp2pyMsgName, names.m_py2cppMsgName, names.m_lockableName);
    };

    /**
     * Gets the impl which has semaphore (synchronization) methods.
     * The old no-argument API remains available and uses the current settings.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface()
    {
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(m_segmentName,
                                                          m_cpp2pyMsgName,
                                                          m_py2cppMsgName,
                                                          m_lockableName,
                                                          "default");
    };

    /**
     * Gets an interface under deterministic names produced from an instance id.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(const std::string& instanceId)
    {
        Ns3AiMsgInterfaceNames names = MakeNames(instanceId);
        return GetInterface<Cpp2PyMsgType, Py2CppMsgType>(names.m_segmentName,
                                                          names.m_cpp2pyMsgName,
                                                          names.m_py2cppMsgName,
                                                          names.m_lockableName,
                                                          instanceId);
    };

    /**
     * Gets an interface under explicitly supplied shared-memory object names.
     */
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>* GetInterface(
        const std::string& segmentName,
        const std::string& cpp2pyMsgName,
        const std::string& py2cppMsgName,
        const std::string& lockableName,
        const std::string& instanceId = "custom")
    {
        const std::string key = BuildInterfaceKey<Cpp2PyMsgType, Py2CppMsgType>(instanceId,
                                                                                segmentName,
                                                                                cpp2pyMsgName,
                                                                                py2cppMsgName,
                                                                                lockableName);
        auto iter = m_interfaces.find(key);
        if (iter != m_interfaces.end())
        {
            return static_cast<Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>*>(iter->second.get());
        }

        auto interface = std::make_unique<Ns3AiMsgInterfaceImpl<Cpp2PyMsgType, Py2CppMsgType>>(
            this->m_isMemoryCreator,
            this->m_useVector,
            this->m_handleFinish,
            this->m_size,
            segmentName.c_str(),
            cpp2pyMsgName.c_str(),
            py2cppMsgName.c_str(),
            lockableName.c_str(),
            this->m_syncTimeoutUs);
        auto rawInterface = interface.get();
        m_interfaces.emplace(key, std::move(interface));
        return rawInterface;
    };

  private:
    template <typename Cpp2PyMsgType, typename Py2CppMsgType>
    std::string BuildInterfaceKey(const std::string& instanceId,
                                  const std::string& segmentName,
                                  const std::string& cpp2pyMsgName,
                                  const std::string& py2cppMsgName,
                                  const std::string& lockableName) const
    {
        std::ostringstream oss;
        oss << typeid(Cpp2PyMsgType).name() << '|' << typeid(Py2CppMsgType).name() << '|'
            << instanceId << '|' << segmentName << '|' << cpp2pyMsgName << '|' << py2cppMsgName
            << '|' << lockableName << '|' << m_isMemoryCreator << '|' << m_useVector << '|'
            << m_handleFinish << '|' << m_size << '|' << m_syncTimeoutUs;
        return oss.str();
    };

    bool m_isMemoryCreator{false};
    bool m_useVector{false};
    bool m_handleFinish{false};
    uint32_t m_size = 4096;
    uint64_t m_syncTimeoutUs = Ns3AiMsgInterfaceImpl<uint8_t, uint8_t>::DEFAULT_SYNC_TIMEOUT_US;
    std::string m_segmentName = "My Seg";
    std::string m_cpp2pyMsgName = "My Cpp to Python Msg";
    std::string m_py2cppMsgName = "My Python to Cpp Msg";
    std::string m_lockableName = "My Lockable";
    std::unordered_map<std::string, std::unique_ptr<Ns3AiMsgInterfaceBase>> m_interfaces;
};

} // namespace ns3

#endif // NS3_AI_MSG_INTERFACE_H
