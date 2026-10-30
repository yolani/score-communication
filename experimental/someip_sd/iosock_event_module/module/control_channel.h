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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_CONTROL_CHANNEL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_CONTROL_CHANNEL_H

#include "experimental/someip_sd/iosock_event_module/module/types.h"

#include "score/message_passing/server_factory.h"

#include <condition_variable>
#include <mutex>
#include <vector>

namespace score::someip_sd::iosock_event_module
{

/// message_passing endpoint of the SD daemon. Server threads enqueue routing commands into a bounded,
/// backpressured queue; the kernel thread drains it with TakeCommands().
class ControlChannel
{
  public:
    OperationSucceeded Start();
    void Stop();
    std::vector<protocol::Message> TakeCommands();

  private:
    using StopRequested = bool;

    score::message_passing::ServerFactory server_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IServer> server_;
    std::mutex mutex_;
    std::condition_variable space_;
    StopRequested stopping_{false};
    std::vector<protocol::Message> commands_;
};

}  // namespace score::someip_sd::iosock_event_module

#endif
