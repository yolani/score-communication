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

// Round-trip benchmark for the SOME/IP-SD prototype (io-sock or POSIX event module): publishes ping samples via
// LoLa, which the event module sends as SOME/IP notifications; the remote reflector echoes them as pong
// notifications, which the event module delivers as LoLa samples. Reports round-trip time statistics.

#include "experimental/someip_sd/benchmark/bench_protocol.h"
#include "experimental/someip_sd/client/sd_client.h"
#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace com = score::mw::com;
namespace protocol = score::someip_sd::event_module_protocol;
namespace bench = score::someip_sd::benchmark;

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::int64_t;

constexpr std::size_t kMaxReceiveSamples{16U};
constexpr Nanoseconds kNotReceived{-1};
constexpr std::chrono::milliseconds kDiscoveryPollInterval{100};
constexpr std::chrono::seconds kDrainTime{2};

struct Run
{
    std::uint32_t payload_size;
    std::uint32_t rate_hz;
    std::uint32_t count;
};

struct Options
{
    std::vector<Run> runs;
    std::uint32_t warmup{50U};
    std::uint32_t setup_timeout_s{30U};
    std::string label{"iosock"};
    std::string mw_com_config{"./etc/mw_com_config.json"};
};

void Print(const std::string& text)
{
    std::cout << "bench: " << text << std::endl;
}

std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

/// Parses "<payload>:<rate_hz>:<count>".
bool ParseRun(const std::string& text, Run& run)
{
    const auto first = text.find(':');
    const auto second = text.find(':', first + 1U);
    if ((first == std::string::npos) || (second == std::string::npos))
    {
        return false;
    }
    run.payload_size = static_cast<std::uint32_t>(std::stoul(text.substr(0U, first)));
    run.rate_hz = static_cast<std::uint32_t>(std::stoul(text.substr(first + 1U, second - first - 1U)));
    run.count = static_cast<std::uint32_t>(std::stoul(text.substr(second + 1U)));
    return (run.payload_size >= bench::kHeaderSize) && (run.payload_size <= protocol::kMaxPayloadSize) &&
           (run.rate_hz > 0U) && (run.count > 0U);
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string name{argv[index]};
        const std::string text{argv[index + 1]};
        if (name == "--run")
        {
            Run run{};
            if (!ParseRun(text, run))
            {
                return false;
            }
            options.runs.push_back(run);
        }
        else if (name == "--mw-com-config")
        {
            options.mw_com_config = text;
        }
        else if (name == "--label")
        {
            options.label = text;
        }
        else if (name == "--warmup")
        {
            options.warmup = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--setup-timeout")
        {
            options.setup_timeout_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else
        {
            return false;
        }
    }
    return ((argc % 2) == 1) && (!options.runs.empty());
}

/// Collects the round-trip times of the current run's sequence numbers; written by the LoLa receive handler.
class RoundTripRecorder
{
  public:
    void StartMeasurement(std::uint32_t first_sequence, std::uint32_t count)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        first_sequence_ = first_sequence;
        rtt_ns_.assign(count, kNotReceived);
        duplicates_ = 0U;
    }

    void OnPong(const bench::Header& header, std::uint64_t receive_time_ns)
    {
        any_pong_.store(true);
        std::lock_guard<std::mutex> lock{mutex_};
        if ((header.sequence < first_sequence_) || ((header.sequence - first_sequence_) >= rtt_ns_.size()))
        {
            return;
        }
        auto& slot = rtt_ns_[header.sequence - first_sequence_];
        if (slot == kNotReceived)
        {
            slot = static_cast<Nanoseconds>(receive_time_ns - header.send_time_ns);
        }
        else
        {
            ++duplicates_;
        }
    }

    bool AnyPong() const
    {
        return any_pong_.load();
    }

    std::vector<Nanoseconds> Received(std::uint64_t& duplicates) const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        duplicates = duplicates_;
        std::vector<Nanoseconds> received;
        std::copy_if(rtt_ns_.begin(), rtt_ns_.end(), std::back_inserter(received), [](Nanoseconds value) {
            return value != kNotReceived;
        });
        return received;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Nanoseconds> rtt_ns_;
    std::uint32_t first_sequence_{0U};
    std::uint64_t duplicates_{0U};
    std::atomic<bool> any_pong_{false};
};

/// Nearest-rank percentile of sorted values, in microseconds.
double PercentileUs(const std::vector<Nanoseconds>& sorted, double percentile)
{
    const auto rank = static_cast<std::size_t>(std::ceil((percentile / 100.0) * static_cast<double>(sorted.size())));
    const auto index = (rank == 0U) ? 0U : std::min(rank - 1U, sorted.size() - 1U);
    return static_cast<double>(sorted[index]) / 1000.0;
}

std::string ResultJson(const std::string& label,
                       const Run& run,
                       std::uint32_t send_failed,
                       std::vector<Nanoseconds> rtt,
                       std::uint64_t duplicates)
{
    std::sort(rtt.begin(), rtt.end());
    std::ostringstream json;
    json << "{\"label\":\"" << label << "\",\"payload\":" << run.payload_size << ",\"rate_hz\":" << run.rate_hz
         << ",\"sent\":" << run.count << ",\"send_failed\":" << send_failed << ",\"received\":" << rtt.size()
         << ",\"lost\":" << (run.count - rtt.size()) << ",\"duplicates\":" << duplicates;
    if (!rtt.empty())
    {
        double sum{0.0};
        for (const auto value : rtt)
        {
            sum += static_cast<double>(value);
        }
        json << ",\"rtt_us\":{\"min\":" << PercentileUs(rtt, 0.0) << ",\"p50\":" << PercentileUs(rtt, 50.0)
             << ",\"p90\":" << PercentileUs(rtt, 90.0) << ",\"p99\":" << PercentileUs(rtt, 99.0)
             << ",\"p999\":" << PercentileUs(rtt, 99.9) << ",\"max\":" << PercentileUs(rtt, 100.0)
             << ",\"mean\":" << (sum / static_cast<double>(rtt.size()) / 1000.0) << "}";
    }
    json << "}";
    return json.str();
}

/// LoLa consumer of the pong event offered by the event module.
class PongReceiver
{
  public:
    explicit PongReceiver(RoundTripRecorder& recorder) : recorder_{recorder} {}

    ~PongReceiver()
    {
        if (event_ != nullptr)
        {
            static_cast<void>(event_->UnsetReceiveHandler());
            event_->Unsubscribe();
        }
    }

    PongReceiver(const PongReceiver&) = delete;
    PongReceiver& operator=(const PongReceiver&) = delete;
    PongReceiver(PongReceiver&&) = delete;
    PongReceiver& operator=(PongReceiver&&) = delete;

    bool Connect()
    {
        const auto name = protocol::LolaInstanceSpecifier(bench::kPongService, bench::kPongInstance);
        auto specifier = com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            return false;
        }
        auto handles = com::GenericProxy::FindService(specifier.value());
        if ((!handles.has_value()) || handles.value().empty())
        {
            return false;
        }
        auto proxy = com::GenericProxy::Create(handles.value()[0]);
        if (!proxy.has_value())
        {
            return false;
        }
        proxy_ = std::make_unique<com::GenericProxy>(std::move(proxy).value());
        const auto found = proxy_->GetEvents().find(protocol::LolaEventName(bench::kPongEvent));
        if ((found == proxy_->GetEvents().cend()) || (!found->second.Subscribe(kMaxReceiveSamples).has_value()))
        {
            Print("subscribing LoLa pong event failed");
            proxy_.reset();
            return false;
        }
        event_ = &found->second;
        // Event-driven reception keeps the harness's own polling delay out of the measured round trip.
        static_cast<void>(event_->SetReceiveHandler([this]() noexcept {
            Drain();
        }));
        return true;
    }

  private:
    void Drain()
    {
        static_cast<void>(event_->GetNewSamples(
            [this](auto sample) noexcept {
                const auto receive_time = NowNs();
                const auto* data = static_cast<const protocol::EventSample*>(sample.get());
                if ((data != nullptr) && (data->size >= bench::kHeaderSize))
                {
                    recorder_.OnPong(bench::DecodeHeader(data->payload.data()), receive_time);
                }
            },
            kMaxReceiveSamples));
    }

    RoundTripRecorder& recorder_;
    std::unique_ptr<com::GenericProxy> proxy_;
    com::GenericProxyEvent* event_{nullptr};
};

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: " << argv[0]
                  << " --run <payload 12..1400>:<rate_hz>:<count> [--run ...] [--warmup 50] [--setup-timeout 30]"
                     " [--label iosock] [--mw-com-config ./etc/mw_com_config.json]"
                  << std::endl;
        return EXIT_FAILURE;
    }

    com::runtime::InitializeRuntime(com::runtime::RuntimeConfiguration{score::filesystem::Path{options.mw_com_config}});

    const auto ping_name = protocol::LolaInstanceSpecifier(bench::kPingService, bench::kPingInstance);
    const auto ping_event_name = protocol::LolaEventName(bench::kPingEvent);
    auto specifier = com::InstanceSpecifier::Create(std::string{ping_name});
    if (!specifier.has_value())
    {
        return EXIT_FAILURE;
    }
    const com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
    const std::array<com::EventInfo, 1U> events{{{ping_event_name, meta}}};
    com::GenericSkeletonServiceElementInfo info;
    info.events = events;
    auto skeleton = com::GenericSkeleton::Create(specifier.value(), info);
    if ((!skeleton.has_value()) || (!skeleton.value().OfferService().has_value()))
    {
        Print("offering LoLa service " + ping_name + " failed");
        return EXIT_FAILURE;
    }
    auto& ping = skeleton.value().GetEvents().find(ping_event_name)->second;

    score::someip_sd::SdClient client{[](std::uint16_t, std::uint16_t, bool) {},
                                      [](std::uint16_t, std::uint16_t, std::uint16_t, bool) {}};
    if (!client.Connect(std::chrono::seconds{5}).has_value())
    {
        Print("connecting to the SD daemon failed");
        return EXIT_FAILURE;
    }
    client.OfferService(bench::kPingService, bench::kPingInstance, 0U, 0U);
    client.RequestService(bench::kPongService, bench::kPongInstance, 0U, 0xFFFFFFFFU);
    client.Subscribe(bench::kPongService, bench::kPongInstance, bench::kPongEventgroup, 0U);

    RoundTripRecorder recorder{};
    PongReceiver receiver{recorder};
    std::uint32_t sequence{0U};
    std::uint32_t send_failed{0U};
    const auto send_ping = [&](std::uint32_t payload_size) {
        auto sample = ping.Allocate();
        if (!sample.has_value())
        {
            ++send_failed;
            ++sequence;
            return;
        }
        auto* const data = static_cast<protocol::EventSample*>(sample.value().Get());
        data->size = payload_size;
        bench::EncodeHeader(bench::Header{sequence, NowNs()}, data->payload.data());
        if (!ping.Send(std::move(sample).value()).has_value())
        {
            ++send_failed;
        }
        ++sequence;
    };
    const auto send_paced = [&](const Run& run, std::uint32_t count) {
        const auto period = std::chrono::nanoseconds{1000000000LL / run.rate_hz};
        auto next = Clock::now();
        for (std::uint32_t index = 0U; index < count; ++index)
        {
            send_ping(run.payload_size);
            next += period;
            std::this_thread::sleep_until(next);
        }
    };

    // Setup: wait for the pong service, then ping until the first reflection proves both subscriptions are active.
    const auto setup_deadline = Clock::now() + std::chrono::seconds{options.setup_timeout_s};
    while (!receiver.Connect())
    {
        if (Clock::now() >= setup_deadline)
        {
            Print("result: FAILED, LoLa pong service not found");
            return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(kDiscoveryPollInterval);
    }
    while (!recorder.AnyPong())
    {
        if (Clock::now() >= setup_deadline)
        {
            Print("result: FAILED, no pong received during setup");
            return EXIT_FAILURE;
        }
        send_ping(bench::kHeaderSize);
        std::this_thread::sleep_for(kDiscoveryPollInterval);
    }

    for (const auto& run : options.runs)
    {
        send_paced(run, options.warmup);
        recorder.StartMeasurement(sequence, run.count);
        send_failed = 0U;
        send_paced(run, run.count);
        std::this_thread::sleep_for(kDrainTime);

        std::uint64_t duplicates{0U};
        const auto received = recorder.Received(duplicates);
        std::cout << "BENCH_RESULT " << ResultJson(options.label, run, send_failed, received, duplicates) << std::endl;
    }

    client.Unsubscribe(bench::kPongService, bench::kPongInstance, bench::kPongEventgroup);
    client.ReleaseService(bench::kPongService, bench::kPongInstance);
    client.StopOfferService(bench::kPingService, bench::kPingInstance);
    skeleton.value().StopOfferService();
    return EXIT_SUCCESS;
}
