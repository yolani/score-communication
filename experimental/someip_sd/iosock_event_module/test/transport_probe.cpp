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
#include "experimental/someip_sd/protocol/event_module_protocol.h"
#include "score/message_passing/client_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace protocol = score::someip_sd::event_module_protocol;
namespace com = score::mw::com;
using namespace std::chrono_literals;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

class UdpSocket
{
  public:
    explicit UdpSocket(std::uint16_t port) : fd_{::socket(AF_INET, SOCK_DGRAM, 0)}
    {
        Require(fd_ >= 0, "creating probe socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            ::close(fd_);
            throw std::runtime_error{"binding probe socket failed"};
        }
    }

    ~UdpSocket()
    {
        ::close(fd_);
    }

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void Send(std::uint16_t port, const std::vector<std::uint8_t>& data)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        Require(
            ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                static_cast<ssize_t>(data.size()),
            "sending probe datagram failed");
    }

    std::vector<std::uint8_t> Receive(int timeout_ms, std::uint16_t source_port)
    {
        pollfd descriptor{fd_, POLLIN, 0};
        const int result = ::poll(&descriptor, 1U, timeout_ms);
        Require(result >= 0, "polling probe socket failed");
        if (result == 0)
        {
            return {};
        }
        std::array<std::uint8_t, 2048U> data{};
        sockaddr_in from{};
        socklen_t length{sizeof(from)};
        const auto size = ::recvfrom(fd_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&from), &length);
        Require(size >= 0, "receiving probe datagram failed");
        Require(ntohs(from.sin_port) == source_port, "incorrect notification source port");
        return {data.begin(), data.begin() + size};
    }

  private:
    int fd_;
};

class Control
{
  public:
    Control()
    {
        const score::message_passing::ServiceProtocolConfig config{protocol::kServiceIdentifier,
                                                                   protocol::kMaxMessageSize,
                                                                   protocol::kMaxMessageSize,
                                                                   protocol::kMaxMessageSize};
        const score::message_passing::IClientFactory::ClientConfig client_config{1U, 64U, true, false, false};
        connection_ = factory_.Create(config, client_config);
        Require(static_cast<bool>(connection_), "creating control client failed");
        connection_->Start(
            [this](score::message_passing::IClientConnection::State state) {
                ready_.store(state == score::message_passing::IClientConnection::State::kReady);
            },
            [](score::cpp::span<const std::uint8_t>) {});
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while ((!ready_.load()) && (std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(10ms);
        }
        Require(ready_.load(), "control connection did not become ready");
    }

    ~Control()
    {
        connection_->Stop();
    }

    void Send(protocol::Message message)
    {
        const auto bytes = protocol::Encode(message);
        Require(connection_->Send(score::cpp::span<const std::uint8_t>{bytes.data(), bytes.size()}).has_value(),
                "sending routing command failed");
        // The unchanged control protocol is one-way; allow the kernel worker to apply the command.
        std::this_thread::sleep_for(50ms);
    }

  private:
    score::message_passing::ClientFactory factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IClientConnection> connection_;
    std::atomic<bool> ready_{false};
};

void Put32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> Notification(std::uint16_t service,
                                       std::uint16_t event,
                                       std::uint16_t session,
                                       std::uint8_t major,
                                       const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> data(16U + payload.size(), 0U);
    Put32(data.data(), (static_cast<std::uint32_t>(service) << 16U) | event);
    Put32(&data[4], static_cast<std::uint32_t>(8U + payload.size()));
    Put32(&data[8], session);
    data[12] = 1U;
    data[13] = major;
    data[14] = 2U;
    std::copy(payload.begin(), payload.end(), data.begin() + 16);
    return data;
}

std::vector<std::uint8_t> Payload(std::size_t size, std::uint8_t seed)
{
    std::vector<std::uint8_t> result(size);
    for (std::size_t index = 0U; index < size; ++index)
    {
        result[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

struct RxProxy
{
    std::unique_ptr<com::GenericProxy> proxy;
    com::GenericProxyEvent* event{nullptr};

    ~RxProxy()
    {
        if (event != nullptr)
        {
            event->Unsubscribe();
        }
    }

    void Connect()
    {
        auto specifier = com::InstanceSpecifier::Create(std::string{"someip/1234/5678"});
        Require(specifier.has_value(), "invalid RX specifier");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto handles = com::GenericProxy::FindService(specifier.value());
            Require(handles.has_value(), "finding RX service failed");
            if (!handles.value().empty())
            {
                auto candidate = com::GenericProxy::Create(handles.value()[0]);
                Require(candidate.has_value(), "creating RX proxy failed");
                proxy = std::make_unique<com::GenericProxy>(std::move(candidate).value());
                auto found = proxy->GetEvents().find("event_8778");
                Require(found != proxy->GetEvents().cend(), "missing RX event");
                Require(found->second.Subscribe(5U).has_value(), "subscribing RX event failed");
                event = &found->second;
                std::this_thread::sleep_for(100ms);
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error{"RX service did not become available"};
    }

    std::vector<protocol::EventSample> Take(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::vector<protocol::EventSample> result;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do
        {
            const auto received = event->GetNewSamples(
                [&result](auto sample) noexcept {
                    result.push_back(*static_cast<const protocol::EventSample*>(sample.get()));
                },
                5U);
            Require(received.has_value(), "getting RX samples failed");
            if (result.size() >= count)
            {
                return result;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return result;
    }
};

void CheckRx(const protocol::EventSample& sample, const std::vector<std::uint8_t>& payload, std::uint16_t session)
{
    Require(sample.size == payload.size(), "RX size mismatch");
    Require(sample.session == session, "RX session mismatch");
    Require(sample.interface_version == 7U && sample.message_type == 2U, "RX metadata mismatch");
    Require(std::equal(payload.begin(), payload.end(), sample.payload.begin()), "RX payload mismatch");
}

void ReceiveTests(Control& control)
{
    UdpSocket sender{31000U};
    UdpSocket wrong_sender{31001U};
    protocol::Message route{};
    route.opcode = protocol::Opcode::kAddRoute;
    route.service = 0x1234U;
    route.instance = 0x5678U;
    route.local_port = 32000U;
    route.remote_ip = {127U, 0U, 0U, 1U};
    route.remote_port = 31000U;
    route.events = {0x8778U};
    control.Send(route);
    RxProxy receiver;
    receiver.Connect();

    // Given situation: The RX route is active and a LoLa consumer is connected, but forwarding is not subscribed.
    // Action: Send a matching notification from the configured remote endpoint.
    // Expected result: The consumer receives no LoLa sample within 200 ms.
    const auto small = Payload(4U, 1U);
    const auto packet = Notification(0x1234U, 0x8778U, 0x4321U, 7U, small);
    sender.Send(32000U, packet);
    Require(receiver.Take(1U, 200ms).empty(), "forwarded without subscription");
    // Given situation: Forwarding is subscribed for event 0x8778 of service 0x1234 on the configured RX route.
    // Action: Send packets with a wrong source port, destination port, service, event or message type; also send
    // invalid lengths, a truncated header and a 1401-byte payload.
    // Expected result: None of these packets produces a LoLa sample within the subsequent 250 ms check.
    auto subscription = route;
    subscription.opcode = protocol::Opcode::kAddSubscription;
    subscription.event = 0x8778U;
    control.Send(subscription);
    wrong_sender.Send(32000U, packet);
    sender.Send(32001U, packet);
    sender.Send(32000U, Notification(0x1235U, 0x8778U, 0U, 7U, small));
    sender.Send(32000U, Notification(0x1234U, 0x8779U, 0U, 7U, small));
    auto non_notification = packet;
    non_notification[14] = 0U;
    sender.Send(32000U, non_notification);
    auto malformed = packet;
    Put32(&malformed[4], 0xFFFFFFFFU);
    sender.Send(32000U, malformed);
    Put32(&malformed[4], 7U);
    sender.Send(32000U, malformed);
    sender.Send(32000U, std::vector<std::uint8_t>{0U, 1U});
    sender.Send(32000U, Notification(0x1234U, 0x8778U, 0U, 7U, Payload(1401U, 0U)));
    Require(receiver.Take(1U, 250ms).empty(), "forwarded an invalid or unmatched notification");

    // Given situation: The matching RX route and event subscription are active.
    // Action: Send one datagram containing three notifications with 1400-byte payloads and sessions 1, 2 and 3.
    // Expected result: Three LoLa samples arrive in order with exact payloads, sizes, sessions and metadata.
    std::vector<std::uint8_t> combined;
    const auto large = Payload(1400U, 3U);
    for (std::uint16_t session = 1U; session <= 3U; ++session)
    {
        const auto next = Notification(0x1234U, 0x8778U, session, 7U, large);
        combined.insert(combined.end(), next.begin(), next.end());
    }
    sender.Send(32000U, combined);
    const auto samples = receiver.Take(3U, 2s);
    Require(samples.size() == 3U, "concatenated mbuf-chain notifications were not all received");
    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        CheckRx(samples[index], large, static_cast<std::uint16_t>(index + 1U));
    }
    // Given situation: The matching RX route and event subscription remain active.
    // Action: Send a notification with an empty payload and session 4.
    // Expected result: One zero-length LoLa sample arrives with session 4 and the expected metadata.
    sender.Send(32000U, Notification(0x1234U, 0x8778U, 4U, 7U, {}));
    auto empty = receiver.Take(1U, 2s);
    Require(empty.size() == 1U, "empty payload notification missing");
    CheckRx(empty[0], {}, 4U);

    // Given situation: The RX event has an active forwarding subscription.
    // Action: Remove that subscription and send another matching notification.
    // Expected result: The consumer receives no LoLa sample within 200 ms.
    subscription.opcode = protocol::Opcode::kRemoveSubscription;
    control.Send(subscription);
    sender.Send(32000U, packet);
    Require(receiver.Take(1U, 200ms).empty(), "RX continued after unsubscribe");
    // Given situation: The RX route remains active after the event-specific subscription was removed.
    // Action: Add the wildcard event subscription 0xFFFF and send a matching event 0x8778 notification.
    // Expected result: One LoLa sample arrives with the original payload, session and metadata.
    subscription.opcode = protocol::Opcode::kAddSubscription;
    subscription.event = 0xFFFFU;
    control.Send(subscription);
    sender.Send(32000U, packet);
    auto wildcard = receiver.Take(1U, 2s);
    Require(wildcard.size() == 1U, "wildcard subscription did not forward");
    CheckRx(wildcard[0], small, 0x4321U);

    // Given situation: The RX route forwards notifications through a wildcard subscription.
    // Action: Remove the route and send another matching notification.
    // Expected result: The existing LoLa consumer receives no sample within 200 ms.
    auto remove = route;
    remove.opcode = protocol::Opcode::kRemoveRoute;
    control.Send(remove);
    sender.Send(32000U, packet);
    Require(receiver.Take(1U, 200ms).empty(), "removed route still forwarded");
    // Given situation: The RX route has been removed, while the original LoLa consumer remains connected.
    // Action: Re-add the route and event-specific subscription, then send a matching notification.
    // Expected result: The same consumer receives one sample with the original payload, session and metadata.
    control.Send(route);
    subscription.event = 0x8778U;
    control.Send(subscription);
    sender.Send(32000U, packet);
    auto reoffered = receiver.Take(1U, 2s);
    Require(reoffered.size() == 1U, "re-offered route did not retain its skeleton");
    CheckRx(reoffered[0], small, 0x4321U);
    control.Send(remove);
    std::cout << "RX: filtering, concatenation, limits, wildcard, unsubscribe and re-offer OK" << std::endl;
}

void TransmitTests(Control& control)
{
    UdpSocket first{31002U};
    UdpSocket second{31003U};
    auto specifier = com::InstanceSpecifier::Create(std::string{"someip/2345/0001"});
    Require(specifier.has_value(), "invalid TX specifier");
    const com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
    const std::array<com::EventInfo, 1U> events{{{"event_8001", meta}}};
    com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = com::GenericSkeleton::Create(specifier.value(), info);
    Require(skeleton.has_value(), "creating TX skeleton failed");
    Require(skeleton.value().OfferService().has_value(), "offering TX skeleton failed");
    auto& event = skeleton.value().GetEvents().find("event_8001")->second;

    protocol::Message service{};
    service.opcode = protocol::Opcode::kAddProvidedService;
    service.service = 0x2345U;
    service.instance = 1U;
    service.local_port = 32001U;
    service.major = 9U;
    service.events = {0x8001U};
    service.fields = {0x8001U};
    control.Send(service);
    std::this_thread::sleep_for(200ms);
    auto subscriber = service;
    subscriber.opcode = protocol::Opcode::kAddRemoteSubscriber;
    subscriber.event = 0x8001U;
    subscriber.remote_ip = {127U, 0U, 0U, 1U};
    subscriber.remote_port = 31002U;
    control.Send(subscriber);

    std::uint16_t session{0U};
    const auto publish = [&event](const std::vector<std::uint8_t>& payload) {
        auto sample = event.Allocate();
        Require(sample.has_value(), "LoLa slot was not released for reuse");
        auto* data = static_cast<protocol::EventSample*>(sample.value().Get());
        data->size = static_cast<std::uint32_t>(payload.size());
        std::copy(payload.begin(), payload.end(), data->payload.begin());
        Require(event.Send(std::move(sample).value()).has_value(), "publishing provider sample failed");
    };
    const auto check = [&session](UdpSocket& socket, const std::vector<std::uint8_t>& payload, bool next_session) {
        if (next_session)
        {
            ++session;
        }
        const auto actual = socket.Receive(2000, 32001U);
        Require(actual == Notification(0x2345U, 0x8001U, session, 9U, payload), "TX wire bytes mismatch");
    };

    // Given situation: A local LoLa field is provided as service 0x2345/event 0x8001 with one remote subscriber.
    // Action: Publish an eight-byte LoLa sample.
    // Expected result: The subscriber receives byte-exact SOME/IP framing and payload from UDP port 32001,
    // with interface version 9 and session 1.
    const auto initial = Payload(8U, 1U);
    publish(initial);
    check(first, initial, true);
    // Given situation: The subscriber is already registered and the module has cached the published field value.
    // Action: Repeat the identical AddRemoteSubscriber command.
    // Expected result: The existing subscriber receives no duplicate field replay within 200 ms.
    control.Send(subscriber);
    Require(first.Receive(200, 32001U).empty(), "renewal replayed the cached field");
    // Given situation: One remote subscriber exists and the latest field value is cached.
    // Action: Add a second subscriber on UDP port 31003 without publishing a new sample.
    // Expected result: Only the new subscriber receives the cached value, with the next session number.
    subscriber.remote_port = 31003U;
    control.Send(subscriber);
    check(second, initial, true);
    Require(first.Receive(200, 32001U).empty(), "new subscriber replay reached an existing subscriber");

    // Given situation: Both remote subscribers are registered and the LoLa deployment has ten sample slots.
    // Action: Publish 64 samples, alternating 1400-byte and empty payloads, reading both recipients after each send.
    // Expected result: Every allocation succeeds beyond the slot count; both recipients receive exact wire bytes
    // and share the same session for each publication, with the session incrementing between publications.
    for (std::uint16_t index = 0U; index < 64U; ++index)
    {
        const auto payload = Payload((index % 2U) == 0U ? 1400U : 0U, static_cast<std::uint8_t>(index));
        publish(payload);
        check(first, payload, true);
        check(second, payload, false);
    }
    // Given situation: Both remote subscribers receive publications of the provided field.
    // Action: Remove the second subscriber and publish another sample.
    // Expected result: The first subscriber receives the exact notification with the next session; the removed
    // subscriber receives nothing within 200 ms.
    subscriber.opcode = protocol::Opcode::kRemoveRemoteSubscriber;
    control.Send(subscriber);
    publish(initial);
    check(first, initial, true);
    Require(second.Receive(200, 32001U).empty(), "TX continued after remote unsubscribe");

    // Given situation: The service is provided and its first remote subscriber remains registered.
    // Action: Remove the provided-service route, then publish a distinct value on the still-offered LoLa skeleton.
    // Expected result: The first subscriber receives no notification within 250 ms.
    auto remove = service;
    remove.opcode = protocol::Opcode::kRemoveRoute;
    control.Send(remove);
    const auto while_unprovided = Payload(12U, 99U);
    publish(while_unprovided);
    Require(first.Receive(250, 32001U).empty(), "TX continued after StopOffer");
    // Given situation: The provided-service route was removed and LoLa holds a newer value published meanwhile.
    // Action: Re-add the service and first subscriber, then publish another sample.
    // Expected result: The subscriber first receives LoLa's current value with session reset to 1, rather than
    // the old route's cached value, then receives the next publication with session 2.
    control.Send(service);
    subscriber.opcode = protocol::Opcode::kAddRemoteSubscriber;
    subscriber.remote_port = 31002U;
    control.Send(subscriber);
    session = 0U;
    check(first, while_unprovided, true);
    publish(initial);
    check(first, initial, true);
    control.Send(remove);
    skeleton.value().StopOfferService();
    std::cout << "TX: wire framing, fan-out, field replay, renewal, 64 slot reuses, unsubscribe and StopOffer OK"
              << std::endl;
}
}  // namespace

int main()
{
    try
    {
        com::runtime::InitializeRuntime(
            com::runtime::RuntimeConfiguration{score::filesystem::Path{"/opt/someip_sd/etc/mw_com_config.json"}});
        Control control;
        ReceiveTests(control);
        TransmitTests(control);
        std::cout << "result: OK" << std::endl;
        return 0;
    }
    catch (const std::runtime_error& error)
    {
        std::cerr << "transport probe failed: " << error.what() << std::endl;
        return 1;
    }
}
