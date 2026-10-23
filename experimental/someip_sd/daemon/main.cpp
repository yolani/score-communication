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
#include "experimental/someip_sd/daemon/event_module_client.h"
#include "experimental/someip_sd/daemon/sd_facade.h"
#include "experimental/someip_sd/daemon/sd_only_router.h"

#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <lg-someip configuration json>" << std::endl;
        return EXIT_FAILURE;
    }

    // Block the termination signals before any thread is started, they are consumed by sigwait below.
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    try
    {
        // Keeps connecting in the background, so the event module may also be started later.
        score::someip_sd::EventModuleClient event_module{};
        if (!event_module.Connect(std::chrono::seconds{2}))
        {
            std::cerr << "event module not reachable yet, routing commands are dropped until it is" << std::endl;
        }

        auto router = std::make_shared<score::someip_sd::SdOnlyRouter>();
        lgsomeip::ServiceManager service_manager{"someip_sd_daemon", argv[1], router};
        router->SetServiceManager(&service_manager);
        router->SetEventModule(&event_module); // TODO: use constructor parameter?
        score::someip_sd::SdFacade facade{service_manager, *router};

        service_manager.init();
        service_manager.start();
        if (!facade.Start())
        {
            std::cerr << "starting the message_passing server failed" << std::endl;
            service_manager.stop();
            return EXIT_FAILURE;
        }
        std::cout << "someip_sd_daemon running, stop with Ctrl+C" << std::endl;

        int signal_number{0};
        sigwait(&signals, &signal_number);

        facade.Stop();
        service_manager.stop();
        router->SetEventModule(nullptr);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "someip_sd_daemon failed: " << exception.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
