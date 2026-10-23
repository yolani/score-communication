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

// Remote SOME/IP consumer for the SD prototype's provider path, on the upstream lg-someip stack (requires a running
// someipd in the same namespace): subscribes the service offered by someip_sd_test_provider, checks the
// notifications, unsubscribes, checks that no further notifications arrive and waits for the StopOffer.

#include <lgsomeip/LgsomeipApi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr lgsomeip::api::service_t kService = 0x2345U;
constexpr lgsomeip::api::instance_t kInstance = 0x0001U;
constexpr lgsomeip::api::eventgroup_t kEventgroup = 0x0010U;
constexpr lgsomeip::api::event_t kEvent = 0x8001U;
constexpr char kApplicationName[] = "someip-sd-peer-consumer";

constexpr std::size_t kEventsToReceive{5U};
constexpr std::chrono::seconds kReceiveTimeout{20};
constexpr std::chrono::milliseconds kUnsubscribeGracePeriod{500};
constexpr std::chrono::seconds kUnsubscribeCheckPeriod{3};
constexpr std::chrono::seconds kStopOfferTimeout{40};

std::mutex mutex;
std::condition_variable condition;
bool available{false};
bool was_available{false};
bool unsubscribed{false};
std::vector<std::uint32_t> counters;
std::size_t late_events{0U};

void Print(const std::string& text)
{
    std::cout << "peer consumer: " << text << std::endl;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <lg-someip config json>" << std::endl;
        return EXIT_FAILURE;
    }

    auto application = lgsomeip::api::Runtime::instance().create_application(kApplicationName, argv[1]);
    if (!application->init())
    {
        std::cerr << "init failed" << std::endl;
        return EXIT_FAILURE;
    }

    application->register_availability_handler(
        kService, kInstance, [](lgsomeip::api::service_t, lgsomeip::api::instance_t, bool is_available) {
            Print(std::string{"service 0x2345.0x0001 "} + (is_available ? "available" : "not available"));
            std::lock_guard<std::mutex> lock{mutex};
            available = is_available;
            was_available = was_available || is_available;
            condition.notify_all();
        });
    application->register_message_handler(
        kService, kInstance, kEvent, [](const std::shared_ptr<lgsomeip::api::Message>& message) {
            if ((message->type != lgsomeip::api::MessageType::Notification) || (message->payload.size() != 4U))
            {
                return;
            }
            const auto counter = (static_cast<std::uint32_t>(message->payload[0]) << 24U) |
                                 (static_cast<std::uint32_t>(message->payload[1]) << 16U) |
                                 (static_cast<std::uint32_t>(message->payload[2]) << 8U) |
                                 static_cast<std::uint32_t>(message->payload[3]);
            Print("event 0x8001 session " + std::to_string(message->session) + " counter " + std::to_string(counter));
            std::lock_guard<std::mutex> lock{mutex};
            if (unsubscribed)
            {
                ++late_events;
            }
            else
            {
                counters.push_back(counter);
            }
            condition.notify_all();
        });
    application->register_application_state_handler([&application](bool registered) {
        if (registered)
        {
            const std::set<lgsomeip::api::eventgroup_t> eventgroups{kEventgroup};
            application->request_service(kService, kInstance);
            application->request_event(kService, kInstance, kEvent, eventgroups);
            application->subscribe(kService, kInstance, kEventgroup);
        }
    });
    application->start();

    bool received_all{false};
    {
        std::unique_lock<std::mutex> lock{mutex};
        received_all = condition.wait_for(lock, kReceiveTimeout, []() {
            return counters.size() >= kEventsToReceive;
        });
    }
    Print("received " + std::to_string(counters.size()) + " event(s)");

    application->unsubscribe(kService, kInstance, kEventgroup);
    Print("unsubscribed eventgroup 0x0010");
    std::this_thread::sleep_for(kUnsubscribeGracePeriod);
    {
        std::lock_guard<std::mutex> lock{mutex};
        unsubscribed = true;
    }
    std::this_thread::sleep_for(kUnsubscribeCheckPeriod);

    bool stop_offer_seen{false};
    {
        std::unique_lock<std::mutex> lock{mutex};
        stop_offer_seen = condition.wait_for(lock, kStopOfferTimeout, []() {
            return was_available && (!available);
        });
    }

    bool increasing{true};
    std::size_t late{0U};
    {
        std::lock_guard<std::mutex> lock{mutex};
        for (std::size_t index = 1U; index < counters.size(); ++index)
        {
            increasing = increasing && (counters[index] > counters[index - 1U]);
        }
        late = late_events;
    }
    application->release_service(kService, kInstance);
    application->stop();

    Print(late == 0U ? "no events after unsubscribe" : std::to_string(late) + " event(s) after unsubscribe");
    Print(stop_offer_seen ? "service not available after stop offer" : "stop offer not seen");
    const bool ok = received_all && increasing && (late == 0U) && stop_offer_seen;
    Print(std::string{"result: "} + (ok ? "OK" : "FAILED"));
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
