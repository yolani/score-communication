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
#include "experimental/someip_sd/event_module/event_module.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"

#include <signal.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <mw_com_config.json>" << std::endl;
        return EXIT_FAILURE;
    }

    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    score::mw::com::runtime::InitializeRuntime(
        score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{argv[1]}});

    std::atomic<bool> running{true};
    {
        score::someip_sd::EventModule event_module{};
        if (!event_module.Start())
        {
            std::cerr << "starting the message_passing server failed" << std::endl;
            return EXIT_FAILURE;
        }
        std::thread receiver{[&event_module, &running]() {
            event_module.Run(running);
        }};
        std::cout << "[EventModule] someip_event_module running, stop with Ctrl+C" << std::endl;

        int signal_number{0};
        sigwait(&signals, &signal_number);

        event_module.Stop();
        running.store(false);
        receiver.join();
    }
    return EXIT_SUCCESS;
}
