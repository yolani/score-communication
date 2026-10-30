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
#include "experimental/someip_sd/iosock_event_module/module/rx_router.h"

#include "experimental/someip_sd/iosock_event_module/module/event_plugin.h"
#include "experimental/someip_sd/iosock_event_module/module/log.h"

#include <utility>

namespace score::someip_sd::iosock_event_module
{
namespace
{

namespace com = score::mw::com;

constexpr EventId kAllEvents{0xFFFFU};

}  // namespace

void RxRouter::AddRoute(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    auto& route = routes_[key];
    route.port = message.local_port;
    route.remote = Endpoint{message.remote_ip, message.remote_port};
    if (!sockets_.OpenUdp(message.local_port))
    {
        return;
    }
    if ((!route.skeleton) && (!CreateSkeleton(key, message, route)))
    {
        return;
    }
    if (!route.active)
    {
        const auto name = protocol::LolaInstanceSpecifier(key.first, key.second);
        if (!route.skeleton->OfferService().has_value())
        {
            someip_log(SOMEIP_LOG_ERROR, "offering LoLa service %s failed", name.c_str());
            return;
        }
        route.active = true;
        someip_log(SOMEIP_LOG_INFO, "offered LoLa service %s", name.c_str());
    }
}

OperationSucceeded RxRouter::CreateSkeleton(const ServiceInstance& key,
                                            const protocol::Message& message,
                                            Route& route)
{
    const auto name = protocol::LolaInstanceSpecifier(key.first, key.second);
    auto specifier = com::InstanceSpecifier::Create(std::string{name});
    if (!specifier.has_value())
    {
        someip_log(SOMEIP_LOG_ERROR, "invalid instance specifier %s", name.c_str());
        return false;
    }
    route.event_names.clear();
    for (const auto event : message.events)
    {
        route.event_names.push_back(protocol::LolaEventName(event));
    }
    const com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
    std::vector<com::EventInfo> events;
    for (const auto& event_name : route.event_names)
    {
        events.push_back(com::EventInfo{event_name, meta});
    }
    com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = com::GenericSkeleton::Create(specifier.value(), info);
    if (!skeleton.has_value())
    {
        someip_log(SOMEIP_LOG_ERROR, "creating LoLa skeleton %s failed (missing in mw_com_config.json?)", name.c_str());
        return false;
    }
    route.skeleton = std::make_unique<com::GenericSkeleton>(std::move(skeleton).value());
    for (const auto event : message.events)
    {
        const auto found = route.skeleton->GetEvents().find(protocol::LolaEventName(event));
        if (found != route.skeleton->GetEvents().cend())
        {
            route.events[event] = &found->second;
        }
        //TODO: should a mismatch message event <-> skeleton event be logged or handled differently?
    }
    return true;
}

void RxRouter::RemoveRoute(const ServiceInstance& key)
{
    const auto found = routes_.find(key);
    if (found == routes_.end())
    {
        return;
    }
    auto& route = found->second;
    route.subscribed.clear();
    if (route.active)
    {
        route.skeleton->StopOfferService();
        route.active = false;
        someip_log(SOMEIP_LOG_INFO,
                   "stopped offering LoLa service %s",
                   protocol::LolaInstanceSpecifier(key.first, key.second).c_str());
    }
}

RxRouter::Route* RxRouter::FindRoute(const ServiceInstance& key)
{
    const auto found = routes_.find(key);
    if (found == routes_.end())
    {
        someip_log(SOMEIP_LOG_WARNING,
                   "subscription for unknown route 0x%04x.0x%04x",
                   static_cast<unsigned>(key.first),
                   static_cast<unsigned>(key.second));
        return nullptr;
    }
    return &found->second;
}

//TODO: handle subscription management properly
void RxRouter::AddSubscription(const ServiceInstance& key, const EventId event)
{
    auto* route = FindRoute(key);
    if (route != nullptr)
    {
        route->subscribed.insert(event);
    }
}

void RxRouter::RemoveSubscription(const ServiceInstance& key, const EventId event)
{
    auto* route = FindRoute(key);
    if (route != nullptr)
    {
        route->subscribed.erase(event);
    }
}

void RxRouter::OnDatagram(const Port port,
                          const Ipv4Address& ip,
                          const Port remote_port,
                          const PacketHandle packet,
                          const PacketSize size)
{
    PacketOffset offset{0U};
    while ((size - offset) >= kHeaderSize)
    {
        SomeipHeader header{};
        someip_event_copy_packet(packet, offset, header.size(), header.data());
        const auto length = Read32(&header[4]);
        if ((length < 8U) || (length > (size - offset - 8U)))
        {
            someip_log(SOMEIP_LOG_WARNING, "malformed SOME/IP datagram dropped");
            return;
        }
        const auto payload_size = length - 8U;
        const auto payload_offset = offset + kHeaderSize;
        offset += length + 8U;
        if ((header[14] != 0x02U) || (payload_size > protocol::kMaxPayloadSize))
        {
            continue;
        }
        const auto id = Read32(header.data());
        const auto service = static_cast<ServiceId>(id >> 16U);
        const auto event = static_cast<EventId>(id);
        // Each notification goes to the first eligible route only, in instance-ID order.
        for (auto entry = routes_.lower_bound(ServiceInstance{service, 0U});
             (entry != routes_.end()) && (entry->first.first == service);
             ++entry)
        {
            auto& route = entry->second;
            if ((!route.active) || (route.port != port) || (route.remote != Endpoint{ip, remote_port}) ||
                ((route.subscribed.count(event) == 0U) && (route.subscribed.count(kAllEvents) == 0U)) ||
                (route.events.count(event) == 0U))
            {
                continue;
            }
            Deliver(entry->first, route, event, header, packet, payload_offset, payload_size);
            break;
        }
    }
}

void RxRouter::Deliver(const ServiceInstance& key,
                       Route& route,
                       const EventId event,
                       const SomeipHeader& header,
                       const PacketHandle packet,
                       const PacketOffset payload_offset,
                       const PacketSize payload_size)
{
    auto* target = route.events.at(event);
    auto sample = target->Allocate();
    if (!sample.has_value())
    {
        someip_log(SOMEIP_LOG_WARNING, "no free LoLa sample slot for event 0x%04x", static_cast<unsigned>(event));
        return;
    }
    auto* data = static_cast<protocol::EventSample*>(sample.value().Get());
    data->size = payload_size;
    data->session = static_cast<SessionId>(Read32(&header[8]));
    data->interface_version = header[13];
    data->message_type = header[14];
    // The only RX payload copy: directly from the socket mbuf chain into the allocated LoLa slot.
    someip_event_copy_packet(packet, payload_offset, payload_size, data->payload.data());
    if (!target->Send(std::move(sample).value()).has_value())
    {
        someip_log(SOMEIP_LOG_ERROR, "sending LoLa sample for event 0x%04x failed", static_cast<unsigned>(event));
        return;
    }
    if (forwarded_[{key.first, event}]++ == 0U)
    {
        someip_log(SOMEIP_LOG_INFO,
                   "RX direct mbuf-to-LoLa: first notification of 0x%04x.0x%04x event 0x%04x",
                   static_cast<unsigned>(key.first),
                   static_cast<unsigned>(key.second),
                   static_cast<unsigned>(event));
    }
}

}  // namespace score::someip_sd::iosock_event_module
