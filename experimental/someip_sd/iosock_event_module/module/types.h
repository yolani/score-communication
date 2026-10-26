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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_TYPES_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_TYPES_H

#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace score::someip_sd::iosock_event_module
{

//TODO: keep namespace original?
namespace protocol = score::someip_sd::event_module_protocol;

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
using NotificationCount = std::uint64_t;
using OperationSucceeded = bool;
using ErrorCode = int;
using PacketHandle = const void*;
using TxContextHandle = void*;

using ServiceInstance = std::pair<ServiceId, InstanceId>;
using ServiceEvent = std::pair<ServiceId, EventId>;
using Ipv4Address = std::array<Byte, 4U>;
using Endpoint = std::pair<Ipv4Address, Port>;

constexpr PacketSize kHeaderSize{16U};
using SomeipHeader = std::array<Byte, kHeaderSize>;

inline WireWord Read32(const Byte* data)
{
    return (static_cast<WireWord>(data[0]) << 24U) | (static_cast<WireWord>(data[1]) << 16U) |
           (static_cast<WireWord>(data[2]) << 8U) | static_cast<WireWord>(data[3]);
}

inline void Write32(Byte* data, WireWord value)
{
    data[0] = static_cast<Byte>(value >> 24U);
    data[1] = static_cast<Byte>(value >> 16U);
    data[2] = static_cast<Byte>(value >> 8U);
    data[3] = static_cast<Byte>(value);
}

}  // namespace score::someip_sd::iosock_event_module

#endif
