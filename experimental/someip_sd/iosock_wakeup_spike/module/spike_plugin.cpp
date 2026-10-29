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
#include "experimental/someip_sd/iosock_wakeup_spike/module/spike_plugin.h"
#include "experimental/someip_sd/iosock_wakeup_spike/spike_protocol.h"

// Not exported by //score/message_passing (restricted visibility), available transitively.
#include "score/message_passing/i_server_connection.h"
#include "score/message_passing/server_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <dlfcn.h>
#include <semaphore.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace
{

namespace spike = score::someip_sd::iosock_wakeup_spike;
using score::message_passing::IServerConnection;
using score::message_passing::UserData;
using Nanoseconds = std::uint64_t;

// io-sock has no useful stdout; the integration test reads this file on failures.
constexpr const char* kLogFile = "/tmp/iosock_spike.log";
// Longer than the worker's wait timeout, so the worker has picked up a new primitive before a run starts.
constexpr std::chrono::milliseconds kPrimitiveSwitchDelay{1200};
// Lets in-flight signals reach the worker before kStop evaluates.
constexpr std::chrono::milliseconds kStopGrace{300};
constexpr std::size_t kMaxLatencies{100000U};

std::mutex log_mutex;
std::ofstream log_file;

std::atomic<std::uint32_t> primitive{0U};
std::atomic<std::uint32_t> source{0U};

std::atomic<std::uint64_t> signal_sequence{0U};
std::atomic<std::uint64_t> run_start_sequence{0U};
std::atomic<Nanoseconds> last_signal_ns{0U};
std::atomic<std::uint64_t> handled_sequence{0U};
std::atomic<std::uint64_t> wakes{0U};
std::atomic<std::uint64_t> empty_wakes{0U};
std::atomic<std::uint64_t> lost{0U};
std::atomic<std::uint64_t> echoes{0U};
std::atomic<std::uint64_t> lola_samples{0U};
std::mutex latency_mutex;
std::vector<std::uint32_t> wake_latencies;
std::vector<std::uint32_t> lola_latencies;

sem_t semaphore;
std::atomic<bool> semaphore_posted{false};

std::thread control;
std::thread signaler;
std::atomic<bool> signaler_done{true};
std::chrono::steady_clock::time_point signaler_deadline{};
std::unique_ptr<score::mw::com::GenericProxy> tick_proxy;
score::mw::com::GenericProxyEvent* tick_event{nullptr};

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock{log_mutex};
    log_file << "iosock_spike: " << text << std::endl;
}

Nanoseconds Now()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<Nanoseconds>(now.tv_sec) * 1000000000ULL) + static_cast<Nanoseconds>(now.tv_nsec);
}

void Record(std::vector<std::uint32_t>& latencies, const Nanoseconds latency)
{
    std::lock_guard<std::mutex> lock{latency_mutex};
    if (latencies.size() < kMaxLatencies)
    {
        latencies.push_back(static_cast<std::uint32_t>(std::min<Nanoseconds>(latency, UINT32_MAX)));
    }
}

spike::Percentiles Summarize(std::vector<std::uint32_t> latencies)
{
    spike::Percentiles result{};
    if (latencies.empty())
    {
        return result;
    }
    std::sort(latencies.begin(), latencies.end());
    const auto at = [&latencies](const std::size_t permille) {
        return latencies[std::min(latencies.size() - 1U, (latencies.size() * permille) / 1000U)];
    };
    result.min_ns = latencies.front();
    result.p50_ns = at(500U);
    result.p99_ns = at(990U);
    result.max_ns = latencies.back();
    return result;
}

void ResetStatistics()
{
    run_start_sequence.store(signal_sequence.load());
    handled_sequence.store(run_start_sequence.load());
    wakes.store(0U);
    empty_wakes.store(0U);
    lost.store(0U);
    echoes.store(0U);
    lola_samples.store(0U);
    std::lock_guard<std::mutex> lock{latency_mutex};
    wake_latencies.clear();
    lola_latencies.clear();
}

bool ConnectTickProxy()
{
    if (tick_event != nullptr)
    {
        return true;
    }
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{spike::kInstanceSpecifier});
    if (!specifier.has_value())
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    score::Result<score::mw::com::ServiceHandleContainer<score::mw::com::GenericProxy::HandleType>> handles{};
    do
    {
        handles = score::mw::com::GenericProxy::FindService(specifier.value());
        if (handles.has_value() && (!handles.value().empty()))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    } while (std::chrono::steady_clock::now() < deadline);
    if ((!handles.has_value()) || handles.value().empty())
    {
        Log("LoLa: tick service not found");
        return false;
    }
    auto proxy = score::mw::com::GenericProxy::Create(handles.value()[0]);
    if (!proxy.has_value())
    {
        Log("LoLa: creating tick proxy failed");
        return false;
    }
    tick_proxy = std::make_unique<score::mw::com::GenericProxy>(std::move(proxy).value());
    const auto found = tick_proxy->GetEvents().find(spike::kEventName);
    if ((found == tick_proxy->GetEvents().cend()) || (!found->second.Subscribe(8U).has_value()))
    {
        Log("LoLa: subscribing tick event failed");
        tick_proxy.reset();
        return false;
    }
    tick_event = &found->second;
    // Runs on a LoLa thread: only drain the samples and signal the worker.
    const auto handler = tick_event->SetReceiveHandler([]() {
        const auto received = Now();
        static_cast<void>(tick_event->GetNewSamples(
            [received](auto sample) noexcept {
                const auto* tick = static_cast<const spike::TickSample*>(sample.get());
                Record(lola_latencies, received - tick->send_ns);
                lola_samples.fetch_add(1U);
            },
            8U));
        if (source.load() == static_cast<std::uint32_t>(spike::Source::kLolaHandler))
        {
            spike_signal();
        }
    });
    if (!handler.has_value())
    {
        Log("LoLa: SetReceiveHandler failed");
        return false;
    }
    Log("LoLa: subscribed to tick event with receive handler");
    return true;
}

void GenerateSignals(const std::uint32_t count, const std::uint32_t rate_hz)
{
    const auto period = (rate_hz == 0U) ? std::chrono::nanoseconds{0}
                                        : std::chrono::nanoseconds{1000000000ULL / rate_hz};
    auto next = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        spike_signal();
        if (rate_hz != 0U)
        {
            next += period;
            std::this_thread::sleep_until(next);
        }
    }
    signaler_done.store(true);
}

bool Start(const spike::Request& request)
{
    if (signaler.joinable())
    {
        signaler.join();
    }
    source.store(0U);
    if ((request.source == spike::Source::kLolaHandler) && (!ConnectTickProxy()))
    {
        return false;
    }
    if (primitive.exchange(static_cast<std::uint32_t>(request.primitive)) !=
        static_cast<std::uint32_t>(request.primitive))
    {
        std::this_thread::sleep_for(kPrimitiveSwitchDelay);
    }
    ResetStatistics();
    source.store(static_cast<std::uint32_t>(request.source));
    if (request.source == spike::Source::kThread)
    {
        const auto duration = (request.rate_hz == 0U) ? std::chrono::milliseconds{0}
                                                      : std::chrono::milliseconds{(1000ULL * request.count) /
                                                                                  request.rate_hz};
        signaler_deadline = std::chrono::steady_clock::now() + duration + std::chrono::seconds{3};
        signaler_done.store(false);
        signaler = std::thread{GenerateSignals, request.count, request.rate_hz};
    }
    return true;
}

spike::Result Stop()
{
    spike::Result result{};
    if (signaler.joinable())
    {
        while ((!signaler_done.load()) && (std::chrono::steady_clock::now() < signaler_deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (signaler_done.load())
        {
            signaler.join();
        }
        else
        {
            // Stuck inside spike_signal(): keep the control server responsive for the remaining runs.
            result.hung = 1U;
            signaler.detach();
            Log("signaling thread stuck in spike_signal()");
        }
    }
    std::this_thread::sleep_for(kStopGrace);
    source.store(0U);
    const auto signals = signal_sequence.load();
    const auto handled = handled_sequence.load();
    result.lost = lost.load() + (signals - handled);
    result.signals = signals - run_start_sequence.load();
    result.wakes = wakes.load();
    result.empty_wakes = empty_wakes.load();
    result.echoes = echoes.load();
    result.lola_samples = lola_samples.load();
    {
        std::lock_guard<std::mutex> lock{latency_mutex};
        result.wake_latency = Summarize(wake_latencies);
        result.lola_latency = Summarize(lola_latencies);
    }
    return result;
}

score::cpp::pmr::unique_ptr<score::message_passing::IServer> StartServer(
    score::message_passing::ServerFactory& factory)
{
    const score::message_passing::ServiceProtocolConfig protocol{
        spike::kServiceIdentifier, spike::kMessageSize, spike::kMessageSize, spike::kMessageSize};
    const score::message_passing::IServerFactory::ServerConfig config{4U, 2U, 4U};
    auto server = factory.Create(protocol, config);
    if (!server)
    {
        Log("message_passing: creating server failed");
        return server;
    }
    auto connect = [](IServerConnection&) -> score::cpp::expected<UserData, score::os::Error> {
        return UserData{std::in_place_type<std::uintptr_t>, 0U};
    };
    auto disconnect = [](IServerConnection&) {};
    auto sent = [](IServerConnection&,
                   score::cpp::span<const std::uint8_t>) -> score::cpp::expected_blank<score::os::Error> {
        return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
    };
    auto sent_with_reply = [](IServerConnection& connection, score::cpp::span<const std::uint8_t> data)
        -> score::cpp::expected_blank<score::os::Error> {
        spike::Request request{};
        if (data.size() < sizeof(request))
        {
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        std::memcpy(&request, data.data(), sizeof(request));
        std::array<std::uint8_t, spike::kMessageSize> reply{};
        if (request.command == spike::Command::kStart)
        {
            const std::uint8_t ok = Start(request) ? 1U : 0U;
            reply[0] = ok;
        }
        else
        {
            const auto result = Stop();
            std::memcpy(reply.data(), &result, sizeof(result));
            Log("result source " + std::to_string(static_cast<unsigned>(request.source)) + " primitive " +
                std::to_string(static_cast<unsigned>(request.primitive)) + ": signals " +
                std::to_string(result.signals) + ", lost " + std::to_string(result.lost) + ", wake p50 " +
                std::to_string(result.wake_latency.p50_ns) + " ns");
        }
        return connection.Reply(score::cpp::span<const std::uint8_t>{reply.data(), reply.size()});
    };
    const auto result =
        server->StartListening(std::move(connect), std::move(disconnect), std::move(sent), std::move(sent_with_reply));
    if (!result.has_value())
    {
        Log("message_passing: StartListening failed: " + std::string{result.error().ToString()});
        server.reset();
        return server;
    }
    Log("message_passing: server listening");
    return server;
}

/// The module is installed as <prefix>/bin/mods-iosock-spike.so and reads <prefix>/etc/mw_com_config.json.
std::string ConfigPath()
{
    Dl_info info{};
    std::string path{(dladdr(reinterpret_cast<void*>(&ConfigPath), &info) != 0) ? info.dli_fname : ""};
    const auto bin = path.rfind("/bin/");
    if (bin == std::string::npos)
    {
        return "/opt/iosock_spike/etc/mw_com_config.json";
    }
    return path.substr(0U, bin) + "/etc/mw_com_config.json";
}

// REVIEW: why does this thread even exist? the runtime could be initialized in spike_plugin_start() and the server could be started there as well.
void Run()
{
    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{ConfigPath()}});
    score::message_passing::ServerFactory factory{};
    auto server = StartServer(factory);
    // The module cannot be unloaded, so the control objects live as long as io-sock.
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::hours{1});
    }
}

}  // namespace

extern "C" int spike_plugin_start(void)
{
    log_file.open(kLogFile, std::ios::out | std::ios::trunc);
    if (sem_init(&semaphore, 0, 0U) != 0)
    {
        Log("sem_init failed");
        return -1;
    }
    wake_latencies.reserve(kMaxLatencies);
    lola_latencies.reserve(kMaxLatencies);
    control = std::thread{Run};
    Log("module loaded");
    return 0;
}

extern "C" unsigned int spike_plugin_primitive(void)
{
    return primitive.load(std::memory_order_relaxed);
}

extern "C" unsigned int spike_plugin_source(void)
{
    return source.load(std::memory_order_relaxed);
}

extern "C" void spike_plugin_note_signal(void)
{
    last_signal_ns.store(Now(), std::memory_order_relaxed);
    signal_sequence.fetch_add(1U, std::memory_order_release);
}

extern "C" void spike_plugin_note_wake(int timed_out)
{
    const auto sequence = signal_sequence.load(std::memory_order_acquire);
    const auto signaled = last_signal_ns.load(std::memory_order_relaxed);
    const auto now = Now();
    const auto previous = handled_sequence.exchange(sequence);
    if (sequence == previous)
    {
        empty_wakes.fetch_add(1U);
    }
    else if (timed_out != 0)
    {
        lost.fetch_add(sequence - previous);
    }
    else
    {
        wakes.fetch_add(1U);
        Record(wake_latencies, (now > signaled) ? (now - signaled) : 0U);
    }
}

extern "C" void spike_plugin_note_echo(void)
{
    echoes.fetch_add(1U);
}

extern "C" void spike_semaphore_post(void)
{
    if (!semaphore_posted.exchange(true))
    {
        static_cast<void>(sem_post(&semaphore));
    }
}

extern "C" int spike_semaphore_wait(unsigned int timeout_ms)
{
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += static_cast<time_t>(timeout_ms / 1000U);
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    if (sem_timedwait(&semaphore, &deadline) != 0)
    {
        return 0;
    }
    semaphore_posted.store(false);
    return 1;
}
