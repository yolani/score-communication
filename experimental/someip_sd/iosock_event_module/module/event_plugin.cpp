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
#include "experimental/someip_sd/iosock_event_module/module/event_plugin.h"

#include "experimental/someip_sd/iosock_event_module/module/control_channel.h"
#include "experimental/someip_sd/iosock_event_module/module/kernel_sockets.h"
#include "experimental/someip_sd/iosock_event_module/module/log.h"
#include "experimental/someip_sd/iosock_event_module/module/rx_router.h"
#include "experimental/someip_sd/iosock_event_module/module/tx_forwarder.h"
#include "experimental/someip_sd/iosock_event_module/module/types.h"

#include "score/mw/com/runtime.h"
#include "score/mw/com/runtime_configuration.h"

#include <dlfcn.h>

#include <cerrno>
#include <memory>
#include <string>
#include <thread>

namespace
{

using namespace score::someip_sd::iosock_event_module;

using TransportPort = unsigned short;
using TransportByte = unsigned char;
using TransportPacketSize = unsigned int;

/// The module is installed as <prefix>/bin/mods-someip-event.so and reads <prefix>/etc/mw_com_config.json.
std::string ConfigPath()
{
    Dl_info info{};
    std::string path{(dladdr(reinterpret_cast<void*>(&ConfigPath), &info) != 0) ? info.dli_fname : ""};
    const auto bin = path.rfind("/bin/");
    if (bin == std::string::npos)
    {
        return "/opt/someip_sd/etc/mw_com_config.json";
    }
    return path.substr(0U, bin) + "/etc/mw_com_config.json";
}

/// Owns the components and serializes all work on the kernel thread. Only the control channel's command queue
/// is accessed from other threads.
class EventModule
{
  public:
    OperationSucceeded Start()
    {
        return control_.Start();
    }

    void Stop()
    {
        control_.Stop();
    }

    void Poll()
    {
        // Order matters: route and subscriber changes apply before proxies are connected and samples are sent.
        for (const auto& message : control_.TakeCommands())
        {
            HandleCommand(message);
        }
        tx_.ReleaseRetiredProxies();
        tx_.ConnectProvidedServices();
        tx_.ForwardSamples();
    }

    void OnDatagram(Port port, const Ipv4Address& ip, Port remote_port, PacketHandle packet, PacketSize size)
    {
        rx_.OnDatagram(port, ip, remote_port, packet, size);
    }

  private:
    void HandleCommand(const protocol::Message& message);

    ControlChannel control_;
    KernelSockets sockets_;
    RxRouter rx_{sockets_};
    TxForwarder tx_{sockets_};
};

void EventModule::HandleCommand(const protocol::Message& message)
{
    const ServiceInstance key{message.service, message.instance};
    switch (message.opcode)
    {
        case protocol::Opcode::kAddRoute:
            rx_.AddRoute(message);
            break;
        case protocol::Opcode::kRemoveRoute:
            // The SD daemon uses the same command for remote routes and provided services.
            if (!tx_.RemoveProvidedService(key))
            {
                rx_.RemoveRoute(key);
            }
            break;
        case protocol::Opcode::kAddSubscription:
            rx_.AddSubscription(key, message.event);
            break;
        case protocol::Opcode::kRemoveSubscription:
            rx_.RemoveSubscription(key, message.event);
            break;
        case protocol::Opcode::kAddProvidedService:
            tx_.AddProvidedService(message);
            break;
        case protocol::Opcode::kAddRemoteSubscriber:
            tx_.AddRemoteSubscriber(message);
            break;
        case protocol::Opcode::kRemoveRemoteSubscriber:
            tx_.RemoveRemoteSubscriber(message);
            break;
        default:
            someip_log(SOMEIP_LOG_WARNING, "unknown routing command dropped");
            break;
    }
}

std::unique_ptr<EventModule> module;

}  // namespace

extern "C" ErrorCode someip_event_start(void)
{
    OperationSucceeded started{false};
    const auto config = ConfigPath();
    someip_log(SOMEIP_LOG_INFO, "LoLa configuration %s", config.c_str());
    std::thread initialize{[&started, &config]() {
        score::mw::com::runtime::InitializeRuntime(
            score::mw::com::runtime::RuntimeConfiguration{score::filesystem::Path{config}});
        module = std::make_unique<EventModule>();
        started = module->Start();
        if (!started)
        {
            module.reset();
        }
    }};
    initialize.join();
    return started ? 0 : EIO;
}

extern "C" void someip_event_abort_start(void)
{
    module->Stop();
    module.reset();
    someip_log(SOMEIP_LOG_ERROR, "kernel thread creation failed; control endpoint stopped");
}

extern "C" void someip_event_poll(void)
{
    module->Poll();
}

extern "C" void someip_event_datagram(TransportPort local_port,
                                      const TransportByte sender_ip[4],
                                      TransportPort sender_port,
                                      PacketHandle packet,
                                      TransportPacketSize size)
{
    const Ipv4Address ip{sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3]};
    module->OnDatagram(local_port, ip, sender_port, packet, size);
}

extern "C" void someip_event_release_tx(TxContextHandle context)
{
    TxForwarder::ReleaseTxContext(context);
}
