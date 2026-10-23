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
#include "experimental/someip_sd/daemon/sd_facade.h"

// Not exported by //score/message_passing (restricted visibility), available transitively.
#include "score/message_passing/i_server_connection.h"

#include "message/MessageBuilder.h"
#include "message/MessageComposer.h"
#include "message/MessageConstant.h"
#include "message/MessageSD.h"
#include "message/SDEntry.h"
#include "runtime/ApplicationConstant.h"
#include "utils/log/logger.h"

#include <algorithm>
#include <exception>
#include <variant>

namespace score::someip_sd
{

namespace
{

using score::message_passing::IServerConnection;
using score::message_passing::UserData;

const score::message_passing::ServiceProtocolConfig kProtocolConfig{protocol::kServiceIdentifier,
                                                                    protocol::kMessageSize,
                                                                    protocol::kMessageSize,
                                                                    protocol::kMessageSize};
const score::message_passing::IServerFactory::ServerConfig kServerConfig{64U, 8U, 64U};

constexpr std::uint8_t kSubscribeMessageFlags{0xC0U};

std::uint16_t ClientIdOf(IServerConnection& connection)
{
    return static_cast<std::uint16_t>(std::get<std::uintptr_t>(connection.GetUserData()));
}

std::shared_ptr<lgsomeip::MessageSD> CreateInternalMessage(std::uint16_t client_id)
{
    auto message = lgsomeip::MessageBuilder::create<lgsomeip::SOMEIPSD>();
    // lg-someip identifies the local application by the client part of the request id.
    message->set_request_id(static_cast<std::uint32_t>(client_id) << 16U);
    return message;
}

}  // namespace

SdFacade::SdFacade(lgsomeip::ServiceManager& service_manager, SdOnlyRouter& router)
    : service_manager_{service_manager}, router_{router}
{
    router_.SetInternalMessageHandler([this](std::uint16_t client_id, std::vector<std::uint8_t> message) {
        router_.Post([this, client_id, message = std::move(message)]() {
            HandleServiceManagerMessage(client_id, message);
        });
    });
}

SdFacade::~SdFacade()
{
    Stop();
    router_.SetInternalMessageHandler(nullptr);
}

bool SdFacade::Start()
{
    server_ = server_factory_.Create(kProtocolConfig, kServerConfig);
    if (!server_)
    {
        return false;
    }

    auto connect_callback =
        [this](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
        const auto client_id = next_client_id_.fetch_add(1U);
        {
            std::lock_guard<std::mutex> lock{connections_mutex_};
            connections_[client_id] = &connection;
        }
        LGSOMEIP_LOG_INFO << "SdFacade: client 0x" << std::hex << client_id << std::dec << " connected (pid "
                          << connection.GetClientIdentity().pid << ")";
        return UserData{std::in_place_type<std::uintptr_t>, client_id};
    };

    auto disconnect_callback = [this](IServerConnection& connection) {
        const auto client_id = ClientIdOf(connection);
        {
            std::lock_guard<std::mutex> lock{connections_mutex_};
            connections_.erase(client_id);
        }
        router_.Post([this, client_id]() {
            HandleClientDisconnect(client_id);
        });
    };

    auto sent_callback = [this](IServerConnection& connection,
                                score::cpp::span<const std::uint8_t> data) -> score::cpp::expected_blank<score::os::Error> {
        const auto message = protocol::Decode(data.data(), data.size());
        if (!message.has_value())
        {
            LGSOMEIP_LOG_WARN << "SdFacade: invalid message dropped";
            return {};
        }
        const auto client_id = ClientIdOf(connection);
        router_.Post([this, client_id, message = message.value()]() {
            HandleClientMessage(client_id, message);
        });
        return {};
    };

    auto sent_with_reply_callback =
        [](IServerConnection& connection,
           score::cpp::span<const std::uint8_t> data) -> score::cpp::expected_blank<score::os::Error> {
        const auto message = protocol::Decode(data.data(), data.size());
        if ((!message.has_value()) || (message->opcode != protocol::Opcode::kRegister))
        {
            return score::cpp::make_unexpected(score::os::Error::createUnspecifiedError());
        }
        protocol::Message reply{};
        reply.opcode = protocol::Opcode::kRegister;
        reply.client_id = ClientIdOf(connection);
        const auto buffer = protocol::Encode(reply);
        return connection.Reply(score::cpp::span<const std::uint8_t>{buffer.data(), buffer.size()});
    };

    const auto result = server_->StartListening(std::move(connect_callback),
                                                std::move(disconnect_callback),
                                                std::move(sent_callback),
                                                std::move(sent_with_reply_callback));
    return result.has_value();
}

void SdFacade::Stop()
{
    if (server_)
    {
        server_->StopListening();
        server_.reset();
    }
}

void SdFacade::HandleClientMessage(std::uint16_t client_id, const protocol::Message& message)
{
    auto& client = clients_[client_id];
    const ServiceKey key{message.service, message.instance};
    auto& subscriptions = client.subscriptions;
    const auto matches = [&message](const Subscription& subscription) {
        return (subscription.service == message.service) && (subscription.instance == message.instance) &&
               (subscription.eventgroup == message.eventgroup);
    };

    LGSOMEIP_LOG_INFO << "SdFacade: client 0x" << std::hex << client_id << " opcode 0x"
                      << static_cast<std::uint32_t>(message.opcode) << " " << message.service << "."
                      << message.instance << " eventgroup " << message.eventgroup << std::dec;

    switch (message.opcode)
    {
        case protocol::Opcode::kRequestService:
            SendFindService(client_id, message, lgsomeip::SOMEIP_DEFAULT_TTL_ON);
            break;
        case protocol::Opcode::kReleaseService:
            for (const auto& subscription : subscriptions)
            {
                if ((subscription.service == message.service) && (subscription.instance == message.instance) &&
                    subscription.sent)
                {
                    SendSubscribe(client_id, subscription, lgsomeip::SOMEIP_DEFAULT_TTL_OFF);
                }
            }
            subscriptions.erase(std::remove_if(subscriptions.begin(),
                                               subscriptions.end(),
                                               [&message](const Subscription& subscription) {
                                                   return (subscription.service == message.service) &&
                                                          (subscription.instance == message.instance);
                                               }),
                                subscriptions.end());
            client.available.erase(key);
            SendFindService(client_id, message, lgsomeip::SOMEIP_DEFAULT_TTL_OFF);
            break;
        case protocol::Opcode::kSubscribe:
        {
            auto found = std::find_if(subscriptions.begin(), subscriptions.end(), matches);
            if (found == subscriptions.end())
            {
                subscriptions.push_back(
                    Subscription{message.service, message.instance, message.eventgroup, message.major, false});
                found = std::prev(subscriptions.end());
            }
            // The ServiceManager only accepts internal subscriptions for already available services.
            if ((!found->sent) && (client.available.count(key) != 0U))
            {
                SendSubscribe(client_id, *found, lgsomeip::SOMEIP_DEFAULT_TTL_ON);
                found->sent = true;
            }
            break;
        }
        case protocol::Opcode::kUnsubscribe:
        {
            const auto found = std::find_if(subscriptions.begin(), subscriptions.end(), matches);
            if (found != subscriptions.end())
            {
                if (found->sent)
                {
                    SendSubscribe(client_id, *found, lgsomeip::SOMEIP_DEFAULT_TTL_OFF);
                }
                subscriptions.erase(found);
            }
            break;
        }
        case protocol::Opcode::kOfferService:
            if (client.offered.count(key) == 0U)
            {
                client.offered[key] = std::make_pair(message.major, message.minor);
                SendOfferService(client_id, key, message.major, message.minor, lgsomeip::SOMEIP_DEFAULT_TTL_ON);
            }
            break;
        case protocol::Opcode::kStopOfferService:
        {
            const auto found = client.offered.find(key);
            if (found != client.offered.end())
            {
                // rename to StopOfferService to avoid confusion
                SendOfferService(
                    client_id, key, found->second.first, found->second.second, lgsomeip::SOMEIP_DEFAULT_TTL_OFF);
                client.offered.erase(found);
            }
            // TODO: error handling? stopping a service that was not offered by the client...
            break;
        }
        case protocol::Opcode::kRegister:
        case protocol::Opcode::kAvailability:
        case protocol::Opcode::kSubscriptionState:
        default:
            LGSOMEIP_LOG_WARN << "SdFacade: unexpected opcode from client 0x" << std::hex << client_id << std::dec;
            break;
    }
}

void SdFacade::HandleServiceManagerMessage(std::uint16_t client_id, std::vector<std::uint8_t> bytes)
{
    const auto found_client = clients_.find(client_id);
    if (found_client == clients_.end())
    {
        return;
    }
    auto& client = found_client->second;

    auto sd_message = lgsomeip::MessageBuilder::create<lgsomeip::SOMEIPSD>();
    if (!lgsomeip::MessageBuilder::build_message(*sd_message, bytes.data(), static_cast<std::uint32_t>(bytes.size())))
    {
        LGSOMEIP_LOG_WARN << "SdFacade: malformed internal SD message for client 0x" << std::hex << client_id;
        return;
    }

    for (auto& entry : sd_message->entries())
    {
        const ServiceKey key{entry.get_service_id(), entry.get_instance_id()};
        protocol::Message notification{};
        notification.service = entry.get_service_id();
        notification.instance = entry.get_instance_id();
        notification.major = entry.get_major_version();

        switch (entry.get_type())
        {
            case lgsomeip::SOMEIP_SD_ENTRY::OFFERSERVICE::TYPEID:
            {
                const bool available = entry.get_ttl() != lgsomeip::SOMEIP_DEFAULT_TTL_OFF;
                notification.opcode = protocol::Opcode::kAvailability;
                notification.minor = entry.get_minor_version();
                notification.flag = available ? 1U : 0U;
                Notify(client_id, notification);

                if (available)
                {
                    client.available.insert(key);
                }
                else
                {
                    client.available.erase(key);
                }
                for (auto& subscription : client.subscriptions)
                {
                    if ((subscription.service != key.first) || (subscription.instance != key.second))
                    {
                        continue;
                    }
                    if (available && (!subscription.sent))
                    {
                        SendSubscribe(client_id, subscription, lgsomeip::SOMEIP_DEFAULT_TTL_ON);
                        subscription.sent = true;
                    }
                    else if ((!available) && subscription.sent)
                    {
                        // Re-subscribed automatically on the next offer.
                        subscription.sent = false;
                        protocol::Message lost{};
                        lost.opcode = protocol::Opcode::kSubscriptionState;
                        lost.service = subscription.service;
                        lost.instance = subscription.instance;
                        lost.eventgroup = subscription.eventgroup;
                        Notify(client_id, lost);
                    }
                }
                break;
            }
            case lgsomeip::SOMEIP_SD_ENTRY::SUBSCRIBEACK::TYPEID:
                notification.opcode = protocol::Opcode::kSubscriptionState;
                notification.eventgroup = entry.get_event_group_id();
                notification.flag = (entry.get_ttl() != lgsomeip::SOMEIP_DEFAULT_TTL_OFF) ? 1U : 0U;
                Notify(client_id, notification);
                break;
            case lgsomeip::SOMEIP_SD_ENTRY::SUBSCRIBE::TYPEID:
                // A (remote) subscriber of a service offered by this client; accepted on the client's behalf.
                if ((client.offered.count(key) != 0U) && (entry.get_ttl() != lgsomeip::SOMEIP_DEFAULT_TTL_OFF))
                {
                    SendSubscribeAck(
                        client_id, key, entry.get_event_group_id(), entry.get_major_version(), entry.get_ttl());
                }
                break;
            default:
                break;
        }
    }
}

void SdFacade::HandleClientDisconnect(std::uint16_t client_id)
{
    LGSOMEIP_LOG_INFO << "SdFacade: client 0x" << std::hex << client_id << std::dec << " disconnected";
    const auto found = clients_.find(client_id);
    if (found != clients_.end())
    {
        // The ServiceManager does not remove the routes of a disconnected provider, a StopOffer does.
        for (const auto& offer : found->second.offered)
        {
            SendOfferService(
                client_id, offer.first, offer.second.first, offer.second.second, lgsomeip::SOMEIP_DEFAULT_TTL_OFF);
        }
    }
    clients_.erase(client_id);
    try
    {
        service_manager_.on_disconnected_application(client_id);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdFacade: cleanup of client failed: " << exception.what();
    }
}

void SdFacade::SendFindService(std::uint16_t client_id, const protocol::Message& message, std::uint32_t ttl)
{
    auto sd_message = CreateInternalMessage(client_id);
    lgsomeip::SDEntry entry{lgsomeip::SOMEIP_SD_ENTRY::FINDSERVICE::TYPEID};
    entry.set_service_id(message.service);
    entry.set_instance_id(message.instance);
    entry.set_major_version(message.major);
    entry.set_minor_version(message.minor);
    entry.set_ttl(ttl);
    lgsomeip::MessageComposer::add_entry(sd_message, &entry, nullptr);

    try
    {
        service_manager_.on_internal_message(sd_message);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdFacade: find service failed: " << exception.what();
    }
}

void SdFacade::SendSubscribe(std::uint16_t client_id, const Subscription& subscription, std::uint32_t ttl)
{
    auto sd_message = CreateInternalMessage(client_id);
    sd_message->set_flag(kSubscribeMessageFlags);
    lgsomeip::SDEntry entry{lgsomeip::SOMEIP_SD_ENTRY::SUBSCRIBE::TYPEID};
    entry.set_service_id(subscription.service);
    entry.set_instance_id(subscription.instance);
    entry.set_major_version(subscription.major);
    entry.set_event_group_id(subscription.eventgroup);
    entry.set_ttl(ttl);
    entry.set_flag(0x00U);
    lgsomeip::MessageComposer::add_entry(sd_message, &entry, nullptr);

    try
    {
        service_manager_.on_internal_message(sd_message);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdFacade: subscribe failed: " << exception.what();
    }
}

void SdFacade::SendOfferService(std::uint16_t client_id,
                                const ServiceKey& key,
                                std::uint8_t major,
                                std::uint32_t minor,
                                std::uint32_t ttl)
{
    auto sd_message = CreateInternalMessage(client_id);
    lgsomeip::SDEntry entry{lgsomeip::SOMEIP_SD_ENTRY::OFFERSERVICE::TYPEID};
    entry.set_service_id(key.first);
    entry.set_instance_id(key.second);
    entry.set_major_version(major);
    entry.set_minor_version(minor);
    entry.set_ttl(ttl);
    // TODO; add_entry return value? why is this needed?
    lgsomeip::MessageComposer::add_entry(sd_message, &entry, nullptr);

    try
    {
        service_manager_.on_internal_message(sd_message);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdFacade: offer service failed: " << exception.what();
    }
}

void SdFacade::SendSubscribeAck(std::uint16_t client_id,
                                const ServiceKey& key,
                                std::uint16_t eventgroup,
                                std::uint8_t major,
                                std::uint32_t ttl)
{
    auto sd_message = CreateInternalMessage(client_id);
    sd_message->set_flag(kSubscribeMessageFlags);
    lgsomeip::SDEntry entry{lgsomeip::SOMEIP_SD_ENTRY::SUBSCRIBEACK::TYPEID};
    entry.set_service_id(key.first);
    entry.set_instance_id(key.second);
    entry.set_major_version(major);
    entry.set_event_group_id(eventgroup);
    entry.set_ttl(ttl);
    entry.set_flag(0x00U);
    lgsomeip::MessageComposer::add_entry(sd_message, &entry, nullptr);

    try
    {
        service_manager_.on_internal_message(sd_message);
    }
    catch (const std::exception& exception)
    {
        LGSOMEIP_LOG_ERROR << "SdFacade: subscribe ack failed: " << exception.what();
    }
}

void SdFacade::Notify(std::uint16_t client_id, const protocol::Message& message)
{
    const auto buffer = protocol::Encode(message);
    std::lock_guard<std::mutex> lock{connections_mutex_};
    const auto found = connections_.find(client_id);
    if (found != connections_.end())
    {
        static_cast<void>(found->second->Notify(score::cpp::span<const std::uint8_t>{buffer.data(), buffer.size()}));
    }
}

}  // namespace score::someip_sd
