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

// Test application: offers a SOME/IP service via the SD daemon and publishes its event as LoLa samples, which the
// event module sends as SOME/IP notifications to the remote subscribers.

#include "experimental/someip_sd/client/sd_client.h"
#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <signal.h>
#include <time.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

namespace protocol = score::someip_sd::event_module_protocol;

struct Options
{
    std::uint16_t service{0x2345U};
    std::uint16_t instance{0x0001U};
    std::uint16_t event{0x8001U};
    std::uint8_t major{0x00U};
    std::uint32_t minor{0x00000000U};
    std::uint32_t runtime_seconds{30U};
    std::uint32_t interval_ms{200U};
    std::string mw_com_config{"./etc/mw_com_config.json"};
};

void Print(const std::string& text)
{
    std::cout << "provider: " << text << std::endl;
}

std::string Hex(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
    return stream.str();
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string name{argv[index]};
        const std::string text{argv[index + 1]};
        if (name == "--mw-com-config")
        {
            options.mw_com_config = text;
            continue;
        }
        const auto value = static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
        if (name == "--service")
        {
            options.service = static_cast<std::uint16_t>(value);
        }
        else if (name == "--instance")
        {
            options.instance = static_cast<std::uint16_t>(value);
        }
        else if (name == "--event")
        {
            options.event = static_cast<std::uint16_t>(value);
        }
        else if (name == "--major")
        {
            options.major = static_cast<std::uint8_t>(value);
        }
        else if (name == "--minor")
        {
            options.minor = value;
        }
        else if (name == "--runtime")
        {
            options.runtime_seconds = value;
        }
        else if (name == "--interval-ms")
        {
            options.interval_ms = value;
        }
        else
        {
            return false;
        }
    }
    return (argc % 2) == 1;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: " << argv[0]
                  << " [--service 0x2345] [--instance 0x0001] [--event 0x8001] [--major 0] [--minor 0]"
                     " [--runtime 30] [--interval-ms 200] [--mw-com-config ./etc/mw_com_config.json]"
                  << std::endl;
        return EXIT_FAILURE;
    }

    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{options.mw_com_config}});

    const auto name = protocol::LolaInstanceSpecifier(options.service, options.instance);
    const auto event_name = protocol::LolaEventName(options.event);
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{name});
    if (!specifier.has_value())
    {
        return EXIT_FAILURE;
    }
    const score::mw::com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
    const std::array<score::mw::com::EventInfo, 1U> events{{{event_name, meta}}};
    score::mw::com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = score::mw::com::GenericSkeleton::Create(specifier.value(), info);
    if ((!skeleton.has_value()) || (!skeleton.value().OfferService().has_value()))
    {
        Print("offering LoLa service " + name + " failed");
        return EXIT_FAILURE;
    }
    auto& event = skeleton.value().GetEvents().find(event_name)->second;

    score::someip_sd::SdClient client{[](std::uint16_t, std::uint16_t, bool) {},
                                      [](std::uint16_t, std::uint16_t, std::uint16_t, bool) {}};
    if (!client.Connect(std::chrono::seconds{5}).has_value())
    {
        Print("connecting to the SD daemon failed");
        return EXIT_FAILURE;
    }
    client.OfferService(options.service, options.instance, options.major, options.minor);
    Print("offered " + Hex(options.service) + "." + Hex(options.instance) + " (LoLa " + name + ")");

    std::uint32_t counter{0U};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{options.runtime_seconds};
    const timespec interval{static_cast<time_t>(options.interval_ms / 1000U),
                            static_cast<long>((options.interval_ms % 1000U) * 1000U * 1000U)};
    while ((std::chrono::steady_clock::now() < deadline) && (sigtimedwait(&signals, nullptr, &interval) < 0))
    {
        auto sample = event.Allocate();
        if (!sample.has_value())
        {
            continue;
        }
        auto* const data = static_cast<protocol::EventSample*>(sample.value().Get());
        data->size = 4U;
        data->payload[0] = static_cast<std::uint8_t>(counter >> 24U);
        data->payload[1] = static_cast<std::uint8_t>(counter >> 16U);
        data->payload[2] = static_cast<std::uint8_t>(counter >> 8U);
        data->payload[3] = static_cast<std::uint8_t>(counter);
        if (event.Send(std::move(sample).value()).has_value())
        {
            ++counter;
        }
    }

    client.StopOfferService(options.service, options.instance);
    Print("stopped offering after publishing " + std::to_string(counter) + " sample(s)");
    skeleton.value().StopOfferService();
    return EXIT_SUCCESS;
}
