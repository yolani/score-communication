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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_FACADE_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_FACADE_H

#include "experimental/someip_sd/daemon/sd_only_router.h"
#include "experimental/someip_sd/protocol/sd_protocol.h"

#include "score/message_passing/server_factory.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace score::someip_sd
{

/// Bridges the opcode protocol (score::message_passing) to lg-someip's ServiceManager.
///
/// Every message_passing connection gets its own client id, which is used as lg-someip application id
/// in the internal SD messages. Client state is only touched on the router's multiplexer thread.
class SdFacade final
{
  public:
    SdFacade(lgsomeip::ServiceManager& service_manager, SdOnlyRouter& router);
    ~SdFacade();

    SdFacade(const SdFacade&) = delete;
    SdFacade& operator=(const SdFacade&) = delete;
    SdFacade(SdFacade&&) = delete;
    SdFacade& operator=(SdFacade&&) = delete;

    bool Start();
    void Stop();

  private:
    using ServiceKey = std::pair<std::uint16_t, std::uint16_t>;

    struct Subscription
    {
        std::uint16_t service;
        std::uint16_t instance;
        std::uint16_t eventgroup;
        std::uint8_t major;
        bool sent;
    };

    struct ClientState
    {
        std::set<ServiceKey> available;
        std::vector<Subscription> subscriptions;
        /// Offered service instances with their (major, minor) version, needed again for StopOffer.
        std::map<ServiceKey, std::pair<std::uint8_t, std::uint32_t>> offered;
    };

    // multiplexer thread
    void HandleClientMessage(std::uint16_t client_id, const protocol::Message& message);
    void HandleServiceManagerMessage(std::uint16_t client_id, std::vector<std::uint8_t> bytes);
    void HandleClientDisconnect(std::uint16_t client_id);
    void SendFindService(std::uint16_t client_id, const protocol::Message& message, std::uint32_t ttl);
    void SendSubscribe(std::uint16_t client_id, const Subscription& subscription, std::uint32_t ttl);
    void SendOfferService(std::uint16_t client_id,
                          const ServiceKey& key,
                          std::uint8_t major,
                          std::uint32_t minor,
                          std::uint32_t ttl);
    void SendSubscribeAck(std::uint16_t client_id,
                          const ServiceKey& key,
                          std::uint16_t eventgroup,
                          std::uint8_t major,
                          std::uint32_t ttl);

    // any thread
    void Notify(std::uint16_t client_id, const protocol::Message& message);

    lgsomeip::ServiceManager& service_manager_;
    SdOnlyRouter& router_;

    score::message_passing::ServerFactory server_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IServer> server_;

    std::atomic<std::uint16_t> next_client_id_{0x0101U};
    std::mutex connections_mutex_;
    std::map<std::uint16_t, score::message_passing::IServerConnection*> connections_;

    std::map<std::uint16_t, ClientState> clients_;
};

}  // namespace score::someip_sd

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_SD_FACADE_H
