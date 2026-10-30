/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "experimental/someip_sd/iosock_event_module/module/wakeup_signal.h"

#include <semaphore.h>
#include <time.h>

#include <atomic>
#include <cerrno>

namespace
{

constexpr unsigned int kMillisecondsPerSecond{1000U};
constexpr long kNanosecondsPerMillisecond{1000000L};
constexpr long kNanosecondsPerSecond{1000000000L};

sem_t semaphore;
std::atomic<unsigned int> pending_reasons{0U};
// At most one outstanding sem_post: signals while the thread is awake coalesce into one wakeup.
std::atomic<bool> posted{false};

}  // namespace

extern "C" int someip_wakeup_init(void)
{
    return (sem_init(&semaphore, 0, 0U) == 0) ? 0 : errno;
}

extern "C" void someip_wakeup_signal(unsigned int reasons)
{
    // Release: publishes the caller's writes (e.g. pending flags) to the thread acquiring the reasons on timeout.
    pending_reasons.fetch_or(reasons, std::memory_order_release);
    // Release: the waiter's acquire exchange reads this or a later RMW, so even a coalesced signal synchronizes.
    if (!posted.exchange(true, std::memory_order_release))
    {
        static_cast<void>(sem_post(&semaphore));
    }
}

extern "C" unsigned int someip_wakeup_wait(unsigned int timeout_ms)
{
    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += static_cast<time_t>(timeout_ms / kMillisecondsPerSecond);
    deadline.tv_nsec += static_cast<long>(timeout_ms % kMillisecondsPerSecond) * kNanosecondsPerMillisecond;
    if (deadline.tv_nsec >= kNanosecondsPerSecond)
    {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= kNanosecondsPerSecond;
    }
    if (sem_timedwait_monotonic(&semaphore, &deadline) == 0)
    {
        // Cleared before taking the reasons, so a signal racing with this wakeup posts again instead of getting lost.
        // An acquire RMW, not a store: a signal that still saw posted set must happen-before the exchange below.
        static_cast<void>(posted.exchange(false, std::memory_order_acquire));
    }
    // Acquire: pairs with the release fetch_or, also on timeout when no sem_post synchronized.
    return pending_reasons.exchange(0U, std::memory_order_acquire);
}
