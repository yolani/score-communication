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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_KERNEL_SOCKETS_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_KERNEL_SOCKETS_H

#include "experimental/someip_sd/iosock_event_module/module/types.h"

#include <set>

namespace score::someip_sd::iosock_event_module
{

/// Kernel UDP sockets shared by RX routes and provided services; a port is opened once and never closed.
class KernelSockets
{
  public:
    OperationSucceeded OpenUdp(Port port);

  private:
    std::set<Port> ports_;
};

}  // namespace score::someip_sd::iosock_event_module

#endif
