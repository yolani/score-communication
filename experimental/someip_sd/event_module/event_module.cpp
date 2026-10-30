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
#include "experimental/someip_sd/event_module/event_module.h"

// Not exported by //score/message_passing (restricted visibility), available transitively.
#include "score/message_passing/i_server_connection.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace score::someip_sd
{

namespace
{

using score::message_passing::IServerConnection;
using score::message_passing::UserData;
namespace protocol = event_module_protocol;

const score::message_passing::ServiceProtocolConfig kProtocolConfig{protocol::kServiceIdentifier,
                                                                    protocol::kMaxMessageSize,
                                                                    protocol::kMaxMessageSize,
                                                                    protocol::kMaxMessageSize};
const score::message_passing::IServerFactory::ServerConfig kServerConfig{64U, 8U, 64U};

constexpr std::size_t kSomeIpHeaderSize{16U};
constexpr std::uint8_t kMessageTypeNotification{0x02U};
constexpr std::uint8_t kProtocolVersion{0x01U};
constexpr std::uint16_t kAnyEvent{0xFFFFU};
// Per provided event: up to two new samples per poll plus the held field value.
constexpr std::size_t kTxMaxSamples{3U};

std::mutex log_mutex;

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock{log_mutex};
    std::cout << "[EventModule] "
              << "someip_event_module: " << text << std::endl;
}

std::string Hex(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
    return stream.str();
}

std::string Endpoint(const std::array<std::uint8_t, 4U>& ip, std::uint16_t port)
{
    std::ostringstream stream;
    stream << static_cast<std::uint32_t>(ip[0]) << "." << static_cast<std::uint32_t>(ip[1]) << "."
           << static_cast<std::uint32_t>(ip[2]) << "." << static_cast<std::uint32_t>(ip[3]) << ":" << port;
    return stream.str();
}

std::uint32_t ReadBigEndian32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void WriteBigEndian32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

EventModule::EventModule()
{
    std::array<int, 2U> wake_pipe{-1, -1};
    if (::pipe(wake_pipe.data()) != 0)
    {
        throw std::runtime_error("EventModule: pipe creation failed");
    }
    for (const int fd : wake_pipe)
    {
        static_cast<void>(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK));
        static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
    }
    wake_read_fd_ = wake_pipe[0];
    wake_write_fd_ = wake_pipe[1];
}

EventModule::~EventModule()
{
    Stop();
    for (auto& service : provided_)
    {
        ReleaseProvidedService(service.second);
    }
    provided_.clear();
    for (auto& route : routes_)
    {
        if (route.second.skeleton && route.second.active)
        {
            route.second.skeleton->StopOfferService();
        }
    }
    for (const auto& socket : sockets_)
    {
        ::close(socket.second);
    }
    ::close(wake_read_fd_);
    ::close(wake_write_fd_);
}

bool EventModule::Start()
{
    server_ = server_factory_.Create(kProtocolConfig, kServerConfig);
    if (!server_)
    {
        return false;
    }

    auto connect_callback = [](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
        Log("SD daemon connected (pid " + std::to_string(connection.GetClientIdentity().pid) + ")");
        return UserData{std::in_place_type<std::uintptr_t>, 0U};
    };
    auto disconnect_callback = [](IServerConnection&) {
        Log("SD daemon disconnected");
    };
    auto sent_callback =
        [this](IServerConnection&,
               score::cpp::span<const std::uint8_t> data) -> score::cpp::expected_blank<score::os::Error> {
        const auto message = protocol::Decode(data.data(), data.size());
        if (!message.has_value())
        {
            Log("invalid routing command dropped");
            return {};
        }
        HandleCommand(message.value());
        return {};
    };
    auto sent_with_reply_callback =
        [](IServerConnection&, score::cpp::span<const std::uint8_t>) -> score::cpp::expected_blank<score::os::Error> {
        return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
    };

    return server_
        ->StartListening(std::move(connect_callback),
                         std::move(disconnect_callback),
                         std::move(sent_callback),
                         std::move(sent_with_reply_callback))
        .has_value();
}

void EventModule::Stop()
{
    if (server_)
    {
        server_->StopListening();
        server_.reset();
    }
}

void EventModule::HandleCommand(const protocol::Message& message)
{
    std::lock_guard<std::mutex> lock{mutex_};
    const ServiceInstance key{message.service, message.instance};
    switch (message.opcode)
    {
        case protocol::Opcode::kAddRoute:
            AddRoute(message);
            break;
        case protocol::Opcode::kRemoveRoute:
        {
            const auto provided = provided_.find(key);
            if (provided != provided_.end())
            {
                ReleaseProvidedService(provided->second);
                provided_.erase(provided);
                Log("stopped providing " + Hex(message.service) + "." + Hex(message.instance));
                break;
            }
            RemoveRoute(key);
            break;
        }
        case protocol::Opcode::kAddProvidedService:
            AddProvidedService(message);
            break;
        case protocol::Opcode::kAddRemoteSubscriber:
            AddRemoteSubscriber(message);
            break;
        case protocol::Opcode::kRemoveRemoteSubscriber:
            RemoveRemoteSubscriber(message);
            break;
        case protocol::Opcode::kAddSubscription:
        {
            const auto found = routes_.find(key);
            if (found == routes_.end())
            {
                Log("subscription for unknown route " + Hex(message.service) + "." + Hex(message.instance));
                break;
            }
            found->second.subscribed.insert(message.event);
            Log("forwarding " + Hex(message.service) + "." + Hex(message.instance) + " event " + Hex(message.event));
            break;
        }
        case protocol::Opcode::kRemoveSubscription:
        {
            const auto found = routes_.find(key);
            if (found != routes_.end())
            {
                found->second.subscribed.erase(message.event);
                Log("stopped forwarding " + Hex(message.service) + "." + Hex(message.instance) + " event " +
                    Hex(message.event));
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
    const auto name = protocol::LolaInstanceSpecifier(message.service, message.instance);
    auto& route = routes_[key];
    route.local_port = message.local_port;
    route.remote_ip = message.remote_ip;
    route.remote_port = message.remote_port;
    Log("route " + Hex(message.service) + "." + Hex(message.instance) + " from " +
        Endpoint(message.remote_ip, message.remote_port) + " to local port " + std::to_string(message.local_port));

    if (!OpenSocket(message.local_port))
    {
        return;
    }

    // A skeleton is never recreated: that would reset the shared memory under subscribed proxies.
    if (!route.skeleton)
    {
        auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            Log("invalid instance specifier " + name);
            return;
        }
        const score::mw::com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
        route.event_names.clear();
        for (const auto event : message.events)
        {
            route.event_names.push_back(protocol::LolaEventName(event));
        }
        std::vector<score::mw::com::EventInfo> events;
        for (const auto& event_name : route.event_names)
        {
            events.push_back(score::mw::com::EventInfo{event_name, meta});
        }
        score::mw::com::GenericSkeletonServiceElementInfo info;
        info.events = events;
        auto skeleton = score::mw::com::GenericSkeleton::Create(specifier.value(), info);
        if (!skeleton.has_value())
        {
            Log("creating LoLa skeleton " + name + " failed (missing in mw_com_config.json?)");
            return;
        }
        route.skeleton = std::make_unique<score::mw::com::GenericSkeleton>(std::move(skeleton).value());
        for (const auto event : message.events)
        {
            const auto found = route.skeleton->GetEvents().find(protocol::LolaEventName(event));
            if (found != route.skeleton->GetEvents().cend())
            {
                route.events[event] = &found->second;
            }
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
        Log("offered LoLa service " + name + " with " + std::to_string(route.events.size()) + " event(s)");
    }
}

void EventModule::RemoveRoute(const ServiceInstance& key)
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
        Log("stopped offering LoLa service " + protocol::LolaInstanceSpecifier(key.first, key.second));
    }
}

bool EventModule::OpenSocket(std::uint16_t port)
{
    if (sockets_.count(port) != 0U)
    {
        return true;
    }
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        Log("socket() failed");
        return false;
    }
    const int enable{1};
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        Log("bind() to UDP port " + std::to_string(port) + " failed");
        ::close(socket_fd);
        return false;
    }
    sockets_[port] = socket_fd;
    Log("Successfully opened socket on UDP port " + std::to_string(port));
    return true;
}

void EventModule::Run(const std::atomic<bool>& running)
{
    // TODO: Run() is handling rX & Tx in 1 thread sequentially, check if this is the correct approach
    // TODO: do not use local buffer, use lola memory in mbuf in kernel module
    // TODO: how to get size, someip_tp, etc.
    std::array<std::uint8_t, 65536U> buffer{};
    std::vector<pollfd> poll_fds;
    std::vector<std::uint16_t> ports;
    while (running.load())
    {
        poll_fds.clear();
        ports.clear();
        poll_fds.push_back(pollfd{wake_read_fd_, POLLIN, 0});
        ports.push_back(0U);
        {
            // lock against service discovery creating additional sockets
            // should only cause contention during init phase while service discovery is setting up sockets
            std::lock_guard<std::mutex> lock{mutex_};
            for (const auto& socket : sockets_)
            {
                poll_fds.push_back(pollfd{socket.second, POLLIN, 0});
                ports.push_back(socket.first);
            }
        }
        if (::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), 200) > 0)
        {
            if ((poll_fds[0].revents & POLLIN) != 0)
            {
                while (::read(wake_read_fd_, buffer.data(), buffer.size()) > 0)
                {
                }
            }
            for (std::size_t index = 1U; index < poll_fds.size(); ++index)
            {
                if ((poll_fds[index].revents & POLLIN) == 0)
                {
                    continue;
                }
                sockaddr_in sender{};
                socklen_t sender_length{sizeof(sender)};
                const auto received = ::recvfrom(poll_fds[index].fd,
                                                 buffer.data(),
                                                 buffer.size(),
                                                 0,
                                                 reinterpret_cast<sockaddr*>(&sender),
                                                 &sender_length);
                if (received <= 0)
                {
                    continue;
                }
                std::array<std::uint8_t, 4U> sender_ip{};

                std::memcpy(sender_ip.data(), &sender.sin_addr.s_addr, sender_ip.size());
                OnDatagram(
                    ports[index], sender_ip, ntohs(sender.sin_port), buffer.data(), static_cast<std::size_t>(received));
            }
        }

        std::lock_guard<std::mutex> lock{mutex_};
        ConnectProvidedServices();
        ForwardProvidedSamples();
    }
}

void EventModule::OnDatagram(std::uint16_t local_port,
                             const std::array<std::uint8_t, 4U>& sender_ip,
                             std::uint16_t sender_port,
                             const std::uint8_t* data,
                             std::size_t size)
{
    // TODO: lock should be removed if possible, conflicts with service discovery
    std::lock_guard<std::mutex> lock{mutex_};
    // A datagram may carry several SOME/IP messages.
    while (size >= kSomeIpHeaderSize)
    {
        const auto message_id = ReadBigEndian32(&data[0]);
        const auto length = static_cast<std::size_t>(ReadBigEndian32(&data[4]));
        if ((length < 8U) || ((length + 8U) > size))
        {
            return;
        }
        const auto message_size = length + 8U;
        const auto service = static_cast<std::uint16_t>(message_id >> 16U);
        const auto event = static_cast<std::uint16_t>(message_id & 0xFFFFU);
        const auto session = static_cast<std::uint16_t>(ReadBigEndian32(&data[8]) & 0xFFFFU);
        const auto interface_version = data[13];
        const auto message_type = data[14];
        const std::uint8_t* const payload = &data[kSomeIpHeaderSize];
        const auto payload_size = message_size - kSomeIpHeaderSize;
        data += message_size;
        size -= message_size;

        if ((message_type != kMessageTypeNotification) || (payload_size > protocol::kMaxPayloadSize))
        {
            continue;
        }
        // SOME/IP carries no instance id; the instance is identified by local port and remote endpoint.
        for (auto& entry : routes_)
        {
            auto& route = entry.second;
            if ((entry.first.first != service) || (!route.active) || (route.local_port != local_port) ||
                (route.remote_ip != sender_ip) || (route.remote_port != sender_port))
            {
                continue;
            }
            if ((route.subscribed.count(event) == 0U) && (route.subscribed.count(kAnyEvent) == 0U))
            {
                continue;
            }
            const auto lola_event = route.events.find(event);
            if (lola_event == route.events.end())
            {
                continue;
            }
            auto sample = lola_event->second->Allocate();
            if (!sample.has_value())
            {
                Log("no free LoLa sample slot for event " + Hex(event));
                continue;
            }
            auto* const event_sample = static_cast<protocol::EventSample*>(sample.value().Get());
            event_sample->size = static_cast<std::uint32_t>(payload_size);
            event_sample->session = session;
            event_sample->interface_version = interface_version;
            event_sample->message_type = message_type;
            std::memcpy(event_sample->payload.data(), payload, payload_size);
            if (!lola_event->second->Send(std::move(sample).value()).has_value())
            {
                Log("sending LoLa sample for event " + Hex(event) + " failed");
                continue;
            }
            if (forwarded_[{service, event}]++ == 0U)
            {
                Log("forwarded first notification of " + Hex(service) + "." + Hex(entry.first.second) + " event " +
                    Hex(event) + " to LoLa");
            }
        }
    }
}

void EventModule::AddProvidedService(const protocol::Message& message)
{
    if (!OpenSocket(message.local_port))
    {
        return;
    }
    auto& service = provided_[ServiceInstance{message.service, message.instance}];
    service.local_port = message.local_port;
    service.major = message.major;
    for (const auto event : message.events)
    {
        service.events[event].is_field =
            std::find(message.fields.begin(), message.fields.end(), event) != message.fields.end();
    }
    Log("providing " + Hex(message.service) + "." + Hex(message.instance) + " with " +
        std::to_string(service.events.size()) + " event(s) from local port " + std::to_string(message.local_port));
}

void EventModule::AddRemoteSubscriber(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    const auto service = provided_.find(key);
    if (service == provided_.end())
    {
        Log("remote subscriber for unknown provided service " + Hex(message.service) + "." + Hex(message.instance));
        return;
    }
    const auto event = service->second.events.find(message.event);
    if (event == service->second.events.end())
    {
        Log("remote subscriber for unknown event " + Hex(message.event));
        return;
    }
    const RemoteEndpoint endpoint{message.remote_ip, message.remote_port};
    if (!event->second.subscribers.insert(endpoint).second)
    {
        return;
    }  // else?
    Log("remote subscriber " + Endpoint(message.remote_ip, message.remote_port) + " for " + Hex(message.service) + "." +
        Hex(message.instance) + " event " + Hex(message.event));
    if (event->second.is_field && event->second.last_value.has_value())
    {
        SendNotification(key, service->second, message.event, event->second, *event->second.last_value, {endpoint});
        Log("sent initial field value of event " + Hex(message.event) + " to " +
            Endpoint(message.remote_ip, message.remote_port));
    }
}

void EventModule::RemoveRemoteSubscriber(const protocol::Message& message)
{
    const auto service = provided_.find(ServiceInstance{message.service, message.instance});
    if (service == provided_.end())
    {
        return;
    }
    const auto event = service->second.events.find(message.event);
    if ((event != service->second.events.end()) &&
        (event->second.subscribers.erase(RemoteEndpoint{message.remote_ip, message.remote_port}) != 0U))
    {
        Log("removed remote subscriber " + Endpoint(message.remote_ip, message.remote_port) + " for " +
            Hex(message.service) + "." + Hex(message.instance) + " event " + Hex(message.event));
    }
}

void EventModule::ReleaseProvidedService(ProvidedService& service)
{
    // LoLa terminates if a proxy event is destroyed while samples are held or while still subscribed.
    for (auto& event : service.events)
    {
        event.second.last_value.reset();
        if (event.second.proxy_event != nullptr)
        {
            event.second.proxy_event->Unsubscribe();
            event.second.proxy_event = nullptr;
        }
    }
    service.proxy.reset();
}

void EventModule::ConnectProvidedServices()
{
    for (auto& entry : provided_)
    {
        auto& service = entry.second;
        if (service.proxy)
        {
            continue;
        }
        const auto name = protocol::LolaInstanceSpecifier(entry.first.first, entry.first.second);
        auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            continue;
        }
        // TODO: use asynchronous service discovery instead of blocking FindService()
        // would remove this loop entirely??
        auto handles = score::mw::com::GenericProxy::FindService(specifier.value());
        if ((!handles.has_value()) || handles.value().empty())
        {
            continue;
        }
        auto proxy = score::mw::com::GenericProxy::Create(handles.value()[0]);
        if (!proxy.has_value())
        {
            Log("creating LoLa proxy " + name + " failed");
            continue;
        }
        service.proxy = std::make_unique<score::mw::com::GenericProxy>(std::move(proxy).value());
        for (auto& event : service.events)
        {
            const auto found = service.proxy->GetEvents().find(protocol::LolaEventName(event.first));
            if (found == service.proxy->GetEvents().cend())
            {
                Log("LoLa service " + name + " has no event " + protocol::LolaEventName(event.first));
                continue;
            }
            if (!found->second.Subscribe(kTxMaxSamples).has_value())
            {
                Log("subscribing LoLa event " + protocol::LolaEventName(event.first) + " failed");
                continue;
            }
            // TODO: SetReceiveHandler() or Polling()
            static_cast<void>(found->second.SetReceiveHandler([this]() {
                Wake();
            }));
            event.second.proxy_event = &found->second;
        }
        Log("consuming LoLa service " + name + " of the local provider");
    }
}

void EventModule::ForwardProvidedSamples()
{
    for (auto& entry : provided_)
    {
        for (auto& event : entry.second.events)
        {
            if (event.second.proxy_event == nullptr)
            {
                continue;
            }
            static_cast<void>(event.second.proxy_event->GetNewSamples(
                [this, &entry, &event](auto sample) noexcept {
                    SendNotification(
                        entry.first, entry.second, event.first, event.second, sample, event.second.subscribers);
                    if (event.second.is_field)
                    {
                        event.second.last_value = std::move(sample);
                    }
                },
                kTxMaxSamples - 1U));
        }
    }
}

void EventModule::SendNotification(const ServiceInstance& key,
                                   ProvidedService& service,
                                   std::uint16_t event_id,
                                   ProvidedEvent& event,
                                   const score::mw::com::SamplePtr<void>& sample,
                                   const std::set<RemoteEndpoint>& receivers)
{
    const auto* const data = static_cast<const protocol::EventSample*>(sample.get());
    const auto socket = sockets_.find(service.local_port);
    if ((data == nullptr) || (data->size > protocol::kMaxPayloadSize) || receivers.empty() ||
        (socket == sockets_.end()))
    {
        return;
    }
    event.session = (event.session == 0xFFFFU) ? 1U : static_cast<std::uint16_t>(event.session + 1U);

    std::array<std::uint8_t, kSomeIpHeaderSize> header{};
    WriteBigEndian32(&header[0], (static_cast<std::uint32_t>(key.first) << 16U) | event_id);
    WriteBigEndian32(&header[4], static_cast<std::uint32_t>(8U + data->size));
    WriteBigEndian32(&header[8], event.session);
    header[12] = kProtocolVersion;
    header[13] = service.major;
    header[14] = kMessageTypeNotification;
    header[15] = 0U;

    // The payload is sent from the LoLa slot; only the header is built here.
    std::array<iovec, 2U> parts{};
    parts[0].iov_base = header.data();
    parts[0].iov_len = header.size();
    parts[1].iov_base = const_cast<std::uint8_t*>(data->payload.data());
    parts[1].iov_len = data->size;
    for (const auto& receiver : receivers)
    {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(receiver.second);
        std::memcpy(&destination.sin_addr.s_addr, receiver.first.data(), receiver.first.size());
        msghdr message{};
        message.msg_name = &destination;
        message.msg_namelen = sizeof(destination);
        message.msg_iov = parts.data();
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(parts.size());
        if (::sendmsg(socket->second, &message, 0) < 0)
        {
            Log("sending notification to " + Endpoint(receiver.first, receiver.second) + " failed");
        }
    }
    if (event.sent++ == 0U)
    {
        Log("sent first notification of " + Hex(key.first) + "." + Hex(key.second) + " event " + Hex(event_id));
    }
}

void EventModule::Wake()
{
    const std::uint8_t wakeup{1U};
    // A full pipe already guarantees a pending wakeup.
    static_cast<void>(::write(wake_write_fd_, &wakeup, sizeof(wakeup)));
}

}  // namespace score::someip_sd
