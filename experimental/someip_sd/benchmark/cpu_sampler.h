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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_CPU_SAMPLER_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_CPU_SAMPLER_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__QNX__)
#include <devctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <unistd.h>

#include <climits>
#include <cstring>
#include <fstream>
#endif

/// CPU time per thread from QNX procfs (DCMD_PROC_TIDSTATUS, system + user time in ns), grouped as:
/// "module" (the event module's thread in io-sock), "io-sock" (all other io-sock threads: stack, drivers, timers),
/// "idle" (the idle threads of procnto, one per CPU) and the name of every other sampled process.
/// The module thread is identified by SOMEIP_EVENT_MODULE_TID (logged by the module at start) or its thread name.
/// Not supported on other systems: Supported() returns false and Sample() is empty.
namespace score::someip_sd::benchmark::cpu
{

using Nanoseconds = std::int64_t;

struct ThreadCpu
{
    std::string group;
    std::string name;
    std::int32_t pid{0};
    std::int32_t tid{0};
    std::uint64_t cpu_ns{0U};
};

using Key = std::pair<std::int32_t, std::int32_t>;
using Snapshot = std::map<Key, ThreadCpu>;

constexpr char kModuleThreadName[] = "someip_event";

class CpuSampler
{
  public:
    /// processes: executable names (without path) of the sampled processes; the calling process is always sampled.
    explicit CpuSampler(const std::vector<std::string>& processes)
    {
#if defined(__QNX__)
        const char* const module_tid = std::getenv("SOMEIP_EVENT_MODULE_TID");
        module_tid_ = (module_tid != nullptr) ? std::atoi(module_tid) : 0;
        own_pid_ = static_cast<std::int32_t>(getpid());
        DIR* const proc = opendir("/proc");
        if (proc == nullptr)
        {
            return;
        }
        while (const dirent* const entry = readdir(proc))
        {
            const auto pid = std::atoi(entry->d_name);
            if (pid <= 0)
            {
                continue;
            }
            const auto name = ProcessName(pid);
            const bool wanted = (pid == 1) || (pid == own_pid_) ||
                                (std::find(processes.begin(), processes.end(), name) != processes.end());
            if (wanted)
            {
                processes_.emplace_back(pid, name);
            }
        }
        closedir(proc);
#else
        static_cast<void>(processes);
#endif
    }

    bool Supported() const
    {
        return !processes_.empty();
    }

    /// "pid name, ..." of the sampled processes.
    std::string Processes() const
    {
        std::string text;
        for (const auto& [pid, name] : processes_)
        {
            text += (text.empty() ? "" : ", ") + std::to_string(pid) + " " + name;
        }
        return text;
    }

    Snapshot Sample() const
    {
        Snapshot snapshot;
#if defined(__QNX__)
        for (const auto& [pid, process] : processes_)
        {
            SampleProcess(pid, process, snapshot);
        }
#endif
        return snapshot;
    }

    /// Busy-waits for duration on the calling thread; returns the CPU time procfs accounted for it.
    Nanoseconds CheckAccounting(std::chrono::milliseconds duration) const
    {
#if defined(__QNX__)
        const Key self{own_pid_, static_cast<std::int32_t>(pthread_self())};
        const auto before = Sample();
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end)
        {
        }
        const auto after = Sample();
        if ((before.count(self) == 0U) || (after.count(self) == 0U))
        {
            return 0;
        }
        return static_cast<Nanoseconds>(after.at(self).cpu_ns - before.at(self).cpu_ns);
#else
        static_cast<void>(duration);
        return 0;
#endif
    }

  private:
#if defined(__QNX__)
    static std::string ProcessName(std::int32_t pid)
    {
        std::string path;
        const int fd = open(("/proc/" + std::to_string(pid) + "/as").c_str(), O_RDONLY);
        if (fd >= 0)
        {
            struct
            {
                procfs_debuginfo info;
                char buffer[PATH_MAX];
            } map{};
            if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), nullptr) == EOK)
            {
                path = map.info.path;
            }
            close(fd);
        }
        if (path.empty())
        {
            std::ifstream file{"/proc/" + std::to_string(pid) + "/exefile"};
            std::getline(file, path);
        }
        const auto slash = path.rfind('/');
        return (slash == std::string::npos) ? path : path.substr(slash + 1U);
    }

    static std::string ThreadName(int fd, std::int32_t tid)
    {
        procfs_threadctl control{};
        control.tid = static_cast<std::uint32_t>(tid);
        control.cmd = _NTO_TCTL_NAME;
        auto* const name = reinterpret_cast<struct _thread_name*>(control.data);
        name->new_name_len = -1;
        name->name_buf_len = static_cast<int>(sizeof(control.data) - sizeof(struct _thread_name));
        if (devctl(fd, DCMD_PROC_THREADCTL, &control, sizeof(control), nullptr) != EOK)
        {
            return {};
        }
        return std::string{name->name_buf, strnlen(name->name_buf, static_cast<std::size_t>(name->name_buf_len))};
    }

    void SampleProcess(std::int32_t pid, const std::string& process, Snapshot& snapshot) const
    {
        const int fd = open(("/proc/" + std::to_string(pid) + "/as").c_str(), O_RDONLY);
        if (fd < 0)
        {
            return;
        }
        procfs_status status{};
        status.tid = 1;
        while (devctl(fd, DCMD_PROC_TIDSTATUS, &status, sizeof(status), nullptr) == EOK)
        {
            const auto tid = static_cast<std::int32_t>(status.tid);
            ThreadCpu thread{};
            thread.pid = pid;
            thread.tid = tid;
            thread.cpu_ns = status.sutime;
            const auto known = names_.find(Key{pid, tid});
            thread.name = (known != names_.end()) ? known->second : (names_[Key{pid, tid}] = ThreadName(fd, tid));
            if (pid == 1)
            {
                // procnto: only its idle threads (priority 0) are of interest.
                if (status.priority != 0U)
                {
                    status.tid = tid + 1;
                    continue;
                }
                thread.group = "idle";
            }
            else if (process == "io-sock")
            {
                const bool module = (module_tid_ > 0) ? (tid == module_tid_) : (thread.name == kModuleThreadName);
                thread.group = module ? "module" : "io-sock";
            }
            else
            {
                thread.group = process;
            }
            snapshot[Key{pid, tid}] = thread;
            status.tid = tid + 1;
        }
        close(fd);
    }

    std::int32_t module_tid_{0};
    std::int32_t own_pid_{0};
#endif
    std::vector<std::pair<std::int32_t, std::string>> processes_;
    mutable std::map<Key, std::string> names_;
};

/// "CPU_RESULT {json}" for one window: CPU time per group and per io-sock thread, in ms and % of one CPU.
inline std::string CpuResult(const std::string& label,
                             const std::string& phase,
                             double scale,
                             Nanoseconds window_ns,
                             std::uint64_t tx_events,
                             std::uint64_t rx_events,
                             const Snapshot& start,
                             const Snapshot& end)
{
    std::map<std::string, std::uint64_t> groups;
    std::map<std::string, std::uint64_t> threads_per_group;
    std::ostringstream threads;
    const auto percent = [window_ns](std::uint64_t cpu_ns) {
        return 100.0 * static_cast<double>(cpu_ns) / static_cast<double>(window_ns);
    };
    bool first{true};
    for (const auto& [key, thread] : end)
    {
        const auto before = start.find(key);
        // Threads created within the window count completely.
        const auto delta = thread.cpu_ns - ((before != start.end()) ? before->second.cpu_ns : 0U);
        groups[thread.group] += delta;
        ++threads_per_group[thread.group];
        if (((thread.group == "module") || (thread.group == "io-sock")) && (delta > 0U))
        {
            threads << (first ? "" : ",") << "{\"group\":\"" << thread.group << "\",\"tid\":" << thread.tid
                    << ",\"name\":\"" << thread.name << "\",\"cpu_ms\":" << (static_cast<double>(delta) / 1.0e6)
                    << ",\"pct\":" << percent(delta) << "}";
            first = false;
        }
    }
    std::ostringstream json;
    json << "CPU_RESULT {\"label\":\"" << label << "\",\"phase\":\"" << phase << "\",\"scale\":" << scale
         << ",\"window_s\":" << (static_cast<double>(window_ns) / 1.0e9) << ",\"tx_events\":" << tx_events
         << ",\"rx_events\":" << rx_events << ",\"cpus\":" << threads_per_group["idle"] << ",\"groups\":{";
    first = true;
    for (const auto& [group, cpu_ns] : groups)
    {
        json << (first ? "" : ",") << "\"" << group << "\":{\"cpu_ms\":" << (static_cast<double>(cpu_ns) / 1.0e6)
             << ",\"pct\":" << percent(cpu_ns) << ",\"threads\":" << threads_per_group[group] << "}";
        first = false;
    }
    json << "},\"threads\":[" << threads.str() << "]}";
    return json.str();
}

}  // namespace score::someip_sd::benchmark::cpu

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_BENCHMARK_CPU_SAMPLER_H
