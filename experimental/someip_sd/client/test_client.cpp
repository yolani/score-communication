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

// Test application: requests a service and subscribes an eventgroup via the SD daemon, and receives the
// notifications as LoLa events offered by the event module.

#include "experimental/someip_sd/client/sd_client.h"
#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <signal.h>
#include <time.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace
{

namespace protocol = score::someip_sd::event_module_protocol;

struct Options
{
    std::uint16_t service{0x1234U};
    std::uint16_t instance{0x5678U};
    std::uint16_t eventgroup{0x4465U};
    std::uint16_t event{0x8778U};
    std::uint8_t major{0x00U};
    std::uint32_t minor{0xFFFFFFFFU};
    std::uint32_t runtime_seconds{0U};
    std::string mw_com_config{"./etc/mw_com_config.json"};
};

constexpr std::size_t kMaxSamples{5U};
// Samples sent before the unsubscribe took effect in the event module may still arrive.
constexpr std::chrono::milliseconds kUnsubscribeGracePeriod{500};
constexpr std::chrono::seconds kUnsubscribeCheckPeriod{3};

std::mutex output_mutex;

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock{output_mutex};
    std::cout << "[TestClient application] " << text << std::endl;
}

std::string Hex(std::uint32_t value, int width)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
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
        else if (name == "--eventgroup")
        {
            options.eventgroup = static_cast<std::uint16_t>(value);
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
        else
        {
            return false;
        }
    }
    return (argc % 2) == 1;
}

/// LoLa consumer of one SOME/IP event, as offered by the event module.
class LolaEventReceiver
{
  public:
    explicit LolaEventReceiver(const Options& options)
        : options_{options},
          specifier_{protocol::LolaInstanceSpecifier(options.service, options.instance)},
          event_name_{protocol::LolaEventName(options.event)}
    {
    }

    ~LolaEventReceiver()
    {
        if (event_ != nullptr)
        {
            event_->Unsubscribe();
        }
    }

    LolaEventReceiver(const LolaEventReceiver&) = delete;
    LolaEventReceiver& operator=(const LolaEventReceiver&) = delete;
    LolaEventReceiver(LolaEventReceiver&&) = delete;
    LolaEventReceiver& operator=(LolaEventReceiver&&) = delete;

    /// Returns the number of samples received.
    std::size_t Poll()
    {
        if (event_ == nullptr)
        {
            Connect();
            return 0U;
        }
        const auto received = event_->GetNewSamples(
            [this](auto sample) noexcept {
                Log(Describe(*static_cast<const protocol::EventSample*>(sample.get())));
            },
            kMaxSamples);
        return received.has_value() ? received.value() : 0U;
    }

  private:
    void Connect()
    {
        auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{specifier_});
        if (!specifier.has_value())
        {
            return;
        }
        auto handles = score::mw::com::GenericProxy::FindService(specifier.value());
        if ((!handles.has_value()) || handles.value().empty())
        {
            return;
        }
        auto proxy = score::mw::com::GenericProxy::Create(handles.value()[0]);
        if (!proxy.has_value())
        {
            Log("LoLa: creating proxy for " + specifier_ + " failed");
            return;
        }
        proxy_ = std::make_unique<score::mw::com::GenericProxy>(std::move(proxy).value());
        const auto found = proxy_->GetEvents().find(event_name_);
        if (found == proxy_->GetEvents().cend())
        {
            Log("LoLa: " + specifier_ + " has no event " + event_name_);
            return;
        }
        if (!found->second.Subscribe(kMaxSamples).has_value())
        {
            Log("LoLa: subscribing " + event_name_ + " failed");
            return;
        }
        event_ = &found->second;
        Log("LoLa: found " + specifier_ + ", subscribed " + event_name_);
    }

    std::string Describe(const protocol::EventSample& sample) const
    {
        std::ostringstream text;
        text << "event: service " << Hex(options_.service, 4) << " event " << Hex(options_.event, 4) << " session "
             << Hex(sample.session, 4) << " type " << Hex(sample.message_type, 2) << " payload length " << std::dec
             << sample.size << " payload";
        for (std::size_t index = 0U; (index < sample.size) && (index < 16U); ++index)
        {
            text << " " << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<std::uint32_t>(sample.payload[index]);
        }
        text << " (via LoLa)";
        return text.str();
    }

    Options options_;
    std::string specifier_;
    std::string event_name_;
    std::unique_ptr<score::mw::com::GenericProxy> proxy_;
    score::mw::com::GenericProxyEvent* event_{nullptr};
};

/// Polls the receiver until the duration elapsed (zero: forever) or a termination signal arrived.
/// Returns false on a termination signal.
bool PollFor(LolaEventReceiver& receiver,
             const sigset_t& signals,
             std::chrono::milliseconds duration,
             std::size_t& received)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    const timespec poll_interval{0, 100 * 1000 * 1000};
    while ((duration.count() == 0) || (std::chrono::steady_clock::now() < deadline))
    {
        if (sigtimedwait(&signals, nullptr, &poll_interval) >= 0)
        {
            return false;
        }
        received += receiver.Poll();
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: " << argv[0]
                  << " [--service 0x1234] [--instance 0x5678] [--eventgroup 0x4465] [--event 0x8778] [--major 0]"
                     " [--minor 0xffffffff] [--runtime <seconds, 0 = until SIGINT/SIGTERM>]"
                     " [--mw-com-config ./etc/mw_com_config.json]"
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

    score::someip_sd::SdClient client{
        [](std::uint16_t service, std::uint16_t instance, bool available) {
            Log("availability: " + Hex(service, 4) + "." + Hex(instance, 4) +
                  (available ? " available" : " not available"));
        },
        [](std::uint16_t service, std::uint16_t instance, std::uint16_t eventgroup, bool acknowledged) {
            Log("subscription: " + Hex(service, 4) + "." + Hex(instance, 4) + " eventgroup " + Hex(eventgroup, 4) +
                  (acknowledged ? " acknowledged" : " not acknowledged"));
        }};

    const auto client_id = client.Connect(std::chrono::seconds{5});
    if (!client_id.has_value())
    {
        Log("connecting to the SD daemon failed (is someip_sd_daemon running in the same network namespace?)");
        return EXIT_FAILURE;
    }
    Log("registered at SD daemon as client " + Hex(client_id.value(), 4));
    client.RequestService(options.service, options.instance, options.major, options.minor);
    client.Subscribe(options.service, options.instance, options.eventgroup, options.major);

    int exit_code{EXIT_SUCCESS};
    {
        LolaEventReceiver receiver{options};
        std::size_t received{0U};
        const bool completed = PollFor(receiver, signals, std::chrono::seconds{options.runtime_seconds}, received);

        client.Unsubscribe(options.service, options.instance, options.eventgroup);
        Log("unsubscribed eventgroup " + Hex(options.eventgroup, 4) + " after " + std::to_string(received) +
              " event(s)");

        if (completed)
        {
            std::size_t in_flight{0U};
            std::size_t late{0U};
            if (PollFor(receiver, signals, kUnsubscribeGracePeriod, in_flight) &&
                PollFor(receiver, signals, kUnsubscribeCheckPeriod, late))
            {
                if (received == 0U)
                {
                    Log("result: FAILED, no events received");
                    exit_code = EXIT_FAILURE;
                }
                else if (late != 0U)
                {
                    Log("result: FAILED, " + std::to_string(late) + " event(s) received after unsubscribe");
                    exit_code = EXIT_FAILURE;
                }
                else
                {
                    Log("result: OK, no events after unsubscribe");
                }
            }
        }
    }

    client.ReleaseService(options.service, options.instance);
    return exit_code;
}
