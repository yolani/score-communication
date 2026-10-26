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
#include "experimental/someip_sd/iosock_event_module/module/kernel_sockets.h"

#include "experimental/someip_sd/iosock_event_module/module/event_plugin.h"
#include "experimental/someip_sd/iosock_event_module/module/log.h"

namespace score::someip_sd::iosock_event_module
{

OperationSucceeded KernelSockets::OpenUdp(const Port port)
{
    if (ports_.count(port) != 0U)
    {
        return true;
    }
    const ErrorCode error = someip_event_open_udp_socket(port);
    if (error != 0)
    {
        someip_log(SOMEIP_LOG_ERROR,
                   "opening kernel UDP socket on port %u failed: %d",
                   static_cast<unsigned>(port),
                   error);
        return false;
    }
    ports_.insert(port);
    someip_log(SOMEIP_LOG_INFO, "listening on kernel UDP port %u", static_cast<unsigned>(port));
    return true;
}

}  // namespace score::someip_sd::iosock_event_module
