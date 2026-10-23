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
#include "experimental/someip_sd/daemon/sd_only_router.h"

#include "config/Configuration.h"
#include "config/ServiceInfo.h"
#include "message/MessageBuilder.h"
#include "message/MessageSD.h"
#include "socket/IP4Address.h"
#include "socket/IP6Address.h"
#include "socket/NetworkDevice.h"
#include "socket/UDPSocket.h"
#include "utils/log/logger.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <exception>
#include <sstream>
#include <stdexcept>

namespace score::someip_sd
{

namespace
{

std::string ToString(const std::shared_ptr<lgsomeip::osabstraction::Address>& address)
{
    return (address != nullptr) ? address->to_string() : std::string{"-"};
}

/// Returns false for anything but a dotted IPv4 address; IPv6 is not supported by the PoC event module.
bool ParseIpv4(const std::string& text, std::array<std::uint8_t, 4U>& bytes)
{
    std::istringstream stream{text};
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        unsigned int value{0U};
        char separator{'.'};
        if ((index > 0U) && (!(stream >> separator) || (separator != '.')))
        {
            return false;
        }
        if (!(stream >> value) || (value > 255U))
        {
            return false;
        }
        bytes[index] = static_cast<std::uint8_t>(value);
    }
    return stream.peek() == std::char_traits<char>::eof();
}

}  // namespace

SdOnlyRouter::~SdOnlyRouter()
{
    std::map<std::int32_t, std::unique_ptr<Timer>> timers;
    {
        std::lock_guard<std::mutex> lock{timers_mutex_};
        timers.swap(timers_);
    }
    for (auto& timer : timers)
    {
        StopTimer(std::move(timer.second));
    }
    for (const int fd : {task_pipe_read_fd_, task_pipe_write_fd_})
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

void SdOnlyRouter::SetServiceManager(lgsomeip::ServiceManager* service_manager)
{
    service_manager_ = service_manager;
}

void SdOnlyRouter::SetEventModule(EventModuleClient* event_module)
{
    event_module_ = event_module;
}

void SdOnlyRouter::ForwardToEventModule(const event_module_protocol::Message& message)
{
    if ((event_module_ != nullptr) && (!event_module_->Send(message)))
    {
        LGSOMEIP_LOG_WARN << "SdOnlyRouter: event module not reachable, routing command 0x" << std::hex
                          << static_cast<std::uint32_t>(message.opcode) << " dropped" << std::dec;
    }
}

bool SdOnlyRouter::MakeRemoteSubscriberMessage(std::uint16_t service_id,
                                               std::uint16_t instance_id,
                                               std::uint16_t event_id,
                                               const std::shared_ptr<lgsomeip::osabstraction::Address>& address,
                                               event_module_protocol::Message& message)
{
    if (address->get_reliable() || (!ParseIpv4(address->get_ip_address(), message.remote_ip)))
    {
        LGSOMEIP_LOG_WARN << "SdOnlyRouter: remote subscriber " << ToString(address)
                          << " is not IPv4 UDP, not forwarded to the event module";
        return false;
    }
    message.service = service_id;
    message.instance = instance_id;
    message.event = event_id;
    message.remote_port = address->get_port_address();
    return true;
}

void SdOnlyRouter::SetInternalMessageHandler(InternalMessageHandler handler)
{
    std::lock_guard<std::mutex> lock{handler_mutex_};
    internal_message_handler_ = std::move(handler);
}

void SdOnlyRouter::Post(Task task)
{
    {
        std::lock_guard<std::mutex> lock{task_mutex_};
        tasks_.push_back(std::move(task));
    }
    if (task_pipe_write_fd_ >= 0)
    {
        // A full pipe already guarantees a pending wakeup, so EAGAIN is fine.
        const std::uint8_t wakeup{1U};
        static_cast<void>(::write(task_pipe_write_fd_, &wakeup, sizeof(wakeup)));
    }
}

void SdOnlyRouter::RunPostedTasks()
{
    std::array<std::uint8_t, 64U> drain{};
    while (::read(task_pipe_read_fd_, drain.data(), drain.size()) > 0)
    {
    }

    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock{task_mutex_};
        tasks.swap(tasks_);
    }
    for (auto& task : tasks)
    {
        try
        {
            task();
        }
        catch (const std::exception& exception)
        {
            LGSOMEIP_LOG_ERROR << "SdOnlyRouter: posted task failed: " << exception.what();
        }
    }
}

void SdOnlyRouter::init()
{
    if (service_manager_ == nullptr)
    {
        throw std::logic_error("SdOnlyRouter: ServiceManager not set");
    }
    auto configuration = service_manager_->get_configuration();

    auto& network_device = lgsomeip::osabstraction::NetworkDevice::instance();
    network_device.initialize(configuration->get_address(), configuration->get_ip_type());
    if (!network_device.is_initialized())
    {
        throw std::runtime_error("SdOnlyRouter: no network device with address " + configuration->get_address());
    }

    multiplexer_ = std::make_shared<lgsomeip::osabstraction::Multiplexer>();
    std::array<int, 2U> task_pipe{-1, -1};
    if (::pipe(task_pipe.data()) != 0)
    {
        throw std::runtime_error("SdOnlyRouter: pipe creation failed");
    }
    task_pipe_read_fd_ = task_pipe[0];
    task_pipe_write_fd_ = task_pipe[1];
    for (const int fd : task_pipe)
    {
        static_cast<void>(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK));
        static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
    }
    multiplexer_->set(task_pipe_read_fd_, [this](bool) {
        RunPostedTasks();
    });

    auto* sd_config = configuration->get_service_discovery_info();

    multicast_address_ = MakeUdpAddress(sd_config->get_multicast(), sd_config->get_port(), sd_config->get_vlan_priority());
    auto multicast_socket = std::make_shared<lgsomeip::osabstraction::UDPSocket>(multicast_address_);
    if (multicast_socket->bind() == -1)
    {
        throw std::runtime_error("SdOnlyRouter: binding SD multicast socket failed");
    }
    if (!multicast_socket->join_multicast(sd_config->get_multicast()))
    {
        throw std::runtime_error("SdOnlyRouter: joining SD multicast group failed");
    }
    multicast_endpoint_ = std::make_shared<lgsomeip::EndpointUDP<SdOnlyRouter>>(this);
    multicast_endpoint_->set_socket(multicast_socket);

    auto unicast_address =
        MakeUdpAddress(configuration->get_address(), sd_config->get_port(), sd_config->get_vlan_priority());
    auto unicast_socket = std::make_shared<lgsomeip::osabstraction::UDPSocket>(unicast_address);
    if (unicast_socket->bind() == -1)
    {
        throw std::runtime_error("SdOnlyRouter: binding SD unicast socket failed");
    }
    unicast_socket->set_multicast_loop(0U);
    unicast_endpoint_ = std::make_shared<lgsomeip::EndpointUDP<SdOnlyRouter>>(this);
    unicast_endpoint_->set_socket(unicast_socket);

    LGSOMEIP_LOG_INFO << "SdOnlyRouter: SD on " << ToString(unicast_address) << ", multicast "
                      << ToString(multicast_address_);
}

void SdOnlyRouter::start()
{
    multiplexer_->start();
    multicast_endpoint_->start_listen(multiplexer_);
    unicast_endpoint_->start_listen(multiplexer_);
}

void SdOnlyRouter::stop()
{
    if (multiplexer_ == nullptr)
    {
        return;
    }
    multicast_endpoint_->stop_listen();
    unicast_endpoint_->stop_listen();
    multiplexer_->unset(task_pipe_read_fd_);
    multiplexer_->stop();
    multiplexer_->join();
}

void SdOnlyRouter::StopTimer(std::unique_ptr<Timer> timer)
{
    if (timer == nullptr)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock{timer->mutex};
        timer->stopped = true;
    }
    timer->condition.notify_all();
    if (timer->thread.joinable())
    {
        timer->thread.join();
    }
}

#if defined(ENABLE_QNX_MESSAGE_PASSING)
void SdOnlyRouter::message_passing_set_timer(std::int32_t id,
                                             std::uint32_t interval_milliseconds,
                                             bool periodic,
                                             std::function<void(void)> handler)
{
    message_passing_kill_timer(id);

    auto timer = std::make_unique<Timer>();
    Timer* const raw_timer = timer.get();
    timer->thread = std::thread{[this, raw_timer, interval_milliseconds, periodic, handler]() {
        const std::chrono::milliseconds interval{interval_milliseconds};
        auto next = std::chrono::steady_clock::now();
        do
        {
            next += interval;
            std::unique_lock<std::mutex> lock{raw_timer->mutex};
            if (raw_timer->condition.wait_until(lock, next, [raw_timer]() {
                    return raw_timer->stopped;
                }))
            {
                return;
            }
            lock.unlock();
            Post(handler);
        } while (periodic);
    }};

    std::lock_guard<std::mutex> lock{timers_mutex_};
    timers_[id] = std::move(timer);
}

void SdOnlyRouter::message_passing_kill_timer(std::int32_t id)
{
    std::unique_ptr<Timer> timer;
    {
        std::lock_guard<std::mutex> lock{timers_mutex_};
        const auto it = timers_.find(id);
        if (it == timers_.end())
        {
            return;
        }
        timer = std::move(it->second);
        timers_.erase(it);
    }
    StopTimer(std::move(timer));
}
#endif

void SdOnlyRouter::on_message(std::shared_ptr<lgsomeip::Endpoint> endpoint,
                              std::uint8_t* message,
                              std::size_t message_length)
{
    const auto length = static_cast<std::uint32_t>(message_length);
    if (!lgsomeip::MessageBuilder::is_sd_message(message, length))
    {
        return;
    }
    auto sd_message = lgsomeip::MessageBuilder::create<lgsomeip::SOMEIPSD>();
    if (!lgsomeip::MessageBuilder::build_message(*sd_message, message, length))
    {
        LGSOMEIP_LOG_WARN << "SdOnlyRouter: malformed SD message dropped";
        return;
    }

    const auto sender = endpoint->get_sender_address();
    if (sender == nullptr)
    {
        return;
    }
    const bool reboot = IsReboot(endpoint, sd_message->get_reboot_flag(), sd_message->get_request_id());

    auto configuration = service_manager_->get_configuration();
    if ((sender->get_port_address() != configuration->get_service_discovery_info()->get_port()) ||
        (sender->get_ip_address() == configuration->get_address()))
    {
        return;
    }

    try
    {
        service_manager_->on_external_message(endpoint, sd_message, reboot);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdOnlyRouter: SD message from " << ToString(sender) << " failed: " << exception.what();
    }
}

bool SdOnlyRouter::IsReboot(const std::shared_ptr<lgsomeip::Endpoint>& endpoint,
                            bool reboot_flag,
                            std::uint32_t request_id)
{
    // Same detection as lg-someip's PacketRouterHost::is_reboot().
    const std::string sender_ip = endpoint->get_sender_address()->get_ip_address();
    const std::string receiver_ip = endpoint->get_socket()->get_src_address()->get_ip_address();
    const auto session = static_cast<std::uint16_t>(request_id & 0xFFFFU);

    bool reboot_detected{false};
    const auto key = std::make_pair(sender_ip, receiver_ip);
    const auto found = reboot_info_.find(key);
    if (found != reboot_info_.end())
    {
        const auto previous_session = found->second.first;
        const bool previous_reboot_flag = found->second.second;
        reboot_detected = ((!previous_reboot_flag) && reboot_flag) ||
                          (previous_reboot_flag && reboot_flag && (previous_session >= session));
    }

    if (reboot_detected)
    {
        LGSOMEIP_LOG_INFO << "SdOnlyRouter: reboot of " << sender_ip << " detected";
        auto peer = reboot_info_.lower_bound(std::make_pair(sender_ip, std::string{}));
        while ((peer != reboot_info_.end()) && (peer->first.first == sender_ip))
        {
            peer = reboot_info_.erase(peer);
        }
    }
    reboot_info_[key] = std::make_pair(session, reboot_flag);
    return reboot_detected;
}

std::shared_ptr<lgsomeip::osabstraction::Multiplexer> SdOnlyRouter::get_multiplexer()
{
    return multiplexer_;
}

std::shared_ptr<lgsomeip::Endpoint> SdOnlyRouter::get_service_discovery_multicast_endpoint()
{
    return multicast_endpoint_;
}

std::shared_ptr<lgsomeip::Endpoint> SdOnlyRouter::get_service_discovery_unicast_endpoint()
{
    return unicast_endpoint_;
}

std::shared_ptr<lgsomeip::osabstraction::Address> SdOnlyRouter::make_address()
{
    if (service_manager_->get_configuration()->get_ip_type() == 6U)
    {
        return std::make_shared<lgsomeip::osabstraction::IP6Address>();
    }
    return std::make_shared<lgsomeip::osabstraction::IP4Address>();
}

std::shared_ptr<lgsomeip::osabstraction::Address> SdOnlyRouter::MakeUdpAddress(const std::string& ip_address,
                                                                             std::uint16_t port,
                                                                             std::uint8_t vlan_priority)
{
    auto address = make_address();
    address->set_reliable(false);
    address->set_port_address(port);
    address->set_vlan_priority(vlan_priority);
    address->set_ip_address(ip_address);
    return address;
}

void SdOnlyRouter::set_instance_id(std::uint16_t, std::uint16_t, std::uint16_t) {}

bool SdOnlyRouter::add_route(std::uint16_t service_id,
                             std::uint16_t instance_id,
                             std::uint16_t app_id,
                             std::shared_ptr<lgsomeip::osabstraction::Address> remote_tcp_address,
                             std::uint16_t local_tcp_port,
                             std::shared_ptr<lgsomeip::osabstraction::Address> remote_udp_address,
                             std::uint16_t local_udp_port,
                             std::uint8_t,
                             std::shared_ptr<std::vector<std::shared_ptr<lgsomeip::osabstraction::Address>>>)
{
    LGSOMEIP_LOG_INFO << "SdOnlyRouter: add_route " << std::hex << service_id << "." << instance_id
                      << " app=" << app_id << std::dec << " remote tcp=" << ToString(remote_tcp_address)
                      << " udp=" << ToString(remote_udp_address) << " local tcp port=" << local_tcp_port
                      << " udp port=" << local_udp_port;

    event_module_protocol::Message message{};
    message.opcode = event_module_protocol::Opcode::kAddRoute;
    message.service = service_id;
    message.instance = instance_id;
    message.local_port = local_udp_port;
    auto* service_info = service_manager_->get_configuration()->get_service_info(service_id, instance_id);
    if (service_info != nullptr)
    {
        for (const auto& event : *service_info->get_events())
        {
            message.events.push_back(event.first);
        }
    }

    // A local application offers the service; the remote addresses are our own configured endpoints then.
    if (app_id != 0U)
    {
        if ((service_info == nullptr) || (local_udp_port == 0U))
        {
            LGSOMEIP_LOG_WARN << "SdOnlyRouter: provided service without configured UDP port, not forwarded";
            return true;
        }
        message.opcode = event_module_protocol::Opcode::kAddProvidedService;
        message.major = service_info->get_major_version();
        for (const auto event : message.events)
        {
            const auto* const event_config = service_info->get_event(event);
            if ((event_config != nullptr) && event_config->is_field())
            {
                message.fields.push_back(event);
            }
        }
        ForwardToEventModule(message);
        return true;
    }

    if ((remote_udp_address == nullptr) || (!ParseIpv4(remote_udp_address->get_ip_address(), message.remote_ip)))
    {
        LGSOMEIP_LOG_WARN << "SdOnlyRouter: no IPv4 UDP endpoint, route not forwarded to the event module";
        return true; // return false?
    }
    message.remote_port = remote_udp_address->get_port_address();
    // default case: service offered on someip
    ForwardToEventModule(message);
    return true;
}

void SdOnlyRouter::remove_route(std::uint16_t service_id, std::uint16_t instance_id)
{
    LGSOMEIP_LOG_INFO << "SdOnlyRouter: remove_route " << std::hex << service_id << "." << instance_id;

    event_module_protocol::Message message{};
    message.opcode = event_module_protocol::Opcode::kRemoveRoute;
    message.service = service_id;
    message.instance = instance_id;
    ForwardToEventModule(message);
}

std::int32_t SdOnlyRouter::find_connection(std::uint16_t,
                                           std::uint16_t,
                                           std::shared_ptr<lgsomeip::osabstraction::Address>)
{
    // TCP connections are owned by the event module (not supported yet); report them as present.
    return 1;
}

void SdOnlyRouter::add_subscribe_route(std::uint16_t service_id,
                                       std::uint16_t instance_id,
                                       std::uint16_t event_id,
                                       std::uint16_t app_id,
                                       std::shared_ptr<lgsomeip::osabstraction::Address> address,
                                       std::uint16_t event_group_id)
{
    LGSOMEIP_LOG_INFO << "SdOnlyRouter: add_subscribe_route " << std::hex << service_id << "." << instance_id
                      << " eventgroup=" << event_group_id << " event=" << event_id << " app=" << app_id << std::dec
                      << " address=" << ToString(address);

    // Local client subscribed a remote service; multicast eventgroups (app_id and address set) are not supported yet.
    if ((app_id != 0U) && (address == nullptr))
    {
        event_module_protocol::Message message{};
        message.opcode = event_module_protocol::Opcode::kAddSubscription; // TODO: rename to kAddLocalSubscription?
        message.service = service_id;
        message.instance = instance_id;
        message.event = event_id;
        ForwardToEventModule(message);
        return;
    }
    // Remote client subscribed a provided service; called again on every renewal.
    if ((app_id == 0U) && (address != nullptr))
    {
        event_module_protocol::Message message{};
        message.opcode = event_module_protocol::Opcode::kAddRemoteSubscriber;
        if (MakeRemoteSubscriberMessage(service_id, instance_id, event_id, address, message))
        {
            ForwardToEventModule(message);
        }
    }
}

void SdOnlyRouter::remove_subscribe_route(std::uint16_t service_id,
                                          std::uint16_t instance_id,
                                          std::uint16_t event_id,
                                          std::uint16_t app_id,
                                          std::shared_ptr<lgsomeip::osabstraction::Address> address)
{
    LGSOMEIP_LOG_INFO << "SdOnlyRouter: remove_subscribe_route " << std::hex << service_id << "." << instance_id
                      << " event=" << event_id << " app=" << app_id << std::dec << " address=" << ToString(address);

    if (app_id == 0U)
    {
        event_module_protocol::Message message{};
        message.opcode = event_module_protocol::Opcode::kRemoveRemoteSubscriber;
        if ((address != nullptr) && MakeRemoteSubscriberMessage(service_id, instance_id, event_id, address, message))
        {
            ForwardToEventModule(message);
        }
        return;
    }
    event_module_protocol::Message message{};
    message.opcode = event_module_protocol::Opcode::kRemoveSubscription;
    message.service = service_id;
    message.instance = instance_id;
    message.event = event_id;
    ForwardToEventModule(message);
}

void SdOnlyRouter::remove_subscribe_route(std::uint16_t service_id,
                                          std::uint16_t instance_id,
                                          std::uint16_t event_id,
                                          std::uint16_t app_id)
{
    remove_subscribe_route(service_id, instance_id, event_id, app_id, nullptr);
}

void SdOnlyRouter::send_internal_message(std::uint8_t* message, std::size_t message_length, std::uint16_t target_app_id)
{
    InternalMessageHandler handler;
    {
        std::lock_guard<std::mutex> lock{handler_mutex_};
        handler = internal_message_handler_;
    }
    if (handler)
    {
        handler(target_app_id, std::vector<std::uint8_t>(message, message + message_length));
    }
}

void SdOnlyRouter::send_external_sd_message(std::uint8_t* message,
                                            std::size_t message_length,
                                            std::shared_ptr<lgsomeip::osabstraction::Address> target_address,
                                            bool)
{
    if (unicast_endpoint_ == nullptr)
    {
        return;
    }
    unicast_endpoint_->send_message(
        message, message_length, (target_address != nullptr) ? target_address : multicast_address_);
}

void SdOnlyRouter::check_and_send_magic_cookies() {}

void SdOnlyRouter::reboot_route(std::shared_ptr<lgsomeip::osabstraction::Address> address)
{
    LGSOMEIP_LOG_INFO << "SdOnlyRouter: reboot_route " << ToString(address);
}

}  // namespace score::someip_sd
