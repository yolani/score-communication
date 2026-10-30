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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_RX_ROUTER_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_RX_ROUTER_H

#include "experimental/someip_sd/iosock_event_module/module/kernel_sockets.h"
#include "experimental/someip_sd/iosock_event_module/module/types.h"

#include "score/mw/com/types.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace score::someip_sd::iosock_event_module
{

/// RX direction: remote services (routes) are offered as LoLa skeletons; subscribed notifications are copied
/// from the received mbuf chain directly into LoLa sample slots.
class RxRouter
{
  public:
    explicit RxRouter(KernelSockets& sockets) : sockets_{sockets} {}

    void AddRoute(const protocol::Message& message);
    void RemoveRoute(const ServiceInstance& key);
    void AddSubscription(const ServiceInstance& key, EventId event);
    void RemoveSubscription(const ServiceInstance& key, EventId event);
    void OnDatagram(Port port, const Ipv4Address& ip, Port remote_port, PacketHandle packet, PacketSize size);

  private:
    using RouteActive = bool;

    struct Route
    {
        Port port{0U};
        Endpoint remote{};
        RouteActive active{false};
        std::vector<std::string> event_names;
        std::unique_ptr<score::mw::com::GenericSkeleton> skeleton;
        std::map<EventId, score::mw::com::GenericSkeletonEvent*> events;
        std::set<EventId> subscribed;
    };

    Route* FindRoute(const ServiceInstance& key);
    OperationSucceeded CreateSkeleton(const ServiceInstance& key, const protocol::Message& message, Route& route);
    void Deliver(const ServiceInstance& key,
                 Route& route,
                 EventId event,
                 const SomeipHeader& header,
                 PacketHandle packet,
                 PacketOffset payload_offset,
                 PacketSize payload_size);

    KernelSockets& sockets_;
    std::map<ServiceInstance, Route> routes_;
    std::map<ServiceEvent, NotificationCount> forwarded_;
};

}  // namespace score::someip_sd::iosock_event_module

#endif
