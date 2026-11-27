// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <x86intrin.h>
#include "common/logging.h"
#include <cstdlib>
#include "common/assert.h"
#include "core/hle/kernel/k_interrupt_manager.h"
#include "core/hle/kernel/k_spin_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"

namespace Kernel {

// The scheduler lock's acquire wait, hold and release (reschedule) times.
namespace SchedulerLockStats {
inline std::atomic<u64> takes{}, wait_ns{}, hold_ns{}, update_ns{}, release_ns{}, next{};
// EDEN_SCHED_LOCK_STATS=1 (env.txt), on its own: the thread stats' hooks run under this lock.
inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_SCHED_LOCK_STATS") != nullptr;
    return enabled;
}
inline u64 Now() {
    return __rdtsc();
}
inline void MaybeLog(u64 now) {
    u64 due = next.load(std::memory_order_relaxed);
    if (now < due || !next.compare_exchange_strong(due, now + 10ULL * 1600'000'000ULL) || due == 0) {
        return;
    }
    const u64 n = takes.exchange(0);
    if (n == 0) return;
    LOG_INFO(Kernel, "Scheduler lock over ~10 s: {} takes, mean wait {} TSC ticks, hold {} (update {}), release {}",
             n, wait_ns.exchange(0) / n, hold_ns.exchange(0) / n, update_ns.exchange(0) / n, release_ns.exchange(0) / n);
}
} // namespace SchedulerLockStats

class KernelCore;
class GlobalSchedulerContext;

template <typename SchedulerType>
class KAbstractSchedulerLock {
public:
    explicit KAbstractSchedulerLock(KernelCore& kernel) : m_kernel{kernel} {}

    bool IsLockedByCurrentThread() const {
        return m_owner_thread == GetCurrentThreadPointer(m_kernel);
    }

    void Lock() {
        if (this->IsLockedByCurrentThread()) {
            // If we already own the lock, the lock count should be > 0.
            // For debug, ensure this is true.
            ASSERT(m_lock_count > 0);
        } else {
            // Otherwise, we want to disable scheduling and acquire the spinlock.
            SchedulerType::DisableScheduling(m_kernel);
            if (SchedulerLockStats::Enabled()) {
                const u64 start = SchedulerLockStats::Now();
                m_spin_lock.Lock();
                m_acquired = SchedulerLockStats::Now();
                SchedulerLockStats::wait_ns.fetch_add(m_acquired - start, std::memory_order_relaxed);
            } else {
                m_spin_lock.Lock();
            }

            ASSERT(m_lock_count == 0);
            ASSERT(m_owner_thread == nullptr);

            // Take ownership of the lock.
            m_owner_thread = GetCurrentThreadPointer(m_kernel);
        }

        // Increment the lock count.
        m_lock_count++;
    }

    void Unlock() {
        ASSERT(this->IsLockedByCurrentThread());
        ASSERT(m_lock_count > 0);

        // Release an instance of the lock.
        if ((--m_lock_count) == 0) {
            // Perform a memory barrier here.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // We're no longer going to hold the lock. Take note of what cores need scheduling.
            const u64 update_start = SchedulerLockStats::Enabled() ? SchedulerLockStats::Now() : 0;
            const u64 cores_needing_scheduling =
                SchedulerType::UpdateHighestPriorityThreads(m_kernel);
            if (update_start) {
                SchedulerLockStats::update_ns.fetch_add(SchedulerLockStats::Now() - update_start, std::memory_order_relaxed);
            }

            // Note that we no longer hold the lock, and unlock the spinlock.
            const u64 acquired = m_acquired;
            m_owner_thread = nullptr;
            m_spin_lock.Unlock();

            // Enable scheduling, and perform a rescheduling operation.
            if (SchedulerLockStats::Enabled()) {
                const u64 released = SchedulerLockStats::Now();
                SchedulerType::EnableScheduling(m_kernel, cores_needing_scheduling);
                const u64 end = SchedulerLockStats::Now();
                SchedulerLockStats::takes.fetch_add(1, std::memory_order_relaxed);
                SchedulerLockStats::hold_ns.fetch_add(acquired ? released - acquired : 0, std::memory_order_relaxed);
                SchedulerLockStats::release_ns.fetch_add(end - released, std::memory_order_relaxed);
                SchedulerLockStats::MaybeLog(end);
            } else {
                SchedulerType::EnableScheduling(m_kernel, cores_needing_scheduling);
            }
        }
    }

private:
    friend class GlobalSchedulerContext;

    KernelCore& m_kernel;
    KAlignedSpinLock m_spin_lock{};
    s32 m_lock_count{};
    u64 m_acquired{};
    std::atomic<KThread*> m_owner_thread{};
};

} // namespace Kernel
