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

// Remote reflector of the SOME/IP round-trip benchmark on the upstream lg-someip stack (requires a running someipd
// in the same namespace): subscribes the ping event and echoes every payload unchanged as pong notification.
// It is identical for all stacks under test, so only the benchmarked side differs between measurements.

#include "experimental/someip_sd/benchmark/bench_protocol.h"

#include <lgsomeip/LgsomeipApi.h>

#include <signal.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>

namespace
{

namespace bench = score::someip_sd::benchmark;

constexpr char kApplicationName[] = "someip-bench-reflector";

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <lg-someip config json>" << std::endl;
        return EXIT_FAILURE;
    }

    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    auto application = lgsomeip::api::Runtime::instance().create_application(kApplicationName, argv[1]);
    if (!application->init())
    {
        std::cerr << "init failed" << std::endl;
        return EXIT_FAILURE;
    }

    std::atomic<std::uint64_t> reflected{0U};
    application->register_message_handler(
        bench::kPingService,
        bench::kPingInstance,
        bench::kPingEvent,
        [&application, &reflected](const std::shared_ptr<lgsomeip::api::Message>& message) {
            if (message->type == lgsomeip::api::MessageType::Notification)
            {
                application->notify(bench::kPongService, bench::kPongInstance, bench::kPongEvent, message->payload);
                reflected.fetch_add(1U);
            }
        });

    // lg-someip drops a subscribe for a not yet available service and does not repeat it, so subscribe on availability.
    application->register_availability_handler(
        bench::kPingService,
        bench::kPingInstance,
        [&application](lgsomeip::api::service_t, lgsomeip::api::instance_t, bool available) {
            std::cout << "reflector: ping service " << (available ? "available" : "not available") << std::endl;
            if (available)
            {
                application->subscribe(bench::kPingService, bench::kPingInstance, bench::kPingEventgroup);
            }
        });

    // The stack restores offers and requests after a daemon reconnect, so setting them up once is sufficient.
    bool set_up{false};
    application->register_application_state_handler([&application, &set_up](bool registered) {
        std::cout << "reflector: daemon " << (registered ? "registered" : "deregistered") << std::endl;
        if (registered && !set_up)
        {
            application->offer_event(bench::kPongService,
                                     bench::kPongInstance,
                                     bench::kPongEvent,
                                     std::set<lgsomeip::api::eventgroup_t>{bench::kPongEventgroup},
                                     false);
            application->offer_service(bench::kPongService, bench::kPongInstance);
            application->request_service(bench::kPingService, bench::kPingInstance);
            application->request_event(bench::kPingService,
                                       bench::kPingInstance,
                                       bench::kPingEvent,
                                       std::set<lgsomeip::api::eventgroup_t>{bench::kPingEventgroup});
            set_up = true;
        }
    });
    application->start();

    int signal_number{0};
    static_cast<void>(sigwait(&signals, &signal_number));

    std::cout << "reflector: reflected " << reflected.load() << " notification(s)" << std::endl;
    application->unsubscribe(bench::kPingService, bench::kPingInstance, bench::kPingEventgroup);
    application->stop_offer_service(bench::kPongService, bench::kPongInstance);
    application->stop();
    return EXIT_SUCCESS;
}
