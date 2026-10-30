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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_PROTOCOL_EVENT_MODULE_PROTOCOL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_PROTOCOL_EVENT_MODULE_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace score::someip_sd::event_module_protocol
{

/// Name of the message_passing service offered by the event module.
inline constexpr const char* kServiceIdentifier = "someip_event_module";

inline constexpr std::uint8_t kVersion = 2U;

/// Routing commands from the SD daemon to the event module (SD daemon -> event module only).
enum class Opcode : std::uint8_t
{
    /// Remote service instance became available: open local_port, accept data from remote_ip:remote_port,
    /// offer the LoLa service instance with the given events.
    kAddRoute = 0x01U,
    /// Removes a remote route or a provided service.
    kRemoveRoute = 0x02U,
    /// Subscription acknowledged: forward notifications of event to LoLa.
    kAddSubscription = 0x03U,
    kRemoveSubscription = 0x04U,
    /// Local application offers the service instance: consume its LoLa events and send them from local_port.
    kAddProvidedService = 0x05U,
    /// Remote subscriber remote_ip:remote_port for event of a provided service; repeated on every renewal.
    kAddRemoteSubscriber = 0x06U,
    kRemoveRemoteSubscriber = 0x07U,
};

inline constexpr std::size_t kMaxEvents = 64U;

struct Message
{
    Opcode opcode{Opcode::kAddRoute};
    std::uint16_t service{0U};
    std::uint16_t instance{0U};
    std::uint16_t event{0U};
    std::uint16_t local_port{0U};
    std::uint16_t remote_port{0U};
    std::array<std::uint8_t, 4U> remote_ip{};  ///< IPv4, network byte order
    std::uint8_t major{0U};                    ///< kAddProvidedService only
    std::vector<std::uint16_t> events{};       ///< kAddRoute, kAddProvidedService
    std::vector<std::uint16_t> fields{};       ///< kAddProvidedService: events that are fields
};

/// Wire layout, little endian:
/// [0] version, [1] opcode, [2..3] service, [4..5] instance, [6..7] event, [8..9] local port,
/// [10..11] remote port, [12..15] remote IPv4, [16] major, [17] reserved, [18..19] event count n,
/// [20..21] field count m, then n event ids and m field ids
inline constexpr std::size_t kHeaderSize = 22U;
inline constexpr std::size_t kMaxMessageSize = kHeaderSize + (4U * kMaxEvents);

inline std::vector<std::uint8_t> Encode(const Message& message)
{
    const std::size_t count = (message.events.size() < kMaxEvents) ? message.events.size() : kMaxEvents;
    const std::size_t field_count = (message.fields.size() < kMaxEvents) ? message.fields.size() : kMaxEvents;
    std::vector<std::uint8_t> buffer(kHeaderSize + (2U * (count + field_count)), 0U);
    const auto put16 = [&buffer](std::size_t pos, std::uint16_t value) {
        buffer[pos] = static_cast<std::uint8_t>(value & 0xFFU);
        buffer[pos + 1U] = static_cast<std::uint8_t>(value >> 8U);
    };
    buffer[0] = kVersion;
    buffer[1] = static_cast<std::uint8_t>(message.opcode);
    put16(2U, message.service);
    put16(4U, message.instance);
    put16(6U, message.event);
    put16(8U, message.local_port);
    put16(10U, message.remote_port);
    for (std::size_t index = 0U; index < message.remote_ip.size(); ++index)
    {
        buffer[12U + index] = message.remote_ip[index];
    }
    buffer[16] = message.major;
    put16(18U, static_cast<std::uint16_t>(count));
    put16(20U, static_cast<std::uint16_t>(field_count));
    for (std::size_t index = 0U; index < count; ++index)
    {
        put16(kHeaderSize + (2U * index), message.events[index]);
    }
    for (std::size_t index = 0U; index < field_count; ++index)
    {
        put16(kHeaderSize + (2U * (count + index)), message.fields[index]);
    }
    return buffer;
}

inline std::optional<Message> Decode(const std::uint8_t* data, std::size_t size)
{
    if ((data == nullptr) || (size < kHeaderSize) || (data[0] != kVersion))
    {
        return std::nullopt;
    }
    const auto get16 = [data](std::size_t pos) {
        return static_cast<std::uint16_t>(data[pos] | static_cast<std::uint16_t>(data[pos + 1U] << 8U));
    };
    const std::size_t count = get16(18U);
    const std::size_t field_count = get16(20U);
    if ((count > kMaxEvents) || (field_count > kMaxEvents) || (size != (kHeaderSize + (2U * (count + field_count)))))
    {
        return std::nullopt;
    }
    Message message{};
    message.opcode = static_cast<Opcode>(data[1]);
    message.service = get16(2U);
    message.instance = get16(4U);
    message.event = get16(6U);
    message.local_port = get16(8U);
    message.remote_port = get16(10U);
    for (std::size_t index = 0U; index < message.remote_ip.size(); ++index)
    {
        message.remote_ip[index] = data[12U + index];
    }
    message.major = data[16];
    for (std::size_t index = 0U; index < count; ++index)
    {
        message.events.push_back(get16(kHeaderSize + (2U * index)));
    }
    for (std::size_t index = 0U; index < field_count; ++index)
    {
        message.fields.push_back(get16(kHeaderSize + (2U * (count + index))));
    }
    return message;
}

/// LoLa naming of a SOME/IP service instance and event, shared by event module, consumers and mw_com_config.json.
inline std::string Hex4(std::uint16_t value)
{
    constexpr const char* kDigits = "0123456789abcdef";
    std::string text(4U, '0');
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        text[3U - index] = kDigits[(value >> (4U * index)) & 0xFU];
    }
    return text;
}

inline std::string LolaInstanceSpecifier(std::uint16_t service, std::uint16_t instance)
{
    return "someip/" + Hex4(service) + "/" + Hex4(instance);
}

inline std::string LolaEventName(std::uint16_t event)
{
    return "event_" + Hex4(event);
}

/// Sample layout of every event in both directions: payload length followed by the serialized SOME/IP payload.
/// For provided services (TX) only size and payload are used; the event module creates the SOME/IP header.
inline constexpr std::size_t kMaxPayloadSize = 1400U;
struct EventSample
{
    std::uint32_t size;
    std::uint16_t session;
    std::uint8_t interface_version;
    std::uint8_t message_type;
    std::array<std::uint8_t, kMaxPayloadSize> payload;
};

}  // namespace score::someip_sd::event_module_protocol

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_PROTOCOL_EVENT_MODULE_PROTOCOL_H
