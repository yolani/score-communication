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

// Mixed load benchmark for the SOME/IP-SD prototype, benchmarked side: publishes the TX streams of the workload via
// LoLa (sent as SOME/IP notifications by the event module) while receiving the RX streams published by the remote
// someip_load_peer. Reports per event class sent counts (TX) and loss, duplicates and arrival jitter (RX).
// With a CPU sampler (QNX), reports CPU time per thread group ("CPU_RESULT") for: an idle baseline before any service
// is offered, a sweep over --cpu-scales (the peer follows via the phase bits of the TX sequence) and the main window.

#include "experimental/someip_sd/benchmark/bench_protocol.h"
#include "experimental/someip_sd/benchmark/cpu_sampler.h"
#include "experimental/someip_sd/benchmark/load_workload.h"
#include "experimental/someip_sd/client/sd_client.h"
#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"
#include "score/mw/com/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace com = score::mw::com;
namespace protocol = score::someip_sd::event_module_protocol;
namespace bench = score::someip_sd::benchmark;
namespace load = score::someip_sd::benchmark::load;
namespace cpu = score::someip_sd::benchmark::cpu;

using load::Nanoseconds;

constexpr std::size_t kMaxReceiveSamples{4U};
constexpr std::chrono::milliseconds kDiscoveryPollInterval{100};
constexpr std::chrono::seconds kDrainTime{2};
constexpr std::chrono::seconds kCpuSettleTime{1};
constexpr std::chrono::milliseconds kCpuCheckTime{200};

enum class Phase : std::uint8_t
{
    kSetup,
    kMeasure,
    kDrain,
};

struct Options
{
    std::string workload{"./etc/workload.txt"};
    std::string mw_com_config{"./etc/mw_com_config.json"};
    std::string label{"iosock"};
    std::uint32_t duration_s{20U};
    std::uint32_t warmup_s{5U};
    std::uint32_t setup_timeout_s{60U};
    double scale{1.0};
    std::uint32_t cpu_baseline_s{5U};
    std::uint32_t cpu_phase_s{10U};
    std::vector<double> cpu_scales;
    std::vector<std::string> cpu_processes{"io-sock", "someip_sd_daemon"};
};

void Print(const std::string& text)
{
    std::cout << "load: " << text << std::endl;
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string name{argv[index]};
        const std::string text{argv[index + 1]};
        if (name == "--workload")
        {
            options.workload = text;
        }
        else if (name == "--mw-com-config")
        {
            options.mw_com_config = text;
        }
        else if (name == "--label")
        {
            options.label = text;
        }
        else if (name == "--duration")
        {
            options.duration_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--warmup")
        {
            options.warmup_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--setup-timeout")
        {
            options.setup_timeout_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--scale")
        {
            options.scale = std::stod(text);
        }
        else if (name == "--cpu-baseline")
        {
            options.cpu_baseline_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--cpu-phase")
        {
            options.cpu_phase_s = static_cast<std::uint32_t>(std::stoul(text));
        }
        else if (name == "--cpu-scales")
        {
            if (!load::ParseScales(text, options.cpu_scales))
            {
                return false;
            }
        }
        else if (name == "--cpu-process")
        {
            options.cpu_processes.push_back(text);
        }
        else
        {
            return false;
        }
    }
    return ((argc % 2) == 1) && (options.duration_s > 0U) && (options.scale > 0.0) && (options.cpu_phase_s > 0U);
}

/// Receive statistics of all RX streams; written by the LoLa receive handlers.
class Receivers
{
  public:
    Receivers(const load::Workload& workload, double scale, const std::atomic<Phase>& phase)
        : workload_{workload}, phase_{phase}, trackers_(workload.streams.size()), seen_(workload.streams.size(), false)
    {
        for (const auto& stream : workload.streams)
        {
            periods_ns_.push_back(load::PeriodNs(stream, scale));
        }
        jitter_ns_.resize(workload.classes.size());
    }

    void OnSample(std::size_t stream, const bench::Header& header, Nanoseconds arrival_ns)
    {
        received_.fetch_add(1U);
        std::lock_guard<std::mutex> lock{mutex_};
        seen_[stream] = true;
        if (phase_.load() == Phase::kMeasure)
        {
            trackers_[stream].OnSample(
                header.sequence, arrival_ns, periods_ns_[stream], jitter_ns_[workload_.streams[stream].event_class]);
        }
    }

    /// All samples received, including those outside the measurement window.
    std::uint64_t Received() const
    {
        return received_.load();
    }

    std::size_t StreamsSeen() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return static_cast<std::size_t>(std::count(seen_.begin(), seen_.end(), true));
    }

    void Report(load::ClassResult& result, std::uint32_t event_class) const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<const load::ReceiveTracker*> trackers;
        for (std::size_t index = 0U; index < workload_.streams.size(); ++index)
        {
            if (workload_.streams[index].event_class == event_class)
            {
                trackers.push_back(&trackers_[index]);
            }
        }
        result.AddReceived(trackers, jitter_ns_[event_class]);
    }

  private:
    const load::Workload& workload_;
    const std::atomic<Phase>& phase_;
    std::atomic<std::uint64_t> received_{0U};
    mutable std::mutex mutex_;
    std::vector<load::ReceiveTracker> trackers_;
    std::vector<bool> seen_;
    std::vector<Nanoseconds> periods_ns_;
    std::vector<std::vector<Nanoseconds>> jitter_ns_;
};

/// LoLa consumer of one RX service offered by the event module.
class RxService
{
  public:
    RxService(std::uint16_t service,
              std::uint16_t instance,
              std::map<std::uint16_t, std::size_t> streams,
              Receivers& receivers)
        : service_{service}, instance_{instance}, streams_{std::move(streams)}, receivers_{receivers}
    {
    }

    ~RxService()
    {
        for (auto* const event : events_)
        {
            static_cast<void>(event->UnsetReceiveHandler());
            event->Unsubscribe();
        }
    }

    RxService(const RxService&) = delete;
    RxService& operator=(const RxService&) = delete;
    RxService(RxService&&) = delete;
    RxService& operator=(RxService&&) = delete;

    bool Connected() const
    {
        return proxy_ != nullptr;
    }

    bool Connect()
    {
        auto specifier = com::InstanceSpecifier::Create(protocol::LolaInstanceSpecifier(service_, instance_));
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
        for (const auto& [event_id, stream] : streams_)
        {
            const auto found = proxy_->GetEvents().find(protocol::LolaEventName(event_id));
            if ((found == proxy_->GetEvents().cend()) || (!found->second.Subscribe(kMaxReceiveSamples).has_value()))
            {
                Print("subscribing LoLa event " + protocol::LolaEventName(event_id) + " of " +
                      protocol::LolaInstanceSpecifier(service_, instance_) + " failed");
                continue;
            }
            auto* const event = &found->second;
            events_.push_back(event);
            const auto index = stream;
            static_cast<void>(event->SetReceiveHandler([this, event, index]() noexcept {
                Drain(*event, index);
            }));
        }
        return true;
    }

  private:
    void Drain(com::GenericProxyEvent& event, std::size_t stream)
    {
        static_cast<void>(event.GetNewSamples(
            [this, stream](auto sample) noexcept {
                const auto arrival = load::NowNs();
                const auto* data = static_cast<const protocol::EventSample*>(sample.get());
                if ((data != nullptr) && (data->size >= bench::kHeaderSize))
                {
                    receivers_.OnSample(stream, bench::DecodeHeader(data->payload.data()), arrival);
                }
            },
            kMaxReceiveSamples));
    }

    std::uint16_t service_;
    std::uint16_t instance_;
    std::map<std::uint16_t, std::size_t> streams_;
    Receivers& receivers_;
    std::unique_ptr<com::GenericProxy> proxy_;
    std::vector<com::GenericProxyEvent*> events_;
};

struct TxService
{
    std::uint16_t service;
    std::uint16_t instance;
    std::vector<std::string> event_names;
    std::unique_ptr<com::GenericSkeleton> skeleton;
};

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: " << argv[0]
                  << " [--workload ./etc/workload.txt] [--duration 20] [--warmup 5] [--setup-timeout 60] [--scale 1.0]"
                     " [--label iosock] [--mw-com-config ./etc/mw_com_config.json] [--cpu-baseline 5]"
                     " [--cpu-scales 0.25,0.5] [--cpu-phase 10] [--cpu-process <name>]..."
                  << std::endl;
        return EXIT_FAILURE;
    }
    load::Workload workload{};
    if (!load::ReadWorkload(options.workload, workload))
    {
        Print("reading workload " + options.workload + " failed");
        return EXIT_FAILURE;
    }

    // Idle baseline: before any service is offered, so neither the module nor the SD daemon have work.
    const cpu::CpuSampler sampler{options.cpu_processes};
    if (sampler.Supported())
    {
        Print("cpu sampling of processes: " + sampler.Processes() + ", event module thread: " +
              (std::getenv("SOMEIP_EVENT_MODULE_TID") != nullptr ? std::getenv("SOMEIP_EVENT_MODULE_TID") : "by name"));
        const auto accounted_ns = sampler.CheckAccounting(kCpuCheckTime);
        Print("cpu accounting check: busy " + std::to_string(kCpuCheckTime.count()) + " ms, accounted " +
              std::to_string(static_cast<double>(accounted_ns) / 1.0e6) + " ms");
        if (options.cpu_baseline_s > 0U)
        {
            const auto start = sampler.Sample();
            const auto start_ns = load::NowNs();
            std::this_thread::sleep_for(std::chrono::seconds{options.cpu_baseline_s});
            const auto end = sampler.Sample();
            std::cout << cpu::CpuResult(options.label, "baseline", 0.0, load::NowNs() - start_ns, 0U, 0U, start, end)
                      << std::endl;
        }
    }
    else
    {
        Print("cpu sampling not supported on this system");
    }

    com::runtime::InitializeRuntime(com::runtime::RuntimeConfiguration{score::filesystem::Path{options.mw_com_config}});

    // Group the streams by service: TX services are offered via LoLa skeletons, RX services consumed via proxies.
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::map<std::uint16_t, std::size_t>> tx_streams;
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::map<std::uint16_t, std::size_t>> rx_streams;
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint16_t> rx_eventgroups;
    for (std::size_t index = 0U; index < workload.streams.size(); ++index)
    {
        const auto& stream = workload.streams[index];
        const auto key = std::make_pair(stream.service, stream.instance);
        if (workload.IsTx(stream))
        {
            tx_streams[key][stream.event] = index;
        }
        else
        {
            rx_streams[key][stream.event] = index;
            rx_eventgroups[key] = stream.eventgroup;
        }
    }

    const com::DataTypeMetaInfo meta{sizeof(protocol::EventSample), alignof(protocol::EventSample)};
    // Owned by pointer: the skeleton may keep views of the event names.
    std::vector<std::unique_ptr<TxService>> tx_services;
    std::vector<com::GenericSkeletonEvent*> tx_events(workload.streams.size(), nullptr);
    for (const auto& [key, events] : tx_streams)
    {
        tx_services.push_back(std::make_unique<TxService>(TxService{key.first, key.second, {}, nullptr}));
        auto& service = *tx_services.back();
        for (const auto& entry : events)
        {
            service.event_names.push_back(protocol::LolaEventName(entry.first));
        }
        std::vector<com::EventInfo> infos;
        for (const auto& name : service.event_names)
        {
            infos.push_back(com::EventInfo{name, meta});
        }
        const auto name = protocol::LolaInstanceSpecifier(key.first, key.second);
        auto specifier = com::InstanceSpecifier::Create(std::string{name});
        if (!specifier.has_value())
        {
            return EXIT_FAILURE;
        }
        com::GenericSkeletonServiceElementInfo info;
        info.events = infos;
        auto skeleton = com::GenericSkeleton::Create(specifier.value(), info);
        if ((!skeleton.has_value()) || (!skeleton.value().OfferService().has_value()))
        {
            Print("offering LoLa service " + name + " failed");
            return EXIT_FAILURE;
        }
        service.skeleton = std::make_unique<com::GenericSkeleton>(std::move(skeleton).value());
        for (const auto& [event_id, stream] : events)
        {
            tx_events[stream] = &service.skeleton->GetEvents().find(protocol::LolaEventName(event_id))->second;
        }
    }

    score::someip_sd::SdClient client{[](std::uint16_t, std::uint16_t, bool) {},
                                      [](std::uint16_t, std::uint16_t, std::uint16_t, bool) {}};
    if (!client.Connect(std::chrono::seconds{5}).has_value())
    {
        Print("connecting to the SD daemon failed");
        return EXIT_FAILURE;
    }
    for (const auto& service : tx_services)
    {
        client.OfferService(service->service, service->instance, 0U, 0U);
    }
    for (const auto& [key, eventgroup] : rx_eventgroups)
    {
        client.RequestService(key.first, key.second, 0U, 0xFFFFFFFFU);
        client.Subscribe(key.first, key.second, eventgroup, 0U);
    }

    // TX is sent from the start, so that the peer subscribes and receives already during the setup.
    std::atomic<Phase> phase{Phase::kSetup};
    // Index into load_phases, also sent in the TX sequence numbers for the peer.
    std::atomic<std::uint32_t> load_phase{0U};
    std::vector<double> load_phases{options.scale};
    for (const auto factor : options.cpu_scales)
    {
        load_phases.push_back(options.scale * factor);
    }
    std::atomic<std::uint64_t> tx_total{0U};
    std::atomic<bool> stop{false};
    std::vector<std::size_t> tx_indices;
    std::vector<Nanoseconds> tx_periods;
    for (std::size_t index = 0U; index < workload.streams.size(); ++index)
    {
        if (workload.IsTx(workload.streams[index]))
        {
            tx_indices.push_back(index);
            tx_periods.push_back(load::PeriodNs(workload.streams[index], 1.0));
        }
    }
    std::vector<std::uint32_t> sequences(workload.streams.size(), 0U);
    std::vector<load::SendCounters> sent(workload.classes.size());
    std::thread sender{[&]() {
        const load::Pacer pacer{tx_indices, tx_periods};
        pacer.Run(
            [&stop]() {
                return stop.load();
            },
            [&]() {
                return load_phases[load_phase.load()];
            },
            [&](std::size_t index, Nanoseconds lag_ns) {
                const auto& stream = workload.streams[index];
                const bool measured = (phase.load() == Phase::kMeasure);
                auto& counters = sent[stream.event_class];
                const auto sequence = ((sequences[index]++) & load::kCounterMask) |
                                      (load_phase.load() << load::kPhaseShift) | (measured ? load::kMeasuredFlag : 0U);
                bool ok{false};
                auto sample = tx_events[index]->Allocate();
                if (sample.has_value())
                {
                    auto* const data = static_cast<protocol::EventSample*>(sample.value().Get());
                    data->size = stream.payload;
                    bench::EncodeHeader(bench::Header{sequence, static_cast<std::uint64_t>(load::NowNs())},
                                        data->payload.data());
                    ok = tx_events[index]->Send(std::move(sample).value()).has_value();
                }
                tx_total.fetch_add(ok ? 1U : 0U);
                if (measured)
                {
                    ++counters.sent;
                    counters.send_failed += ok ? 0U : 1U;
                    counters.max_lag_ns = std::max(counters.max_lag_ns, lag_ns);
                }
            });
    }};
    const auto stop_sender = [&]() {
        stop.store(true);
        sender.join();
    };

    // Setup: connect all RX proxies, then wait until every RX stream delivered a sample.
    Receivers receivers{workload, options.scale, phase};
    std::vector<std::unique_ptr<RxService>> rx_services;
    std::size_t rx_stream_count{0U};
    for (const auto& [key, events] : rx_streams)
    {
        rx_stream_count += events.size();
        rx_services.push_back(std::make_unique<RxService>(key.first, key.second, events, receivers));
    }
    const auto setup_deadline = load::Clock::now() + std::chrono::seconds{options.setup_timeout_s};
    while (true)
    {
        const auto connected = static_cast<std::size_t>(
            std::count_if(rx_services.begin(), rx_services.end(), [](const std::unique_ptr<RxService>& service) {
                return service->Connected() || service->Connect();
            }));
        const auto seen = receivers.StreamsSeen();
        if ((connected == rx_services.size()) && (seen == rx_stream_count))
        {
            break;
        }
        if (load::Clock::now() >= setup_deadline)
        {
            Print("result: FAILED, setup timed out with " + std::to_string(connected) + "/" +
                  std::to_string(rx_services.size()) + " RX services connected and " + std::to_string(seen) + "/" +
                  std::to_string(rx_stream_count) + " RX streams received");
            stop_sender();
            return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(kDiscoveryPollInterval);
    }
    Print("setup done: " + std::to_string(tx_indices.size()) + " TX and " + std::to_string(rx_stream_count) +
          " RX streams");

    std::this_thread::sleep_for(std::chrono::seconds{options.warmup_s});
    const auto cpu_window = [&](const std::string& name, double scale, std::chrono::seconds duration) {
        const auto start = sampler.Sample();
        const auto start_ns = load::NowNs();
        const auto tx = tx_total.load();
        const auto rx = receivers.Received();
        std::this_thread::sleep_for(duration);
        const auto end = sampler.Sample();
        const auto window_ns = load::NowNs() - start_ns;
        if (sampler.Supported())
        {
            std::cout << cpu::CpuResult(options.label,
                                        name,
                                        scale,
                                        window_ns,
                                        tx_total.load() - tx,
                                        receivers.Received() - rx,
                                        start,
                                        end)
                      << std::endl;
        }
    };
    // CPU sweep: TX and (via the peer) RX rates scaled by each factor; the main window below runs at full load.
    for (std::uint32_t index = 1U; index < load_phases.size(); ++index)
    {
        load_phase.store(index);
        std::this_thread::sleep_for(kCpuSettleTime);
        std::ostringstream name;
        name << "scale " << options.cpu_scales[index - 1U];
        cpu_window(name.str(), load_phases[index], std::chrono::seconds{options.cpu_phase_s});
    }
    if (load_phases.size() > 1U)
    {
        load_phase.store(0U);
        std::this_thread::sleep_for(kCpuSettleTime);
    }
    phase.store(Phase::kMeasure);
    cpu_window("load", options.scale, std::chrono::seconds{options.duration_s});
    // Keeps sending unmeasured samples, so the peer receives the measured ones still in flight.
    phase.store(Phase::kDrain);
    std::this_thread::sleep_for(kDrainTime);
    stop_sender();

    for (const auto& item : workload.classes)
    {
        const auto stream = std::find_if(workload.streams.begin(), workload.streams.end(), [&item](const auto& entry) {
            return entry.event_class == item.index;
        });
        if (stream == workload.streams.end())
        {
            continue;
        }
        load::ClassResult result{options.label, "sd", item, *stream, options.scale};
        if (item.tx)
        {
            result.AddSent(sent[item.index]);
        }
        else
        {
            receivers.Report(result, item.index);
        }
        std::cout << result.Line() << std::endl;
    }

    rx_services.clear();
    for (const auto& [key, eventgroup] : rx_eventgroups)
    {
        client.Unsubscribe(key.first, key.second, eventgroup);
        client.ReleaseService(key.first, key.second);
    }
    for (auto& service : tx_services)
    {
        client.StopOfferService(service->service, service->instance);
        service->skeleton->StopOfferService();
    }
    return EXIT_SUCCESS;
}
