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

#ifndef NS3_AI_SEMAPHORE_H
#define NS3_AI_SEMAPHORE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

/**
 * \brief Structure providing semaphore operations
 */
struct Ns3AiSemaphore
{
    explicit Ns3AiSemaphore() = default;

    /**
     * Default wait upper bound for legacy callers.
     *
     * A broken C++/Python send-recv order or a dead peer must not trap the
     * process in an infinite busy-wait. 300 s is deliberately conservative so
     * normal slow Python inference is still tolerated while protocol bugs
     * become visible.
     */
    static constexpr uint64_t DEFAULT_SEM_WAIT_TIMEOUT_US = 300000000;

    static inline uint8_t atomic_read8(const volatile uint8_t* mem)
    {
        return AtomicRef(mem)->load(std::memory_order_acquire);
    }

    static inline uint8_t atomic_cas8(volatile uint8_t* mem, uint8_t with, uint8_t cmp)
    {
        uint8_t expected = cmp;
        AtomicRef(mem)->compare_exchange_strong(expected,
                                                with,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
        return expected;
    }

    static inline uint8_t atomic_add8(volatile uint8_t* mem, uint8_t val)
    {
        return AtomicRef(mem)->fetch_add(val, std::memory_order_release);
    }

    static inline bool atomic_add_unless8(volatile uint8_t* mem, uint8_t value, uint8_t unless_this)
    {
        auto* atomicMem = AtomicRef(mem);
        uint8_t current = atomicMem->load(std::memory_order_acquire);
        while (current != unless_this)
        {
            const uint8_t desired = static_cast<uint8_t>(current + value);
            if (atomicMem->compare_exchange_weak(current,
                                                 desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    static inline bool sem_try_wait(volatile uint8_t* mem)
    {
        return atomic_add_unless8(mem, static_cast<uint8_t>(-1), 0);
    }

    /**
     * Waits for a semaphore token without throwing.
     *
     * timeout_us == 0 preserves the historical unbounded wait behavior.
     * abort_flag may point to a shared-memory flag used by a higher-level
     * protocol to stop waiting when the peer has finished.
     */
    static inline bool sem_timed_wait(volatile uint8_t* mem,
                                      uint64_t timeout_us,
                                      const volatile bool* abort_flag = nullptr)
    {
        if (sem_try_wait(mem))
        {
            return true;
        }

        const auto start = std::chrono::steady_clock::now();
        uint32_t attempts = 0;
        while (true)
        {
            if (abort_flag != nullptr && AtomicReadBool(abort_flag))
            {
                return false;
            }
            if (sem_try_wait(mem))
            {
                return true;
            }
            if (timeout_us > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
                if (static_cast<uint64_t>(elapsed.count()) >= timeout_us)
                {
                    return false;
                }
            }
            Backoff(attempts++);
        }
    }

    /**
     * Legacy blocking API. It now fails loudly instead of spinning forever.
     */
    static inline void sem_wait(volatile uint8_t* mem)
    {
        if (!sem_timed_wait(mem, DEFAULT_SEM_WAIT_TIMEOUT_US))
        {
            throw std::runtime_error(
                "ns3-ai semaphore wait timed out. Check that C++ and Python send/recv calls "
                "are paired in the same order, and that the peer process is still alive.");
        }
    }

    /**
     * Blocking API with a caller-specified timeout. timeout_us == 0 means no timeout.
     */
    static inline bool sem_wait(volatile uint8_t* mem,
                                uint64_t timeout_us,
                                const volatile bool* abort_flag = nullptr)
    {
        return sem_timed_wait(mem, timeout_us, abort_flag);
    }

    static inline uint8_t sem_post(volatile uint8_t* mem)
    {
        return atomic_add8(mem, 1);
    }

  private:
    static inline std::atomic<uint8_t>* AtomicRef(volatile uint8_t* mem)
    {
        return reinterpret_cast<std::atomic<uint8_t>*>(const_cast<uint8_t*>(mem));
    }

    static inline const std::atomic<uint8_t>* AtomicRef(const volatile uint8_t* mem)
    {
        return reinterpret_cast<const std::atomic<uint8_t>*>(const_cast<const uint8_t*>(mem));
    }

    static inline bool AtomicReadBool(const volatile bool* mem)
    {
        return reinterpret_cast<const std::atomic<bool>*>(const_cast<const bool*>(mem))
            ->load(std::memory_order_acquire);
    }

    static inline void Backoff(uint32_t attempts)
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
    }
};

#endif // NS3_AI_SEMAPHORE_H
