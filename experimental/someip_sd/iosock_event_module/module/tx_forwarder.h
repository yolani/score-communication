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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_TX_FORWARDER_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_TX_FORWARDER_H

#include "experimental/someip_sd/iosock_event_module/module/kernel_sockets.h"
#include "experimental/someip_sd/iosock_event_module/module/types.h"

#include "score/mw/com/types.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace score::someip_sd::iosock_event_module
{

/// TX direction: LoLa services offered by local applications are consumed via generic proxies and their samples
/// are sent as SOME/IP notifications to remote subscribers, attaching the LoLa payload to the mbuf (MEXTADD).
class TxForwarder
{
  public:
    explicit TxForwarder(KernelSockets& sockets) : sockets_{sockets} {}

    void AddProvidedService(const protocol::Message& message);
    /// Returns false if the service instance is not provided by this module.
    OperationSucceeded RemoveProvidedService(const ServiceInstance& key);
    void AddRemoteSubscriber(const protocol::Message& message);
    void RemoveRemoteSubscriber(const protocol::Message& message);

    /// Drops proxies of removed services once the network no longer references any of their samples.
    void ReleaseRetiredProxies();
    void ConnectProvidedServices();
    void ForwardSamples();

    /// Releases the sample reference of one sent mbuf; called from the mbuf free callback.
    static void ReleaseTxContext(TxContextHandle context);

  private:
    using FieldEvent = bool;

    struct SubscribedEvent
    {
        score::mw::com::GenericProxyEvent* event{nullptr};
        // Set by the LoLa receive handler; starts set to forward samples that arrived before it was installed.
        std::atomic<bool> pending{true};
    };

    struct ProxyOwner
    {
        std::unique_ptr<score::mw::com::GenericProxy> proxy;
        // Map nodes keep their address, which the receive handlers capture.
        std::map<EventId, SubscribedEvent> subscribed;

        ~ProxyOwner();
    };

    struct SampleLease
    {
        // Members are destroyed in reverse order: release the sample before the last proxy owner.
        std::shared_ptr<ProxyOwner> owner;
        score::mw::com::SamplePtr<void> sample;
    };

    struct TxContext
    {
        std::shared_ptr<SampleLease> lease;
    };

    struct ProvidedEvent
    {
        FieldEvent is_field{false};
        SubscribedEvent* subscription{nullptr};
        std::set<Endpoint> subscribers;
        SessionId session{0U};
        NotificationCount sent{0U};
        std::shared_ptr<SampleLease> last_value;
    };

    struct ProvidedService
    {
        Port port{0U};
        InterfaceVersion major{0U};
        // Set by the LoLa find service handler, which may outlive this entry.
        std::shared_ptr<std::atomic<bool>> provider_changed;
        std::optional<score::mw::com::FindServiceHandle> find_handle;
        std::shared_ptr<ProxyOwner> owner;
        std::map<EventId, ProvidedEvent> events;
    };

    std::shared_ptr<ProxyOwner> CreateProxyOwner(const ServiceInstance& key, ProvidedService& service);
    void SendNotification(const ServiceInstance& key,
                          ProvidedService& service,
                          EventId event_id,
                          ProvidedEvent& event,
                          const std::shared_ptr<SampleLease>& sample,
                          const std::set<Endpoint>& receivers);

    KernelSockets& sockets_;
    std::map<ServiceInstance, ProvidedService> provided_;
    std::map<ServiceInstance, std::shared_ptr<ProxyOwner>> retired_;
};

}  // namespace score::someip_sd::iosock_event_module

#endif
