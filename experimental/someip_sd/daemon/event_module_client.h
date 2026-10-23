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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_EVENT_MODULE_CLIENT_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_EVENT_MODULE_CLIENT_H

#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/message_passing/client_factory.h"

#include <atomic>
#include <chrono>

namespace score::someip_sd
{

/// SD daemon side of the routing interface towards the event module.
/// No resync yet: if the event module restarts, the SD daemon has to be restarted as well.
class EventModuleClient final
{
  public:
    EventModuleClient() = default;
    ~EventModuleClient();

    EventModuleClient(const EventModuleClient&) = delete;
    EventModuleClient& operator=(const EventModuleClient&) = delete;
    EventModuleClient(EventModuleClient&&) = delete;
    EventModuleClient& operator=(EventModuleClient&&) = delete;

    /// Returns false if the event module is not reachable within the timeout.
    bool Connect(std::chrono::milliseconds timeout);

    /// Thread-safe; drops the message if the event module is not connected.
    bool Send(const event_module_protocol::Message& message);

  private:
    score::message_passing::ClientFactory client_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IClientConnection> connection_;
    std::atomic<score::message_passing::IClientConnection::State> state_{
        score::message_passing::IClientConnection::State::kStarting};
};

}  // namespace score::someip_sd

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_DAEMON_EVENT_MODULE_CLIENT_H
