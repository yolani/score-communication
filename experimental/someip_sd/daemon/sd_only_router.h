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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_ONLY_ROUTER_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_ONLY_ROUTER_H

// lg-someip includes these system headers inside its own namespace, so they must be included first.
#if defined(__QNX__)
#include <sys/select.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

#include "experimental/someip_sd/daemon/event_module_client.h"

#include "endpoint/Endpoint.h"
#include "multiplex/Multiplexer.h"
#include "packetrouter/ServiceRouter.h"
#include "runtime/ServiceManager.h"
#include "socket/Address.h"

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace score::someip_sd
{

/// ServiceRouter for lg-someip's ServiceManager that only handles SOME/IP-SD traffic.
///
/// Owns the SD multicast / unicast UDP endpoints and the multiplexer thread. The routing callbacks for
/// SOME/IP data (add_route, add_subscribe_route, ...) are forwarded to the event module, if one is set.
class SdOnlyRouter final : public lgsomeip::ServiceRouter
{
  public:
    using InternalMessageHandler = std::function<void(std::uint16_t client_id, std::vector<std::uint8_t> message)>;
    using Task = std::function<void()>;

    SdOnlyRouter() = default;
    ~SdOnlyRouter() override;

    SdOnlyRouter(const SdOnlyRouter&) = delete;
    SdOnlyRouter& operator=(const SdOnlyRouter&) = delete;
    SdOnlyRouter(SdOnlyRouter&&) = delete;
    SdOnlyRouter& operator=(SdOnlyRouter&&) = delete;

    void SetServiceManager(lgsomeip::ServiceManager* service_manager);

    /// Optional; without event module the routing callbacks are only logged.
    void SetEventModule(EventModuleClient* event_module);

    /// Receives the SD messages the ServiceManager sends to local applications (offers, subscribe acks).
    /// May be called from any thread.
    void SetInternalMessageHandler(InternalMessageHandler handler);

    /// Runs the task on the multiplexer thread. All ServiceManager entry points are serialized there,
    /// like in lg-someip's own daemon.
    void Post(Task task);

    /// Called by the SD UDP endpoints on the multiplexer thread.
    void on_message(std::shared_ptr<lgsomeip::Endpoint> endpoint, std::uint8_t* message, std::size_t message_length);

    // lgsomeip::ServiceRouter
    void init() override;
    void start() override;
    void stop() override;

    std::shared_ptr<lgsomeip::osabstraction::Multiplexer> get_multiplexer() override;
    std::shared_ptr<lgsomeip::Endpoint> get_service_discovery_multicast_endpoint() override;
    std::shared_ptr<lgsomeip::Endpoint> get_service_discovery_unicast_endpoint() override;
    std::shared_ptr<lgsomeip::osabstraction::Address> make_address() override;

    void set_instance_id(std::uint16_t service_id, std::uint16_t instance_id, std::uint16_t app_id) override;
    bool add_route(std::uint16_t service_id,
                   std::uint16_t instance_id,
                   std::uint16_t app_id,
                   std::shared_ptr<lgsomeip::osabstraction::Address> remote_tcp_address,
                   std::uint16_t local_tcp_port,
                   std::shared_ptr<lgsomeip::osabstraction::Address> remote_udp_address,
                   std::uint16_t local_udp_port,
                   std::uint8_t vlan_priority,
                   std::shared_ptr<std::vector<std::shared_ptr<lgsomeip::osabstraction::Address>>>
                       local_multicast_list) override;
    void remove_route(std::uint16_t service_id, std::uint16_t instance_id) override;
    std::int32_t find_connection(std::uint16_t service_id,
                                 std::uint16_t instance_id,
                                 std::shared_ptr<lgsomeip::osabstraction::Address> address) override;

    void add_subscribe_route(std::uint16_t service_id,
                             std::uint16_t instance_id,
                             std::uint16_t event_id,
                             std::uint16_t app_id,
                             std::shared_ptr<lgsomeip::osabstraction::Address> address,
                             std::uint16_t event_group_id) override;
    void remove_subscribe_route(std::uint16_t service_id,
                                std::uint16_t instance_id,
                                std::uint16_t event_id,
                                std::uint16_t app_id,
                                std::shared_ptr<lgsomeip::osabstraction::Address> address) override;
    void remove_subscribe_route(std::uint16_t service_id,
                                std::uint16_t instance_id,
                                std::uint16_t event_id,
                                std::uint16_t app_id) override;

    void send_internal_message(std::uint8_t* message, std::size_t message_length, std::uint16_t target_app_id) override;
    void send_external_sd_message(std::uint8_t* message,
                                  std::size_t message_length,
                                  std::shared_ptr<lgsomeip::osabstraction::Address> target_address,
                                  bool multicast) override;
    void check_and_send_magic_cookies() override;
    void reboot_route(std::shared_ptr<lgsomeip::osabstraction::Address> address) override;

#if defined(ENABLE_QNX_MESSAGE_PASSING)
    /// Used by the ServiceManager instead of its TimerMux; the handler runs on the multiplexer thread.
    void message_passing_set_timer(std::int32_t id,
                                   std::uint32_t interval_milliseconds,
                                   bool periodic,
                                   std::function<void(void)> handler) override;
    void message_passing_kill_timer(std::int32_t id) override;
#endif

  private:
    struct Timer
    {
        std::thread thread;
        std::mutex mutex;
        std::condition_variable condition;
        bool stopped{false};
    };

    void StopTimer(std::unique_ptr<Timer> timer);
    std::shared_ptr<lgsomeip::osabstraction::Address> MakeUdpAddress(const std::string& ip_address,
                                                                     std::uint16_t port,
                                                                     std::uint8_t vlan_priority);
    bool IsReboot(const std::shared_ptr<lgsomeip::Endpoint>& endpoint, bool reboot_flag, std::uint32_t request_id);
    void RunPostedTasks();

    void ForwardToEventModule(const event_module_protocol::Message& message);
    bool MakeRemoteSubscriberMessage(std::uint16_t service_id,
                                     std::uint16_t instance_id,
                                     std::uint16_t event_id,
                                     const std::shared_ptr<lgsomeip::osabstraction::Address>& address,
                                     event_module_protocol::Message& message);

    lgsomeip::ServiceManager* service_manager_{nullptr};
    EventModuleClient* event_module_{nullptr};
    std::shared_ptr<lgsomeip::osabstraction::Multiplexer> multiplexer_;
    std::shared_ptr<lgsomeip::osabstraction::Address> multicast_address_;
    std::shared_ptr<lgsomeip::Endpoint> multicast_endpoint_;
    std::shared_ptr<lgsomeip::Endpoint> unicast_endpoint_;

    // key: (sender ip, receiver ip), value: (last session id, last reboot flag)
    std::map<std::pair<std::string, std::string>, std::pair<std::uint16_t, bool>> reboot_info_;

    std::mutex handler_mutex_;
    InternalMessageHandler internal_message_handler_;

    int task_pipe_read_fd_{-1};
    int task_pipe_write_fd_{-1};
    std::mutex task_mutex_;
    std::vector<Task> tasks_;

    std::mutex timers_mutex_;
    std::map<std::int32_t, std::unique_ptr<Timer>> timers_;
};

}  // namespace score::someip_sd

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_ONLY_ROUTER_H
