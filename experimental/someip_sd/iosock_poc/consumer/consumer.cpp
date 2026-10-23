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

// Regular process talking to the io-sock PoC module via score::message_passing, LoLa and UDP.

#include "experimental/someip_sd/iosock_poc/poc_protocol.h"

#include "score/message_passing/client_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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

namespace
{

namespace poc = score::someip_sd::iosock_poc;
using score::message_passing::IClientConnection;

constexpr std::chrono::seconds kTimeout{10};
constexpr std::size_t kSamplesToReceive{5U};

std::unique_ptr<score::mw::com::GenericProxy> lola_proxy;
score::mw::com::GenericProxyEvent* counter_event{nullptr};
score::mw::com::GenericProxyEvent* datagram_event{nullptr};

bool CheckMessagePassing()
{
    const score::message_passing::ServiceProtocolConfig protocol{
        poc::kServiceIdentifier, poc::kMessageSize, poc::kMessageSize, poc::kMessageSize};
    const score::message_passing::IClientFactory::ClientConfig config{1U, 4U, true, false, false};
    score::message_passing::ClientFactory factory{};
    auto connection = factory.Create(protocol, config);
    if (!connection)
    {
        std::cout << "message_passing: creating client FAILED" << std::endl;
        return false;
    }
    std::atomic<IClientConnection::State> state{IClientConnection::State::kStarting};
    connection->Start(
        [&state](IClientConnection::State new_state) {
            state.store(new_state);
        },
        [](score::cpp::span<const std::uint8_t>) {});

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while ((state.load() == IClientConnection::State::kStarting) && (std::chrono::steady_clock::now() < deadline))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    bool ok{false};
    if (state.load() != IClientConnection::State::kReady)
    {
        std::cout << "message_passing: connecting to '" << poc::kServiceIdentifier << "' FAILED" << std::endl;
    }
    else
    {
        const std::uint64_t request{41U};
        std::array<std::uint8_t, sizeof(request)> request_buffer{};
        std::memcpy(request_buffer.data(), &request, sizeof(request));
        std::array<std::uint8_t, sizeof(request)> reply_buffer{};
        const auto reply = connection->SendWaitReply(
            score::cpp::span<const std::uint8_t>{request_buffer.data(), request_buffer.size()},
            score::cpp::span<std::uint8_t>{reply_buffer.data(), reply_buffer.size()});
        if ((!reply.has_value()) || (reply.value().size() != sizeof(request)))
        {
            std::cout << "message_passing: SendWaitReply FAILED" << std::endl;
        }
        else
        {
            std::uint64_t value{0U};
            std::memcpy(&value, reply.value().data(), sizeof(value));
            ok = (value == request + 1U);
            std::cout << "message_passing: reply " << value << (ok ? " OK" : " FAILED") << std::endl;
        }
    }
    connection->Stop();
    return ok;
}

bool CheckLola()
{
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{poc::kInstanceSpecifier});
    if (!specifier.has_value())
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
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
        std::cout << "LoLa: service '" << poc::kInstanceSpecifier << "' not found" << std::endl;
        return false;
    }

    auto proxy = score::mw::com::GenericProxy::Create(handles.value()[0]);
    if (!proxy.has_value())
    {
        std::cout << "LoLa: creating proxy FAILED" << std::endl;
        return false;
    }
    const auto found = proxy.value().GetEvents().find(poc::kEventName);
    const auto found_datagram = proxy.value().GetEvents().find(poc::kDatagramEventName);
    if ((found == proxy.value().GetEvents().cend()) || (found_datagram == proxy.value().GetEvents().cend()))
    {
        std::cout << "LoLa: events missing" << std::endl;
        return false;
    }
    lola_proxy = std::make_unique<score::mw::com::GenericProxy>(std::move(proxy).value());
    counter_event = &lola_proxy->GetEvents().find(poc::kEventName)->second;
    datagram_event = &lola_proxy->GetEvents().find(poc::kDatagramEventName)->second;
    if ((!counter_event->Subscribe(kSamplesToReceive).has_value()) ||
        (!datagram_event->Subscribe(kSamplesToReceive).has_value()))
    {
        std::cout << "LoLa: Subscribe FAILED" << std::endl;
        return false;
    }

    std::size_t received{0U};
    while ((received < kSamplesToReceive) && (std::chrono::steady_clock::now() < deadline + kTimeout))
    {
        static_cast<void>(counter_event->GetNewSamples(
            [&received](auto sample) noexcept {
                std::cout << "LoLa: sample " << *static_cast<const std::uint64_t*>(sample.get()) << std::endl;
                ++received;
            },
            kSamplesToReceive));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::cout << "LoLa: received " << received << " sample(s)" << std::endl;
    return received >= kSamplesToReceive;
}

/// Sends datagrams to the module's kernel socket, expects them echoed back (sosend) and offered via LoLa (from the
/// rx kthread).
bool CheckKernelSocket()
{
    if (datagram_event == nullptr)
    {
        return false;
    }
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        return false;
    }
    const timeval receive_timeout{1, 0};
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)));
    sockaddr_in module_address{};
    module_address.sin_family = AF_INET;
    module_address.sin_port = htons(poc::kUdpPort);
    module_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::size_t echoed{0U};
    for (std::size_t index = 0U; index < kSamplesToReceive; ++index)
    {
        const std::string text{"datagram " + std::to_string(index)};
        if (::sendto(socket_fd,
                     text.data(),
                     text.size(),
                     0,
                     reinterpret_cast<const sockaddr*>(&module_address),
                     sizeof(module_address)) < 0)
        {
            std::cout << "kernel socket: sendto FAILED" << std::endl;
            continue;
        }
        std::array<char, 256U> echo{};
        const auto size = ::recv(socket_fd, echo.data(), echo.size(), 0);
        if ((size > 0) && (std::string{echo.data(), static_cast<std::size_t>(size)} == text))
        {
            std::cout << "kernel socket: echo '" << text << "' OK" << std::endl;
            ++echoed;
        }
        else
        {
            std::cout << "kernel socket: no echo for '" << text << "'" << std::endl;
        }
    }
    ::close(socket_fd);

    std::size_t received{0U};
    bool payloads_match{true};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while ((received < kSamplesToReceive) && (std::chrono::steady_clock::now() < deadline))
    {
        static_cast<void>(datagram_event->GetNewSamples(
            [&received, &payloads_match](auto sample) noexcept {
                const auto* const datagram = static_cast<const poc::DatagramSample*>(sample.get());
                const auto expected = "datagram " + std::to_string(received);
                const bool matches = (datagram->size <= datagram->data.size()) &&
                    (std::string{reinterpret_cast<const char*>(datagram->data.data()), datagram->size} == expected);
                payloads_match = payloads_match && matches;
                std::cout << "LoLa: datagram '"
                          << expected << (matches ? "' OK" : "' FAILED")
                          << std::endl;
                ++received;
            },
            kSamplesToReceive));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::cout << "kernel socket: " << echoed << " echo(s), " << received << " datagram(s) via LoLa" << std::endl;
    return (echoed == kSamplesToReceive) && (received == kSamplesToReceive) && payloads_match;
}

bool CheckApplicationTx()
{
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        return false;
    }
    const timeval receive_timeout{2, 0};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(poc::kTxUdpPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) != 0) ||
        (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0))
    {
        ::close(socket_fd);
        return false;
    }
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{poc::kTxInstanceSpecifier});
    const score::mw::com::DataTypeMetaInfo meta{sizeof(poc::DatagramSample), alignof(poc::DatagramSample)};
    const std::array<score::mw::com::EventInfo, 1U> events{{{poc::kDatagramEventName, meta}}};
    score::mw::com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = score::mw::com::GenericSkeleton::Create(specifier.value(), info);
    if ((!skeleton.has_value()) || (!skeleton.value().OfferService().has_value()))
    {
        ::close(socket_fd);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds{1});
    auto& event = skeleton.value().GetEvents().find(poc::kDatagramEventName)->second;
    std::size_t received{0U};
    constexpr std::size_t kTxSamples{10U};
    for (std::size_t index = 0U; index < kTxSamples; ++index)
    {
        const auto text = "application TX " + std::to_string(index);
        auto sample = event.Allocate();
        if (!sample.has_value())
        {
            break;
        }
        auto* datagram = static_cast<poc::DatagramSample*>(sample.value().Get());
        datagram->size = static_cast<std::uint32_t>(text.size());
        std::memcpy(datagram->data.data(), text.data(), text.size());
        if (!event.Send(std::move(sample).value()).has_value())
        {
            break;
        }
        std::array<char, 256U> buffer{};
        const auto size = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if ((size <= 0) || (std::string{buffer.data(), static_cast<std::size_t>(size)} != text))
        {
            break;
        }
        ++received;
    }
    ::close(socket_fd);
    skeleton.value().StopOfferService();
    std::cout << "application TX: " << received << "/" << kTxSamples << " LoLa samples received via UDP" << std::endl;
    return received == kTxSamples;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <mw_com_config.json>" << std::endl;
        return EXIT_FAILURE;
    }
    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{argv[1]}});

    const bool message_passing_ok = CheckMessagePassing();
    const bool lola_ok = CheckLola();
    const bool kernel_socket_ok = CheckKernelSocket();
    const bool application_tx_ok = CheckApplicationTx();
    if (counter_event != nullptr)
    {
        counter_event->Unsubscribe();
        datagram_event->Unsubscribe();
    }
    // Must be destroyed before the static mw::com runtime objects.
    lola_proxy.reset();
    const bool ok = message_passing_ok && lola_ok && kernel_socket_ok && application_tx_ok;
    std::cout << "result: " << (ok ? "OK" : "FAILED") << " (message_passing " << (message_passing_ok ? "OK" : "FAILED")
              << ", LoLa " << (lola_ok ? "OK" : "FAILED") << ", kernel socket " << (kernel_socket_ok ? "OK" : "FAILED")
              << ", application TX " << (application_tx_ok ? "OK" : "FAILED") << ")" << std::endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
