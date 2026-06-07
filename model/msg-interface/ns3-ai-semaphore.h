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
#include <ostream>
#include <stdexcept>
#include <thread>

/**
 * \brief 二元信号量，基于 std::atomic 实现。
 *
 * 信号量仅在 0（空闲）和 1（已占用）之间振荡。
 * sem_post 使用 exchange(1) 饱和写入，不会回绕。
 * sem_try_wait 使用单次 CAS(1→0) 获取令牌。
 */
struct Ns3AiSemaphore
{
    /** \brief sem_timed_wait 的返回结果 */
    enum class WaitResult : uint8_t
    {
        Acquired = 0, // 成功获取令牌
        Timeout = 1,  // 超时
        Aborted = 2   // 对端完成（abort_flag 触发）
    };

    /** \brief WaitResult 的流式输出，用于测试和诊断 */
    friend inline std::ostream& operator<<(std::ostream& os, WaitResult wr)
    {
        switch (wr)
        {
        case WaitResult::Acquired:
            return os << "Acquired";
        case WaitResult::Timeout:
            return os << "Timeout";
        case WaitResult::Aborted:
            return os << "Aborted";
        }
        return os << "Unknown";
    }

    /**
     * 旧式阻塞等待的超时上限。
     *
     * 错误配对或对端死亡不应导致无限忙等。
     * 300 秒足够容忍慢速 Python 推理，同时确保协议 bug 可见。
     */
    static constexpr uint64_t DEFAULT_SEM_WAIT_TIMEOUT_US = 300000000;

    /**
     * 尝试获取令牌，不等待。
     * \return true 表示获取成功，false 表示计数器为 0。
     */
    static inline bool sem_try_wait(std::atomic<uint8_t>* mem)
    {
        uint8_t expected = 1;
        return mem->compare_exchange_strong(expected,
                                            0,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed);
    }

    /**
     * 带超时和中止标志的等待。
     *
     * \param mem            信号量计数器
     * \param timeout_us     超时微秒数；0 表示无超时
     * \param abort_flag     对端完成标志，触发后返回 WaitResult::Aborted
     * \return               获取结果
     */
    static inline WaitResult sem_timed_wait(std::atomic<uint8_t>* mem,
                                            uint64_t timeout_us,
                                            const std::atomic<bool>* abort_flag = nullptr)
    {
        if (sem_try_wait(mem))
        {
            return WaitResult::Acquired;
        }

        const auto start = std::chrono::steady_clock::now();
        uint32_t attempts = 0;
        while (true)
        {
            if (abort_flag != nullptr && abort_flag->load(std::memory_order_acquire))
            {
                return WaitResult::Aborted;
            }
            if (sem_try_wait(mem))
            {
                return WaitResult::Acquired;
            }
            if (timeout_us > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
                if (static_cast<uint64_t>(elapsed.count()) >= timeout_us)
                {
                    return WaitResult::Timeout;
                }
            }
            Backoff(attempts++);
        }
    }

    /**
     * 旧式阻塞 API。失败时立即抛异常，而非无限自旋。
     */
    static inline void sem_wait(std::atomic<uint8_t>* mem)
    {
        if (sem_timed_wait(mem, DEFAULT_SEM_WAIT_TIMEOUT_US) != WaitResult::Acquired)
        {
            throw std::runtime_error(
                "ns3-ai 信号量等待超时。请检查 C++ 和 Python 的 send/recv 调用是否成对且顺序一致，"
                "以及对端进程是否仍然存活。");
        }
    }

    /**
     * 带调用者指定超时的等待。
     * timeout_us == 0 表示无超时。
     * \return WaitResult::Acquired 表示获取成功，否则为失败原因。
     */
    static inline WaitResult sem_wait(std::atomic<uint8_t>* mem,
                                      uint64_t timeout_us,
                                      const std::atomic<bool>* abort_flag = nullptr)
    {
        return sem_timed_wait(mem, timeout_us, abort_flag);
    }

    /**
     * 释放令牌（计数器置 1）。
     *
     * 使用 exchange(1) 饱和写入，避免 uint8_t 回绕。
     * 若旧值 >= 1，表示重复释放（协议 bug），输出诊断日志。
     * \return 旧值。
     */
    static inline uint8_t sem_post(std::atomic<uint8_t>* mem)
    {
        return mem->exchange(1, std::memory_order_release);
    }

    /**
     * 忙等待回退策略：yield → 50μs sleep → 1ms sleep。
     */
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
