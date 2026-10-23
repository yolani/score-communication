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
#include "experimental/someip_sd/daemon/event_module_client.h"

#include <thread>

namespace score::someip_sd
{

namespace
{

using score::message_passing::IClientConnection;

const score::message_passing::ServiceProtocolConfig kProtocolConfig{event_module_protocol::kServiceIdentifier,
                                                                    event_module_protocol::kMaxMessageSize,
                                                                    event_module_protocol::kMaxMessageSize,
                                                                    event_module_protocol::kMaxMessageSize};
const score::message_passing::IClientFactory::ClientConfig kClientConfig{1U, 64U, true, false, false};

}  // namespace

EventModuleClient::~EventModuleClient()
{
    if (connection_)
    {
        connection_->Stop();
        connection_.reset();
    }
}

bool EventModuleClient::Connect(std::chrono::milliseconds timeout)
{
    connection_ = client_factory_.Create(kProtocolConfig, kClientConfig);
    if (!connection_)
    {
        return false;
    }
    connection_->Start(
        [this](IClientConnection::State state) {
            state_.store(state);
        },
        [](score::cpp::span<const std::uint8_t>) {});

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ((state_.load() == IClientConnection::State::kStarting) && (std::chrono::steady_clock::now() < deadline))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return state_.load() == IClientConnection::State::kReady;
}

bool EventModuleClient::Send(const event_module_protocol::Message& message)
{
    if ((!connection_) || (state_.load() != IClientConnection::State::kReady))
    {
        return false;
    }
    const auto buffer = event_module_protocol::Encode(message);
    return connection_->Send(score::cpp::span<const std::uint8_t>{buffer.data(), buffer.size()}).has_value();
}

}  // namespace score::someip_sd
