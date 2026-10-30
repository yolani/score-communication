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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_BENCH_PROTOCOL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_BENCH_PROTOCOL_H

#include <cstddef>
#include <cstdint>

/// Wire contract of the SOME/IP round-trip benchmark, shared by all stacks under test and the reflector.
/// The benchmark application provides "ping" and consumes "pong"; the reflector echoes every ping payload unchanged
/// as a pong notification. Both are plain events (no fields) over UDP.
namespace score::someip_sd::benchmark
{

constexpr std::uint16_t kPingService{0xB001U};
constexpr std::uint16_t kPingInstance{0x0001U};
constexpr std::uint16_t kPingEventgroup{0x0001U};
constexpr std::uint16_t kPingEvent{0x8001U};

constexpr std::uint16_t kPongService{0xB002U};
constexpr std::uint16_t kPongInstance{0x0001U};
constexpr std::uint16_t kPongEventgroup{0x0002U};
constexpr std::uint16_t kPongEvent{0x8002U};

/// Payload header, big endian: [0..3] sequence number, [4..11] send time in ns (sender's monotonic clock).
/// The rest of the payload is padding up to the configured payload size.
constexpr std::size_t kHeaderSize{12U};

struct Header
{
    std::uint32_t sequence;
    std::uint64_t send_time_ns;
};

inline void EncodeHeader(const Header& header, std::uint8_t* const payload)
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        payload[index] = static_cast<std::uint8_t>(header.sequence >> (8U * (3U - index)));
    }
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        payload[4U + index] = static_cast<std::uint8_t>(header.send_time_ns >> (8U * (7U - index)));
    }
}

inline Header DecodeHeader(const std::uint8_t* const payload)
{
    Header header{0U, 0U};
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        header.sequence = (header.sequence << 8U) | payload[index];
    }
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        header.send_time_ns = (header.send_time_ns << 8U) | payload[4U + index];
    }
    return header;
}

}  // namespace score::someip_sd::benchmark

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_BENCH_PROTOCOL_H
