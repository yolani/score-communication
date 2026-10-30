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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_POC_PROTOCOL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_POC_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace score::someip_sd::iosock_poc
{

inline constexpr const char* kServiceIdentifier = "iosock_poc";
/// Requests and replies are a single little endian uint64; the reply is the request + 1.
inline constexpr std::size_t kMessageSize = sizeof(std::uint64_t);

inline constexpr const char* kInstanceSpecifier = "iosock/poc";
inline constexpr const char* kEventName = "counter";

/// Datagrams received on the module's kernel UDP socket are offered as this event and echoed to the sender.
inline constexpr std::uint16_t kUdpPort = 30700U;
inline constexpr const char* kDatagramEventName = "datagram";
inline constexpr const char* kTxInstanceSpecifier = "iosock/tx";
inline constexpr std::uint16_t kTxUdpPort = 30701U;
struct DatagramSample
{
    std::uint32_t size;
    std::array<std::uint8_t, 256U> data;
};

}  // namespace score::someip_sd::iosock_poc

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_POC_PROTOCOL_H
