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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_SPIKE_PROTOCOL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_SPIKE_PROTOCOL_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace score::someip_sd::iosock_wakeup_spike
{

// Reuses the event module's name: a target secpol policy for the event module only lets io-sock attach that path.
inline constexpr const char* kServiceIdentifier = "someip_event_module";
inline constexpr const char* kInstanceSpecifier = "iosock/spike_tick";
inline constexpr const char* kEventName = "tick";
inline constexpr std::uint16_t kUdpPort = 30750U;

/// Who wakes the module's worker.
enum class Source : std::uint32_t
{
    kSocketUpcall = 1U,  ///< soupcall on the kernel UDP socket, datagrams from the driver
    kThread = 2U,        ///< std::thread inside the module, like the message_passing dispatch thread
    kLolaHandler = 3U,   ///< LoLa receive handler of an event offered by the driver
};

/// How the worker waits and is woken.
enum class Primitive : std::uint32_t
{
    kNone = 0U,
    kSemaphore = 1U,     ///< kthread in sem_timedwait(), sem_post() plus an atomic flag
    kTaskqueue = 2U,     ///< FreeBSD taskqueue with one thread, taskqueue_enqueue()
    kMsleepWakeup = 3U,  ///< kthread in msleep() on a mutex and flag, wakeup()
};

enum class Command : std::uint32_t
{
    kStart = 1U,
    kStop = 2U,
};

/// Driver -> module, sent with reply. rate_hz 0 sends back to back (burst).
struct Request
{
    Command command;
    Source source;
    Primitive primitive;
    std::uint32_t count;
    std::uint32_t rate_hz;
};

struct Percentiles
{
    std::uint32_t min_ns;
    std::uint32_t p50_ns;
    std::uint32_t p99_ns;
    std::uint32_t max_ns;
};

/// Module -> driver, reply to kStop.
struct Result
{
    std::uint64_t signals;      ///< spike_signal() calls
    std::uint64_t wakes;        ///< worker wakes that saw new signals
    std::uint64_t empty_wakes;  ///< worker wakes without new signals (spurious or idle timeouts)
    std::uint64_t lost;         ///< signals only noticed by a timeout or still pending at kStop
    std::uint64_t echoes;       ///< datagrams echoed by the worker (sosend after soreceive)
    std::uint64_t lola_samples;
    std::uint64_t hung;         ///< 1 if the module's signaling thread got stuck in spike_signal()
    Percentiles wake_latency;   ///< last signal -> worker running
    Percentiles lola_latency;   ///< driver Send() -> receive handler running
};

struct TickSample
{
    std::uint64_t send_ns;  ///< CLOCK_MONOTONIC, shared by all processes of the target
};

inline constexpr std::size_t kMessageSize = std::max(sizeof(Request), sizeof(Result));

}  // namespace score::someip_sd::iosock_wakeup_spike

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_SPIKE_PROTOCOL_H
