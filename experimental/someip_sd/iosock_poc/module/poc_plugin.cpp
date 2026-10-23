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
#include "experimental/someip_sd/iosock_poc/module/poc_plugin.h"
#include "experimental/someip_sd/iosock_poc/poc_protocol.h"

// Not exported by //score/message_passing (restricted visibility), available transitively.
#include "score/message_passing/i_server_connection.h"
#include "score/message_passing/server_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <unistd.h>

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

namespace poc = score::someip_sd::iosock_poc;
using score::message_passing::IServerConnection;
using score::message_passing::UserData;

// io-sock has no useful stdout; the integration test reads this file.
constexpr const char* kLogFile = "/tmp/iosock_poc.log";

std::atomic<bool> stop_requested{false};
std::thread worker;
std::mutex log_mutex;
std::ofstream log_file;
// Set by the worker once offered, used by the rx kthread.
std::atomic<score::mw::com::GenericSkeletonEvent*> datagram_event{nullptr};
std::atomic<std::uint64_t> datagrams_forwarded{0U};
std::atomic<std::uint64_t> datagrams_dropped{0U};
std::atomic<std::uint64_t> pending_tx{0U};
std::unique_ptr<score::mw::com::GenericProxy> app_tx_proxy;
score::mw::com::GenericProxyEvent* app_tx_event{nullptr};

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock{log_mutex};
    log_file << "iosock_poc: " << text << std::endl;
}

score::cpp::pmr::unique_ptr<score::message_passing::IServer> StartServer(
    score::message_passing::ServerFactory& factory)
{
    const score::message_passing::ServiceProtocolConfig protocol{
        poc::kServiceIdentifier, poc::kMessageSize, poc::kMessageSize, poc::kMessageSize};
    const score::message_passing::IServerFactory::ServerConfig config{16U, 4U, 16U};
    auto server = factory.Create(protocol, config);
    if (!server)
    {
        Log("message_passing: creating server FAILED");
        return server;
    }

    auto connect = [](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
        Log("message_passing: client connected (pid " + std::to_string(connection.GetClientIdentity().pid) + ")");
        return UserData{std::in_place_type<std::uintptr_t>, 0U};
    };
    auto disconnect = [](IServerConnection&) {
        Log("message_passing: client disconnected");
    };
    auto sent = [](IServerConnection&,
                   score::cpp::span<const std::uint8_t>) -> score::cpp::expected_blank<score::os::Error> {
        return {};
    };
    auto sent_with_reply = [](IServerConnection& connection, score::cpp::span<const std::uint8_t> data)
        -> score::cpp::expected_blank<score::os::Error> {
        if (data.size() != poc::kMessageSize)
        {
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        std::uint64_t value{0U};
        std::memcpy(&value, data.data(), sizeof(value));
        Log("message_passing: request " + std::to_string(value));
        ++value;
        std::array<std::uint8_t, sizeof(value)> reply{};
        std::memcpy(reply.data(), &value, sizeof(value));
        return connection.Reply(score::cpp::span<const std::uint8_t>{reply.data(), reply.size()});
    };

    const auto result = server->StartListening(
        std::move(connect), std::move(disconnect), std::move(sent), std::move(sent_with_reply));
    if (!result.has_value())
    {
        Log("message_passing: StartListening FAILED: " + std::string{result.error().ToString()});
        server.reset();
        return server;
    }
    Log("message_passing: server '" + std::string{poc::kServiceIdentifier} + "' listening");
    return server;
}

std::unique_ptr<score::mw::com::GenericSkeleton> OfferSkeleton()
{
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{poc::kInstanceSpecifier});
    if (!specifier.has_value())
    {
        Log("LoLa: invalid instance specifier");
        return nullptr;
    }
    const score::mw::com::DataTypeMetaInfo meta{sizeof(std::uint64_t), alignof(std::uint64_t)};
    const score::mw::com::DataTypeMetaInfo datagram_meta{sizeof(poc::DatagramSample), alignof(poc::DatagramSample)};
    const std::vector<score::mw::com::EventInfo> events{{poc::kEventName, meta},
                                                        {poc::kDatagramEventName, datagram_meta}};
    score::mw::com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = score::mw::com::GenericSkeleton::Create(specifier.value(), info);
    if (!skeleton.has_value())
    {
        Log("LoLa: GenericSkeleton::Create FAILED: " + std::string{skeleton.error().Message()});
        return nullptr;
    }
    auto result = std::make_unique<score::mw::com::GenericSkeleton>(std::move(skeleton).value());
    const auto offered = result->OfferService();
    if (!offered.has_value())
    {
        Log("LoLa: OfferService FAILED: " + std::string{offered.error().Message()});
        return nullptr;
    }
    Log("LoLa: service '" + std::string{poc::kInstanceSpecifier} + "' offered");
    return result;
}

void Run()
{
    Log("worker thread started in pid " + std::to_string(::getpid()) + ", uid " + std::to_string(::getuid()) +
        ", euid " + std::to_string(::geteuid()));

    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{IOSOCK_POC_MW_COM_CONFIG}});
    Log("mw::com: runtime initialized with " + std::string{IOSOCK_POC_MW_COM_CONFIG});

    score::message_passing::ServerFactory server_factory{};
    auto server = StartServer(server_factory);
    auto skeleton = OfferSkeleton();

    score::mw::com::GenericSkeletonEvent* event{nullptr};
    if (skeleton)
    {
        const auto found = skeleton->GetEvents().find(poc::kEventName);
        if (found != skeleton->GetEvents().cend())
        {
            event = &found->second;
        }
        const auto found_datagram = skeleton->GetEvents().find(poc::kDatagramEventName);
        if (found_datagram != skeleton->GetEvents().cend())
        {
            datagram_event.store(&found_datagram->second);
        }
    }

    std::uint64_t counter{0U};
    while ((!stop_requested.load()) || (pending_tx.load() != 0U))
    {
        if (event != nullptr)
        {
            auto sample = event->Allocate();
            if (sample.has_value())
            {
                *static_cast<std::uint64_t*>(sample.value().Get()) = counter;
                if (event->Send(std::move(sample).value()).has_value() && (counter == 0U))
                {
                    Log("LoLa: first sample sent");
                }
                ++counter;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    datagram_event.store(nullptr);
    if (skeleton)
    {
        skeleton->StopOfferService();
    }
    if (server)
    {
        server->StopListening();
    }
    Log("worker thread stopped after " + std::to_string(counter) + " sample(s), " +
        std::to_string(datagrams_forwarded.load()) + " datagram(s) forwarded, " +
        std::to_string(datagrams_dropped.load()) + " dropped");
}

}  // namespace

namespace
{
struct TxSample
{
    score::mw::com::GenericSkeletonEvent* event;
    score::mw::com::SampleAllocateePtr<void> sample;
};
struct AppTxSample
{
    score::mw::com::SamplePtr<void> sample;
};
}

extern "C" void* iosock_poc_take_app_tx(const unsigned char** payload, unsigned int* size)
{
    if ((datagram_event.load() == nullptr) || stop_requested.load())
    {
        return nullptr;
    }
    if (!app_tx_proxy)
    {
        auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{poc::kTxInstanceSpecifier});
        if (!specifier.has_value())
        {
            return nullptr;
        }
        auto handles = score::mw::com::GenericProxy::FindService(specifier.value());
        if ((!handles.has_value()) || handles.value().empty())
        {
            return nullptr;
        }
        auto proxy = score::mw::com::GenericProxy::Create(handles.value()[0]);
        if (!proxy.has_value())
        {
            return nullptr;
        }
        auto candidate = std::make_unique<score::mw::com::GenericProxy>(std::move(proxy).value());
        const auto found = candidate->GetEvents().find(poc::kDatagramEventName);
        if ((found == candidate->GetEvents().cend()) || (!found->second.Subscribe(5U).has_value()))
        {
            return nullptr;
        }
        app_tx_event = &found->second;
        app_tx_proxy = std::move(candidate);
        Log("TX: subscribed to application LoLa service");
    }
    AppTxSample* context{nullptr};
    static_cast<void>(app_tx_event->GetNewSamples(
        [&context, payload, size](auto sample) noexcept {
            const auto* datagram = static_cast<const poc::DatagramSample*>(sample.get());
            if ((datagram->size == 0U) || (datagram->size > datagram->data.size()))
            {
                return;
            }
            *payload = datagram->data.data();
            *size = datagram->size;
            context = new AppTxSample{std::move(sample)};
            pending_tx.fetch_add(1U);
        },
        1U));
    return context;
}

extern "C" void iosock_poc_release_app_tx(void* context)
{
    delete static_cast<AppTxSample*>(context);
    Log("TX: application LoLa SamplePtr released by mbuf callback");
    pending_tx.fetch_sub(1U);
}

extern "C" void* iosock_poc_prepare_tx(const unsigned char* data, unsigned int size, const unsigned char** payload)
{
    auto* const event = datagram_event.load();
    if ((event == nullptr) || (size > poc::DatagramSample{}.data.size()))
    {
        datagrams_dropped.fetch_add(1U);
        return nullptr;
    }
    auto sample = event->Allocate();
    if (!sample.has_value())
    {
        datagrams_dropped.fetch_add(1U);
        return nullptr;
    }
    auto* const datagram = static_cast<poc::DatagramSample*>(sample.value().Get());
    datagram->size = size;
    std::memcpy(datagram->data.data(), data, size);
    *payload = datagram->data.data();
    auto* const context = new TxSample{event, std::move(sample).value()};
    pending_tx.fetch_add(1U);
    return context;
}

extern "C" void iosock_poc_release_tx(void* context)
{
    std::unique_ptr<TxSample> tx{static_cast<TxSample*>(context)};
    if (!tx->event->Send(std::move(tx->sample)).has_value())
    {
        datagrams_dropped.fetch_add(1U);
        tx.reset();
        pending_tx.fetch_sub(1U);
        return;
    }
    const auto forwarded = datagrams_forwarded.fetch_add(1U) + 1U;
    Log("MEXTADD: LoLa slot released by mbuf callback, sample " + std::to_string(forwarded) + " published");
    tx.reset();
    pending_tx.fetch_sub(1U);
}

extern "C" int iosock_poc_start(void)
{
    log_file.open(kLogFile, std::ios::out | std::ios::trunc);
    Log("module loaded");
    stop_requested.store(false);
    worker = std::thread{Run};
    return 0;
}

extern "C" void iosock_poc_stop(void)
{
    stop_requested.store(true);
    if (worker.joinable())
    {
        worker.join();
    }
    app_tx_proxy.reset();
    app_tx_event = nullptr;
}
