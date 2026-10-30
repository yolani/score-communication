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
#include "experimental/someip_sd/iosock_event_module/module/log.h"

#include <sys/slog.h>
#include <sys/slogcodes.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace
{

// Formatting into a fixed stack buffer avoids heap allocation on the RX/TX paths; longer messages are truncated.
constexpr std::size_t kMaxMessageSize{256U};

int ToSlogSeverity(const someip_log_severity severity)
{
    switch (severity)
    {
        case SOMEIP_LOG_ERROR:
            return _SLOG_ERROR;
        case SOMEIP_LOG_WARNING:
            return _SLOG_WARNING;
        case SOMEIP_LOG_INFO:
            return _SLOG_INFO;
        case SOMEIP_LOG_DEBUG:
        default:
            return _SLOG_DEBUG1;
    }
}

}  // namespace

extern "C" void someip_log(const someip_log_severity severity, const char* const format, ...)
{
    std::array<char, kMaxMessageSize> message{};
    va_list arguments;
    va_start(arguments, format);
    static_cast<void>(std::vsnprintf(message.data(), message.size(), format, arguments));
    va_end(arguments);

    static_cast<void>(slogf(_SLOGC_NETWORK, ToSlogSeverity(severity), "[someip_event] %s", message.data()));
}
