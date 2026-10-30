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
#include "experimental/someip_sd/iosock_event_module/module/control_channel.h"

#include "experimental/someip_sd/iosock_event_module/module/log.h"
#include "experimental/someip_sd/iosock_event_module/module/wakeup_signal.h"

#include "score/message_passing/i_server_connection.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace score::someip_sd::iosock_event_module
{
namespace
{

using score::message_passing::IServerConnection;
using score::message_passing::UserData;
using ConnectionUserData = std::uintptr_t;
using CommandCount = std::size_t;

constexpr CommandCount kMaxPendingCommands{64U};

}  // namespace

OperationSucceeded ControlChannel::Start()
{
    const score::message_passing::ServiceProtocolConfig config{
        protocol::kServiceIdentifier, protocol::kMaxMessageSize, protocol::kMaxMessageSize, protocol::kMaxMessageSize};
    const score::message_passing::IServerFactory::ServerConfig server_config{64U, 8U, 64U};
    server_ = server_factory_.Create(config, server_config);
    if (!server_)
    {
        someip_log(SOMEIP_LOG_ERROR, "creating control server failed");
        return false;
    }
    auto on_connect = [](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
        someip_log(SOMEIP_LOG_INFO, "SD daemon connected (pid %d)", static_cast<int>(connection.GetClientIdentity().pid));
        return UserData{std::in_place_type<ConnectionUserData>, 0U};
    };
    auto on_disconnect = [](IServerConnection&) {
        someip_log(SOMEIP_LOG_INFO, "SD daemon disconnected");
    };
    auto on_routing_command = [this](IServerConnection&,
                                     score::cpp::span<const Byte> data) -> score::cpp::expected_blank<score::os::Error> {
        auto message = protocol::Decode(data.data(), data.size());
        if (!message.has_value())
        {
            someip_log(SOMEIP_LOG_WARNING, "invalid routing command dropped");
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        std::unique_lock<std::mutex> lock{mutex_};
        space_.wait(lock, [this]() {
            return stopping_ || (commands_.size() < kMaxPendingCommands);
        });
        if (stopping_)
        {
            someip_log(SOMEIP_LOG_WARNING, "routing command rejected during startup rollback");
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        commands_.push_back(std::move(message).value());
        someip_wakeup_signal(SOMEIP_WAKEUP_CONTROL);
        return {};
    };
    auto on_unexpected_request = [](IServerConnection&,
                                    score::cpp::span<const Byte>) -> score::cpp::expected_blank<score::os::Error> {
        someip_log(SOMEIP_LOG_WARNING, "unexpected control request with reply");
        return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
    };
    const auto result = server_->StartListening(std::move(on_connect),
                                                std::move(on_disconnect),
                                                std::move(on_routing_command),
                                                std::move(on_unexpected_request));
    if (!result.has_value())
    {
        someip_log(SOMEIP_LOG_ERROR,
                   "starting control server failed: %s",
                   std::string{result.error().ToString()}.c_str());
        server_.reset();
        return false;
    }
    return true;
}

void ControlChannel::Stop()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        stopping_ = true;
    }
    space_.notify_all();
    if (server_)
    {
        server_->StopListening();
        server_.reset();
    }
}

std::vector<protocol::Message> ControlChannel::TakeCommands()
{
    std::vector<protocol::Message> commands;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        commands.swap(commands_);
    }
    space_.notify_all();
    return commands;
}

}  // namespace score::someip_sd::iosock_event_module
