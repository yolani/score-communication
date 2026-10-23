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
#include "experimental/someip_sd/iosock_event_module/module/event_plugin.h"
#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/message_passing/i_server_connection.h"
#include "score/message_passing/server_factory.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

//TODO: keep namespace original?
namespace protocol = score::someip_sd::event_module_protocol;
namespace com = score::mw::com;

//TODO: move to header
using ServiceId = std::uint16_t;
using InstanceId = std::uint16_t;
using EventId = std::uint16_t;
using Port = std::uint16_t;
using SessionId = std::uint16_t;
using InterfaceVersion = std::uint8_t;
using Byte = std::uint8_t;
using WireWord = std::uint32_t;
using PacketSize = std::uint32_t;
using PacketOffset = std::uint32_t;
using SampleCount = std::size_t;
using CommandCount = std::size_t;
using NotificationCount = std::uint64_t;
using TxReferenceCount = std::uint64_t;
using HexValue = std::uint16_t;
using ConnectionUserData = std::uintptr_t;
using ConfigPath = const char*;
using OperationSucceeded = bool;
using RouteActive = bool;
using FieldEvent = bool;
using StopRequested = bool;
using ErrorCode = int;
using TransportPort = unsigned short;
using TransportByte = unsigned char;
using TransportPacketSize = unsigned int;
using PacketHandle = const void*;
using TxContextHandle = void*;

using ServiceInstance = std::pair<ServiceId, InstanceId>;
using ServiceEvent = std::pair<ServiceId, EventId>;
using Ipv4Address = std::array<Byte, 4U>;
using Endpoint = std::pair<Ipv4Address, Port>;
using score::message_passing::IServerConnection;
using score::message_passing::UserData;

constexpr PacketSize kHeaderSize{16U};
constexpr SampleCount kTxMaxSamples{3U};
constexpr CommandCount kMaxPendingCommands{64U};
constexpr ConfigPath kConfig{"/opt/someip_sd/etc/mw_com_config.json"};
using SomeipHeader = std::array<Byte, kHeaderSize>;

std::mutex log_mutex;
std::ofstream log_file;
std::atomic<TxReferenceCount> pending_tx{0U};
std::atomic<TxReferenceCount> released_tx{0U};

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock{log_mutex};
    log_file << "[IoSockEventModule] " << text << std::endl;
}

std::string Hex(HexValue value)
{
    return "0x" + protocol::Hex4(value);
}

std::string Name(const ServiceInstance& key)
{
    return Hex(key.first) + "." + Hex(key.second);
}

WireWord Read32(const Byte* data)
{
    return (static_cast<WireWord>(data[0]) << 24U) | (static_cast<WireWord>(data[1]) << 16U) |
           (static_cast<WireWord>(data[2]) << 8U) | static_cast<WireWord>(data[3]);
}

void Write32(Byte* data, WireWord value)
{
    data[0] = static_cast<Byte>(value >> 24U);
    data[1] = static_cast<Byte>(value >> 16U);
    data[2] = static_cast<Byte>(value >> 8U);
    data[3] = static_cast<Byte>(value);
}

struct ProxyOwner
{
    std::unique_ptr<com::GenericProxy> proxy;
    std::vector<com::GenericProxyEvent*> subscribed;

    ~ProxyOwner()
    {
        for (auto* event : subscribed)
        {
            event->Unsubscribe();
        }
    }
};

struct SampleLease
{
    // Members are destroyed in reverse order: release the sample before the last proxy owner.
    std::shared_ptr<ProxyOwner> owner;
    com::SamplePtr<void> sample;
};

struct TxContext
{
    std::shared_ptr<SampleLease> lease;
};

class EventModule
{
  public:
    OperationSucceeded Start();
    void Stop();
    void Poll();
    void OnDatagram(Port port, const Ipv4Address& ip, Port remote_port, PacketHandle packet, PacketSize size);

  private:
    struct Route
    {
        Port port{0U};
        Endpoint remote{};
        RouteActive active{false};
        std::vector<std::string> event_names;
        std::unique_ptr<com::GenericSkeleton> skeleton;
        std::map<EventId, com::GenericSkeletonEvent*> events;
        std::set<EventId> subscribed;
    };

    struct ProvidedEvent
    {
        FieldEvent is_field{false};
        com::GenericProxyEvent* proxy_event{nullptr};
        std::set<Endpoint> subscribers;
        SessionId session{0U};
        NotificationCount sent{0U};
        std::shared_ptr<SampleLease> last_value;
    };

    struct ProvidedService
    {
        Port port{0U};
        InterfaceVersion major{0U};
        std::shared_ptr<ProxyOwner> owner;
        std::map<EventId, ProvidedEvent> events;
    };

    OperationSucceeded OpenSocket(Port port);
    void HandleCommand(const protocol::Message& message);
    void AddRoute(const protocol::Message& message);
    void AddProvidedService(const protocol::Message& message);
    void AddRemoteSubscriber(const protocol::Message& message);
    void ConnectProvidedServices();
    void ForwardProvidedSamples();
    void SendNotification(const ServiceInstance& key,
                          ProvidedService& service,
                          EventId event_id,
                          ProvidedEvent& event,
                          const std::shared_ptr<SampleLease>& sample,
                          const std::set<Endpoint>& receivers);

    score::message_passing::ServerFactory server_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IServer> server_;
    std::mutex command_mutex_;
    std::condition_variable command_space_;
    StopRequested stopping_{false};
    std::vector<protocol::Message> commands_;
    std::map<ServiceInstance, Route> routes_;
    std::map<ServiceInstance, ProvidedService> provided_;
    std::map<ServiceInstance, std::shared_ptr<ProxyOwner>> retired_;
    std::set<Port> ports_;
    std::map<ServiceEvent, NotificationCount> forwarded_;
};

OperationSucceeded EventModule::Start()
{
    const score::message_passing::ServiceProtocolConfig config{
        protocol::kServiceIdentifier, protocol::kMaxMessageSize, protocol::kMaxMessageSize, protocol::kMaxMessageSize};
    const score::message_passing::IServerFactory::ServerConfig server_config{64U, 8U, 64U};
    server_ = server_factory_.Create(config, server_config);
    if (!server_)
    {
        Log("creating control server failed");
        return false;
    }
    auto connect = [](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
        Log("SD daemon connected (pid " + std::to_string(connection.GetClientIdentity().pid) + ")");
        return UserData{std::in_place_type<ConnectionUserData>, 0U};
    };
    auto disconnect = [](IServerConnection&) {
        Log("SD daemon disconnected");
    };
    auto sent = [this](IServerConnection&,
                       score::cpp::span<const Byte> data) -> score::cpp::expected_blank<score::os::Error> {
        auto message = protocol::Decode(data.data(), data.size());
        if (!message.has_value())
        {
            Log("invalid routing command dropped");
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        std::unique_lock<std::mutex> lock{command_mutex_};
        command_space_.wait(lock, [this]() {
            return stopping_ || (commands_.size() < kMaxPendingCommands);
        });
        if (stopping_)
        {
            Log("routing command rejected during startup rollback");
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        commands_.push_back(std::move(message).value());
        return {};
    };
    auto with_reply = [](IServerConnection&,
                         score::cpp::span<const Byte>) -> score::cpp::expected_blank<score::os::Error> {
        Log("unexpected control request with reply");
        return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
    };
    const auto result =
        server_->StartListening(std::move(connect), std::move(disconnect), std::move(sent), std::move(with_reply));
    if (!result.has_value())
    {
        Log("starting control server failed: " + std::string{result.error().ToString()});
        server_.reset();
        return false;
    }
    return true;
}

void EventModule::Stop()
{
    {
        std::lock_guard<std::mutex> lock{command_mutex_};
        stopping_ = true;
    }
    command_space_.notify_all();
    if (server_)
    {
        server_->StopListening();
        server_.reset();
    }
}

OperationSucceeded EventModule::OpenSocket(Port port)
{
    if (ports_.count(port) != 0U)
    {
        return true;
    }
    const ErrorCode error = someip_event_open_socket(port);
    if (error != 0)
    {
        Log("opening kernel UDP socket on port " + std::to_string(port) + " failed: " + std::to_string(error));
        return false;
    }
    ports_.insert(port);
    Log("listening on kernel UDP port " + std::to_string(port));
    return true;
}

void EventModule::HandleCommand(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    switch (message.opcode)
    {
        //TODO: move logic to dedicated functions
        case protocol::Opcode::kAddRoute:
            AddRoute(message);
            break;
        case protocol::Opcode::kRemoveRoute:
        {
            const auto provided = provided_.find(key);
            if (provided != provided_.end())
            {
                if (provided->second.owner)
                {
                    retired_[key] = std::move(provided->second.owner);
                }
                provided_.erase(provided);
                Log("stopped providing " + Name(key));
                Log("pending TX references: " + std::to_string(pending_tx.load()));
                break;
            }
            const auto found = routes_.find(key);
            if (found != routes_.end())
            {
                auto& route = found->second;
                route.subscribed.clear();
                if (route.active)
                {
                    route.skeleton->StopOfferService();
                    route.active = false;
                    Log("stopped offering LoLa service " + protocol::LolaInstanceSpecifier(key.first, key.second));
                }
            }
            break;
        }
        //TODO: handle subscription management properly
        case protocol::Opcode::kAddSubscription:
        case protocol::Opcode::kRemoveSubscription:
        {
            const auto found = routes_.find(key);
            if (found == routes_.end())
            {
                Log("subscription for unknown route " + Name(key));
                break;
            }
            if (message.opcode == protocol::Opcode::kAddSubscription)
            {
                found->second.subscribed.insert(message.event);
            }
            else
            {
                found->second.subscribed.erase(message.event);
            }
            break;
        }
        case protocol::Opcode::kAddProvidedService:
            AddProvidedService(message);
            break;
        case protocol::Opcode::kAddRemoteSubscriber:
            AddRemoteSubscriber(message);
            break;
        case protocol::Opcode::kRemoveRemoteSubscriber:
        {
            const auto service = provided_.find(key);
            if (service != provided_.end())
            {
                const auto event = service->second.events.find(message.event);
                if (event != service->second.events.end())
                {
                    event->second.subscribers.erase(Endpoint{message.remote_ip, message.remote_port});
                }
            }
            break;
        }
        default:
            Log("unknown routing command dropped");
            break;
    }
}

void EventModule::AddRoute(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    auto& route = routes_[key];
    route.port = message.local_port;
    route.remote = Endpoint{message.remote_ip, message.remote_port};
    if (!OpenSocket(message.local_port))
    {
        return;
    }
    const auto name = protocol::LolaInstanceSpecifier(key.first, key.second);
    if (!route.skeleton)
    {
        auto specifier = com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            Log("invalid instance specifier " + name);
            return;
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
            Log("creating LoLa skeleton " + name + " failed (missing in mw_com_config.json?)");
            return;
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
    }
    if (!route.active)
    {
        if (!route.skeleton->OfferService().has_value())
        {
            Log("offering LoLa service " + name + " failed");
            return;
        }
        route.active = true;
        Log("offered LoLa service " + name);
    }
}

void EventModule::OnDatagram(Port port, const Ipv4Address& ip, Port remote_port, PacketHandle packet, PacketSize size)
{
    PacketOffset offset{0U};
    while ((size - offset) >= kHeaderSize)
    {
        SomeipHeader header{};
        someip_event_copy_packet(packet, offset, header.size(), header.data());
        const auto length = Read32(&header[4]);
        if ((length < 8U) || (length > (size - offset - 8U)))
        {
            Log("malformed SOME/IP datagram dropped");
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
        for (auto entry = routes_.lower_bound(ServiceInstance{service, 0U});
             (entry != routes_.end()) && (entry->first.first == service);
             ++entry)
        {
            auto& route = entry->second;
            if ((!route.active) || (route.port != port) ||
                (route.remote != Endpoint{ip, remote_port}) ||
                ((route.subscribed.count(event) == 0U) && (route.subscribed.count(0xFFFFU) == 0U)))
            {
                continue;
            }
            const auto target = route.events.find(event);
            if (target == route.events.end())
            {
                continue;
            }
            auto sample = target->second->Allocate();
            if (!sample.has_value())
            {
                Log("no free LoLa sample slot for event " + Hex(event));
                break;
            }
            auto* data = static_cast<protocol::EventSample*>(sample.value().Get());
            data->size = payload_size;
            data->session = static_cast<SessionId>(Read32(&header[8]));
            data->interface_version = header[13];
            data->message_type = header[14];
            // The only RX payload copy: directly from the socket mbuf chain into the allocated LoLa slot.
            someip_event_copy_packet(packet, payload_offset, payload_size, data->payload.data());
            if (!target->second->Send(std::move(sample).value()).has_value())
            {
                Log("sending LoLa sample for event " + Hex(event) + " failed");
                break;
            }
            if (forwarded_[{service, event}]++ == 0U)
            {
                Log("RX direct mbuf-to-LoLa: first notification of " + Name(entry->first) + " event " + Hex(event));
            }
            break;
        }
    }
}

void EventModule::AddProvidedService(const protocol::Message& message)
{
    if (!OpenSocket(message.local_port))
    {
        return;
    }
    const ServiceInstance key{message.service, message.instance};
    auto& service = provided_[key];
    service.port = message.local_port;
    service.major = message.major;
    for (const auto event : message.events)
    {
        service.events[event].is_field =
            std::find(message.fields.begin(), message.fields.end(), event) != message.fields.end();
    }
    Log("providing " + Name(key));
}

void EventModule::AddRemoteSubscriber(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    const auto service = provided_.find(key);
    if (service == provided_.end())
    {
        Log("remote subscriber for unknown provided service " + Name(key));
        return;
    }
    const auto found = service->second.events.find(message.event);
    if (found == service->second.events.end())
    {
        Log("remote subscriber for unknown event " + Hex(message.event));
        return;
    }
    auto& event = found->second;
    const Endpoint endpoint{message.remote_ip, message.remote_port};
    if (!event.subscribers.insert(endpoint).second)
    {
        //TODO: silent bailout really good?
        return;
    }
    if (event.is_field && event.last_value)
    {
        SendNotification(key, service->second, message.event, event, event.last_value, {endpoint});
        Log("sent initial field value of event " + Hex(message.event));
    }
}

void EventModule::ConnectProvidedServices()
{
    //TODO: separate concerns
    for (auto retired = retired_.begin(); retired != retired_.end();)
    {
        if (retired->second.use_count() == 1)
        {
            retired = retired_.erase(retired);
        }
        else
        {
            ++retired;
        }
    }
    //TODO: connect only to services that have new subscribers
    for (auto& entry : provided_)
    {
        auto& service = entry.second;
        // Do not re-subscribe the same PID/event before the old proxy has released all of its samples.
        if (service.owner || (retired_.count(entry.first) != 0U))
        {
            continue;
        }
        const auto name = protocol::LolaInstanceSpecifier(entry.first.first, entry.first.second);
        auto specifier = com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            Log("invalid provided instance specifier " + name);
            continue;
        }
        //TODO: use StartFindService instead of FindService for asynchronous discovery
        auto handles = com::GenericProxy::FindService(specifier.value());
        if (!handles.has_value())
        {
            Log("finding LoLa service " + name + " failed");
            continue;
        }
        if (handles.value().empty())
        {
            continue;
        }
        auto proxy = com::GenericProxy::Create(handles.value()[0]);
        if (!proxy.has_value())
        {
            Log("creating LoLa proxy " + name + " failed");
            continue;
        }
        auto owner = std::make_shared<ProxyOwner>();
        owner->proxy = std::make_unique<com::GenericProxy>(std::move(proxy).value());
        for (auto& event : service.events)
        {
            const auto found = owner->proxy->GetEvents().find(protocol::LolaEventName(event.first));
            if (found == owner->proxy->GetEvents().cend())
            {
                Log("LoLa service " + name + " has no event " + Hex(event.first));
                continue;
            }
            if (!found->second.Subscribe(kTxMaxSamples).has_value())
            {
                Log("subscribing LoLa event " + Hex(event.first) + " failed");
                continue;
            }
            event.second.proxy_event = &found->second;
            owner->subscribed.push_back(&found->second);
        }
        service.owner = std::move(owner);
        Log("consuming LoLa service " + name + " of the local provider");
    }
}

void EventModule::ForwardProvidedSamples()
{
    for (auto& entry : provided_)
    {
        for (auto& element : entry.second.events)
        {
            auto& event = element.second;
            if ((event.proxy_event == nullptr) || (event.proxy_event->GetFreeSampleCount() == 0U))
            {
                continue;
            }
            const auto result = event.proxy_event->GetNewSamples(
                [this, &entry, &element](auto sample) noexcept {
                    auto lease = std::make_shared<SampleLease>(SampleLease{entry.second.owner, std::move(sample)});
                    SendNotification(
                        entry.first, entry.second, element.first, element.second, lease, element.second.subscribers);
                    if (element.second.is_field)
                    {
                        element.second.last_value = std::move(lease);
                    }
                },
                kTxMaxSamples - 1U);
            if (!result.has_value())
            {
                Log("getting LoLa samples for event " + Hex(element.first) + " failed");
            }
        }
    }
}

void EventModule::SendNotification(const ServiceInstance& key,
                                   ProvidedService& service,
                                   EventId event_id,
                                   ProvidedEvent& event,
                                   const std::shared_ptr<SampleLease>& sample,
                                   const std::set<Endpoint>& receivers)
{
    const auto* data = static_cast<const protocol::EventSample*>(sample->sample.get());
    if ((data == nullptr) || (data->size > protocol::kMaxPayloadSize))
    {
        Log("invalid LoLa sample for event " + Hex(event_id));
        return;
    }
    if (receivers.empty())
    {
        return;
    }
    event.session = (event.session == 0xFFFFU) ? 1U : static_cast<SessionId>(event.session + 1U);
    SomeipHeader header{};
    Write32(header.data(), (static_cast<WireWord>(key.first) << 16U) | event_id);
    Write32(&header[4], 8U + data->size);
    Write32(&header[8], event.session);
    header[12] = 1U;
    header[13] = service.major;
    header[14] = 2U;
    for (const auto& receiver : receivers)
    {
        //TODO: rework dynamic alloc here
        auto* context = new (std::nothrow) TxContext{sample};
        if (context == nullptr)
        {
            Log("allocating TX ownership context failed");
            continue;
        }
        pending_tx.fetch_add(1U);
        const ErrorCode error = someip_event_send(service.port,
                                                  receiver.first.data(),
                                                  receiver.second,
                                                  header.data(),
                                                  data->payload.data(),
                                                  data->size,
                                                  context);
        if (error != 0)
        {
            Log("kernel sosend event " + Hex(event_id) + " failed: " + std::to_string(error));
        }
    }
    if (event.sent++ == 0U)
    {
        Log("TX MEXTADD: first notification of " + Name(key) + " event " + Hex(event_id));
    }
}

void EventModule::Poll()
{
    std::vector<protocol::Message> commands;
    {
        std::lock_guard<std::mutex> lock{command_mutex_};
        commands.swap(commands_);
    }
    command_space_.notify_all();
    for (const auto& message : commands)
    {
        HandleCommand(message);
    }
    ConnectProvidedServices();
    ForwardProvidedSamples();
}

std::unique_ptr<EventModule> module;

}  // namespace

extern "C" ErrorCode someip_event_start(void)
{
    log_file.open("/tmp/someip_sd_event_module.log", std::ios::out | std::ios::trunc);
    if (!log_file.is_open())
    {
        return EIO;
    }
    OperationSucceeded started{false};
    std::thread initialize{[&started]() {
        com::runtime::InitializeRuntime(com::runtime::RuntimeConfiguration{score::filesystem::Path{kConfig}});
        module = std::make_unique<EventModule>();
        started = module->Start();
        if (!started)
        {
            module.reset();
        }
    }};
    initialize.join();
    return started ? 0 : EIO;
}

extern "C" void someip_event_ready(void)
{
    Log("ready: kernel socket event module; resident until io-sock exits");
}

extern "C" void someip_event_abort_start(void)
{
    module->Stop();
    module.reset();
    Log("kernel thread creation failed; control endpoint stopped");
}

extern "C" void someip_event_poll(void)
{
    module->Poll();
}

extern "C" void someip_event_datagram(TransportPort local_port,
                                      const TransportByte sender_ip[4],
                                      TransportPort sender_port,
                                      PacketHandle packet,
                                      TransportPacketSize size)
{
    const Ipv4Address ip{sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3]};
    module->OnDatagram(local_port, ip, sender_port, packet, size);
}

extern "C" void someip_event_release_tx(TxContextHandle context)
{
    delete static_cast<TxContext*>(context);
    pending_tx.fetch_sub(1U);
    if (released_tx.fetch_add(1U) == 0U)
    {
        Log("TX MEXTADD: first LoLa sample reference released");
    }
}
