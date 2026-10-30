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

// Remote SOME/IP peer for the SD prototype: offers one service instance continuously via the
// upstream lg-someip stack (requires a running someipd in the same namespace) and notifies one field.

#include <lgsomeip/LgsomeipApi.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>

namespace
{

constexpr lgsomeip::api::service_t kService = 0x1234U;
constexpr lgsomeip::api::instance_t kInstance = 0x5678U;
constexpr lgsomeip::api::eventgroup_t kEventgroup = 0x4465U;
constexpr lgsomeip::api::event_t kEvent = 0x8778U;
constexpr char kApplicationName[] = "someip-sd-peer";

std::atomic<bool> g_running{true};
std::atomic<bool> g_registered{false};

void OnSignal(int /*signal*/)
{
    g_running = false;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <lg-someip config json>" << std::endl;
        return 1;
    }

    auto application = lgsomeip::api::Runtime::instance().create_application(kApplicationName, argv[1]);
    if (!application->init())
    {
        std::cerr << "init failed" << std::endl;
        return 1;
    }

    // The stack restores offers after a daemon reconnect, so offering once is sufficient.
    bool offered = false;
    application->register_application_state_handler([&application, &offered](bool registered) {
        std::cout << "daemon " << (registered ? "registered" : "deregistered") << std::endl;
        if (registered && !offered)
        {
            const std::set<lgsomeip::api::eventgroup_t> eventgroups{kEventgroup};
            application->offer_event(kService, kInstance, kEvent, eventgroups, true);
            application->offer_service(kService, kInstance);
            offered = true;
        }
        g_registered = registered;
    });

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    application->start();

    std::uint32_t counter = 0U;
    while (g_running)
    {
        if (g_registered)
        {
            const lgsomeip::api::Payload payload{static_cast<std::uint8_t>(counter >> 24U),
                                                 static_cast<std::uint8_t>(counter >> 16U),
                                                 static_cast<std::uint8_t>(counter >> 8U),
                                                 static_cast<std::uint8_t>(counter)};
            application->notify(kService, kInstance, kEvent, payload);
            std::cout << "[SomeIP Peer Provider] notified counter " << counter << std::endl;
            ++counter;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    application->stop_offer_service(kService, kInstance);
    application->stop();
    return 0;
}
