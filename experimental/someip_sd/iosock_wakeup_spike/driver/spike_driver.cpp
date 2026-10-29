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

// Drives the wakeup spike module: for every primitive and wake source it starts a run, generates the signals
// (datagrams, LoLa samples, or none for the module's own thread) and prints the module's statistics.
//
// usage: iosock_spike_driver <mw_com_config.json> [--count <n>] [--rate <hz>]... [--primitive <name>] [--source <name>]
//        rate 0 = burst; names as printed in the RESULT lines, default all

#include "experimental/someip_sd/iosock_wakeup_spike/spike_protocol.h"

#include "score/message_passing/client_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace spike = score::someip_sd::iosock_wakeup_spike;
using score::message_passing::IClientConnection;

struct Run
{
    spike::Primitive primitive;
    spike::Source source;
    std::uint32_t rate_hz;
};

const char* Name(const spike::Primitive primitive)
{
    switch (primitive)
    {
        case spike::Primitive::kSemaphore:
            return "semaphore";
        case spike::Primitive::kTaskqueue:
            return "taskqueue";
        case spike::Primitive::kMsleepWakeup:
            return "msleep_wakeup";
        default:
            return "none";
    }
}

const char* Name(const spike::Source source)
{
    switch (source)
    {
        case spike::Source::kSocketUpcall:
            return "socket_upcall";
        case spike::Source::kThread:
            return "thread";
        case spike::Source::kLolaHandler:
            return "lola_handler";
        default:
            return "none";
    }
}

std::uint64_t Now()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL) + static_cast<std::uint64_t>(now.tv_nsec);
}

std::string Format(const spike::Percentiles& percentiles)
{
    return std::to_string(percentiles.min_ns / 1000U) + "/" + std::to_string(percentiles.p50_ns / 1000U) + "/" +
           std::to_string(percentiles.p99_ns / 1000U) + "/" + std::to_string(percentiles.max_ns / 1000U);
}

score::cpp::pmr::unique_ptr<IClientConnection> Connect(score::message_passing::ClientFactory& factory)
{
    const score::message_passing::ServiceProtocolConfig protocol{
        spike::kServiceIdentifier, spike::kMessageSize, spike::kMessageSize, spike::kMessageSize};
    const score::message_passing::IClientFactory::ClientConfig config{1U, 4U, true, false, false};
    auto connection = factory.Create(protocol, config);
    if (!connection)
    {
        return connection;
    }
    std::atomic<IClientConnection::State> state{IClientConnection::State::kStarting};
    connection->Start(
        [&state](IClientConnection::State new_state) {
            state.store(new_state);
        },
        [](score::cpp::span<const std::uint8_t>) {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while ((state.load() == IClientConnection::State::kStarting) && (std::chrono::steady_clock::now() < deadline))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    if (state.load() != IClientConnection::State::kReady)
    {
        connection.reset();
    }
    return connection;
}

bool Send(IClientConnection& connection, const spike::Request& request, std::array<std::uint8_t, spike::kMessageSize>& reply)
{
    std::array<std::uint8_t, spike::kMessageSize> buffer{};
    std::memcpy(buffer.data(), &request, sizeof(request));
    const auto result = connection.SendWaitReply(score::cpp::span<const std::uint8_t>{buffer.data(), buffer.size()},
                                                 score::cpp::span<std::uint8_t>{reply.data(), reply.size()});
    return result.has_value() && (result.value().size() == reply.size());
}

std::unique_ptr<score::mw::com::GenericSkeleton> OfferTick()
{
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{spike::kInstanceSpecifier});
    if (!specifier.has_value())
    {
        return nullptr;
    }
    const score::mw::com::DataTypeMetaInfo meta{sizeof(spike::TickSample), alignof(spike::TickSample)};
    // info.events only references the event list.
    const std::vector<score::mw::com::EventInfo> events{{spike::kEventName, meta}};
    score::mw::com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = score::mw::com::GenericSkeleton::Create(specifier.value(), info);
    if (!skeleton.has_value())
    {
        return nullptr;
    }
    auto result = std::make_unique<score::mw::com::GenericSkeleton>(std::move(skeleton).value());
    if (!result->OfferService().has_value())
    {
        return nullptr;
    }
    return result;
}

void Pace(std::chrono::steady_clock::time_point& next, const std::uint32_t rate_hz)
{
    if (rate_hz != 0U)
    {
        next += std::chrono::nanoseconds{1000000000ULL / rate_hz};
        std::this_thread::sleep_until(next);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <mw_com_config.json> [--count <n>] [--rate <hz>]..." << std::endl;
        return 2;
    }
    std::uint32_t count{1000U};
    std::vector<std::uint32_t> rates;
    std::string only_primitive;
    std::string only_source;
    for (int index = 2; (index + 1) < argc; index += 2)
    {
        const std::string option{argv[index]};
        const auto value = static_cast<std::uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
        if (option == "--count")
        {
            count = value;
        }
        else if (option == "--rate")
        {
            rates.push_back(value);
        }
        else if (option == "--primitive")
        {
            only_primitive = argv[index + 1];
        }
        else if (option == "--source")
        {
            only_source = argv[index + 1];
        }
    }
    if (rates.empty())
    {
        rates = {1000U, 0U};
    }

    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{argv[1]}});
    auto tick = OfferTick();
    if (!tick)
    {
        std::cout << "driver: offering the tick service FAILED" << std::endl;
        return 1;
    }
    auto& tick_event = tick->GetEvents().find(spike::kEventName)->second;

    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    const timeval receive_timeout{0, 100000};
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)));
    sockaddr_in module_address{};
    module_address.sin_family = AF_INET;
    module_address.sin_port = htons(spike::kUdpPort);
    module_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::atomic<bool> stop_receiving{false};
    std::atomic<std::uint64_t> echoes_received{0U};
    std::thread receiver{[&]() {
        std::array<char, 256U> buffer{};
        while (!stop_receiving.load())
        {
            if (::recv(socket_fd, buffer.data(), buffer.size(), 0) > 0)
            {
                echoes_received.fetch_add(1U);
            }
        }
    }};

    score::message_passing::ClientFactory factory{};
    auto connection = Connect(factory);
    if (!connection)
    {
        std::cout << "driver: connecting to the spike module FAILED" << std::endl;
        stop_receiving.store(true);
        receiver.join();
        return 1;
    }

    // msleep/wakeup last: calling it from non-kernel threads may take io-sock down.
    std::vector<Run> runs;
    for (const auto primitive :
         {spike::Primitive::kSemaphore, spike::Primitive::kTaskqueue, spike::Primitive::kMsleepWakeup})
    {
        for (const auto source : {spike::Source::kSocketUpcall, spike::Source::kThread, spike::Source::kLolaHandler})
        {
            if ((!only_primitive.empty() && (only_primitive != Name(primitive))) ||
                (!only_source.empty() && (only_source != Name(source))))
            {
                continue;
            }
            for (const auto rate : rates)
            {
                runs.push_back(Run{primitive, source, rate});
            }
        }
    }

    int failures{0};
    for (const auto& run : runs)
    {
        const std::string label = std::string{"primitive="} + Name(run.primitive) + " source=" + Name(run.source) +
                                  " rate_hz=" + std::to_string(run.rate_hz) + " count=" + std::to_string(count);
        std::array<std::uint8_t, spike::kMessageSize> reply{};
        spike::Request request{spike::Command::kStart, run.source, run.primitive, count, run.rate_hz};
        if ((!Send(*connection, request, reply)) || (reply[0] != 1U))
        {
            std::cout << "RESULT " << label << " FAILED to start" << std::endl;
            ++failures;
            continue;
        }
        echoes_received.store(0U);
        auto next = std::chrono::steady_clock::now();
        if (run.source == spike::Source::kSocketUpcall)
        {
            for (std::uint32_t index = 0U; index < count; ++index)
            {
                static_cast<void>(::sendto(socket_fd,
                                           &index,
                                           sizeof(index),
                                           0,
                                           reinterpret_cast<const sockaddr*>(&module_address),
                                           sizeof(module_address)));
                Pace(next, run.rate_hz);
            }
        }
        else if (run.source == spike::Source::kLolaHandler)
        {
            for (std::uint32_t index = 0U; index < count; ++index)
            {
                auto sample = tick_event.Allocate();
                if (sample.has_value())
                {
                    static_cast<spike::TickSample*>(sample.value().Get())->send_ns = Now();
                    static_cast<void>(tick_event.Send(std::move(sample).value()));
                }
                Pace(next, run.rate_hz);
            }
        }
        request.command = spike::Command::kStop;
        if (!Send(*connection, request, reply))
        {
            std::cout << "RESULT " << label << " FAILED to stop (io-sock gone?)" << std::endl;
            ++failures;
            break;
        }
        spike::Result result{};
        std::memcpy(&result, reply.data(), sizeof(result));
        const bool ok = (result.signals > 0U) && (result.lost == 0U) && (result.hung == 0U);
        failures += ok ? 0 : 1;
        std::cout << "RESULT " << label << " signals=" << result.signals << " wakes=" << result.wakes
                  << " empty_wakes=" << result.empty_wakes << " lost=" << result.lost << " hung=" << result.hung
                  << " echoes=" << result.echoes << "/" << echoes_received.load()
                  << " lola_samples=" << result.lola_samples
                  << " wake_us(min/p50/p99/max)=" << Format(result.wake_latency)
                  << " lola_us(min/p50/p99/max)=" << Format(result.lola_latency) << (ok ? " OK" : " FAILED")
                  << std::endl;
    }

    connection->Stop();
    stop_receiving.store(true);
    receiver.join();
    ::close(socket_fd);
    tick->StopOfferService();
    std::cout << "driver: " << runs.size() << " run(s), " << failures << " failure(s)" << std::endl;
    return (failures == 0) ? 0 : 1;
}
