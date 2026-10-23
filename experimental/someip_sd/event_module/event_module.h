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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H

#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/message_passing/server_factory.h"
#include "score/mw/com/types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace score::someip_sd
{

/// User-space stand-in for the io-sock event module. RX: receives SOME/IP notifications on UDP and offers them as
/// LoLa events (one GenericSkeleton per remote SOME/IP service instance). TX: consumes the LoLa events of locally
/// provided service instances (GenericProxy) and sends them as SOME/IP notifications to the remote subscribers.
/// Routing is configured at runtime by the SD daemon via event_module_protocol.
class EventModule final
{
  public:
    EventModule();
    ~EventModule();

    EventModule(const EventModule&) = delete;
    EventModule& operator=(const EventModule&) = delete;
    EventModule(EventModule&&) = delete;
    EventModule& operator=(EventModule&&) = delete;

    bool Start();
    void Stop();

    /// Receive loop; returns once running becomes false.
    void Run(const std::atomic<bool>& running);

  private:
    using ServiceInstance = std::pair<std::uint16_t, std::uint16_t>;
    using RemoteEndpoint = std::pair<std::array<std::uint8_t, 4U>, std::uint16_t>;

    struct Route
    {
        std::uint16_t local_port{0U};
        std::array<std::uint8_t, 4U> remote_ip{};
        std::uint16_t remote_port{0U};
        bool active{false};
        std::vector<std::string> event_names;  // backing storage of the EventInfo names
        std::unique_ptr<score::mw::com::GenericSkeleton> skeleton;
        std::map<std::uint16_t, score::mw::com::GenericSkeletonEvent*> events;
        std::set<std::uint16_t> subscribed;
    };

    struct ProvidedEvent
    {
        bool is_field{false};
        score::mw::com::GenericProxyEvent* proxy_event{nullptr};
        std::set<RemoteEndpoint> subscribers;
        std::uint16_t session{0U};
        std::uint64_t sent{0U};
        /// Latest value of a field, sent to every new subscriber.
        std::optional<score::mw::com::SamplePtr<void>> last_value;  // TODO: getter & setter
    };

    struct ProvidedService
    {
        std::uint16_t local_port{0U};
        std::uint8_t major{0U};
        std::unique_ptr<score::mw::com::GenericProxy> proxy;
        std::map<std::uint16_t, ProvidedEvent> events;
    };

    // Handle incoming routing commands from the SD daemon (message-passing).
    void HandleCommand(const event_module_protocol::Message& message);
    void AddRoute(const event_module_protocol::Message& message);
    void RemoveRoute(const ServiceInstance& key);
    bool OpenSocket(std::uint16_t port);
    // handle incoming datagrams on the UDP sockets.
    void OnDatagram(std::uint16_t local_port,
                    const std::array<std::uint8_t, 4U>& sender_ip,
                    std::uint16_t sender_port,
                    const std::uint8_t* data,
                    std::size_t size);

    // TX, all with mutex_ held.
    void AddProvidedService(const event_module_protocol::Message& message);
    void AddRemoteSubscriber(const event_module_protocol::Message& message);
    void RemoveRemoteSubscriber(const event_module_protocol::Message& message);
    void ConnectProvidedServices();
    void ReleaseProvidedService(ProvidedService& service);
    void ForwardProvidedSamples();
    void SendNotification(const ServiceInstance& key,
                          ProvidedService& service,
                          std::uint16_t event_id,
                          ProvidedEvent& event,
                          const score::mw::com::SamplePtr<void>& sample,
                          const std::set<RemoteEndpoint>& receivers);
    void Wake();

    score::message_passing::ServerFactory server_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IServer> server_;

    // Self-pipe that wakes the receive loop when an application published a LoLa sample.
    // TODO: check if this approach should end up in production, shoudl we use 2 threads for RX & TX?
    int wake_read_fd_{-1};
    int wake_write_fd_{-1};

    std::mutex mutex_;
    std::map<ServiceInstance, Route> routes_;
    std::map<ServiceInstance, ProvidedService> provided_;
    std::map<std::uint16_t, int> sockets_;  // local port -> fd
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint64_t> forwarded_;
};

}  // namespace score::someip_sd

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H
