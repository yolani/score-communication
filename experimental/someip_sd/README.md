<!--
*******************************************************************************
Copyright (c) 2026 Contributors to the Eclipse Foundation

See the NOTICE file(s) distributed with this work for additional
information regarding copyright ownership.

This program and the accompanying materials are made available under the
terms of the Apache License Version 2.0 which is available at
https://www.apache.org/licenses/LICENSE-2.0

SPDX-License-Identifier: Apache-2.0
*******************************************************************************
-->

# Experimental SOME/IP service discovery prototype

SOME/IP-SD only daemon based on the `ServiceManager` of [lg-someip](https://github.com/LGE-OSS/lg-someip),
exposed to local clients via `score::message_passing`. Not part of the product.

| Target | Description |
| --- | --- |
| `:someip_sd_daemon` | SD-only daemon (lg-someip `ServiceManager` with an injected `SdOnlyRouter`) |
| `:someip_event_module` | User-space stand-in for the io-sock event module: SOME/IP notifications (UDP) to LoLa events and back |
| `iosock_event_module:mods_someip_event` | QNX io-sock event module (`mods-someip-event.so`), with kernel UDP sockets and MEXTADD-backed TX |
| `:sd_client` | Client library for the daemon protocol (`protocol/sd_protocol.h`) |
| `:someip_sd_test_client` | Requests a service, subscribes an eventgroup and prints the events received via LoLa |
| `:someip_sd_test_provider` | Offers `0x2345/0x0001` via the SD daemon and publishes event `0x8001` as LoLa samples |
| `test:someip_sd_peer` | Remote provider on the upstream lg-someip stack, offers `0x1234/0x5678` continuously |
| `test:someip_sd_peer_consumer` | Remote consumer on the upstream lg-someip stack, subscribes `0x2345/0x0001` |
| `@lg_someip//:someipd` | Upstream lg-someip routing daemon, required by the `test:someip_sd_peer*` applications |
| `test:someip_sd_dual_qemu_test` | Integration test on two QNX QEMU VMs (see below) |
| `test:someip_sd_docker_test` | Same integration test on two Linux Docker containers |
| `iosock_poc:iosock_poc_test` | QNX only: LoLa and `score::message_passing` inside an io-sock module (see below) |
| `iosock_event_module:kernel_transport_test` | QNX only: direct kernel transport, routing, payload boundaries, fan-out and LoLa slot reuse |
| `test:scenario_lifecycle_test` | Host-side platform selection, module readiness and scenario cleanup checks |

## Prototyp target architecture overview
![Architecture diagram](doc/prototype.svg)

## Architecture (stage 3)

```mermaid
flowchart LR
    subgraph SD["SD side (10.0.3.1)"]
        client["someip_sd_test_client<br/>(sd_client + GenericProxy)"]
        provider["someip_sd_test_provider<br/>(sd_client + GenericSkeleton)"]
        subgraph daemon["someip_sd_daemon"]
            facade["SdFacade<br/>message_passing server"]
            sm["lg-someip ServiceManager<br/>(SD state machine)"]
            router["SdOnlyRouter<br/>multiplexer thread, timers,<br/>SD UDP endpoints"]
        end
        module["someip_event_module<br/>RX: UDP 30600, GenericSkeleton<br/>TX: GenericProxy, UDP 30620"]
    end

    subgraph Peer["Peer side (10.0.3.2), lg-someip stack"]
        someipd["someipd<br/>ServiceManager + PacketRouterHost"]
        peer["someip_sd_peer<br/>offers 0x1234.0x5678,<br/>notifies 0x8778"]
        peer_consumer["someip_sd_peer_consumer<br/>subscribes 0x2345.0x0001"]
    end

    client -- "score::message_passing<br/>Register, RequestService, Subscribe /<br/>Availability, SubscriptionState" --> facade
    provider -- "score::message_passing<br/>OfferService, StopOfferService" --> facade
    facade -- "internal SD messages<br/>(Post to multiplexer thread)" --> sm
    sm -- "ServiceRouter API" --> router
    router -- "score::message_passing<br/>AddRoute, AddSubscription,<br/>AddProvidedService, AddRemoteSubscriber, Remove*" --> module
    router -- "SOME/IP-SD, UDP 30491<br/>multicast 224.244.224.245 + unicast" --- someipd
    peer -- "local IPC<br/>(QNX message passing / Unix sockets)" --- someipd
    peer_consumer -- "local IPC" --- someipd
    someipd -- "SOME/IP events 0x8778<br/>UDP 30509 to 30600" --> module
    module -- "LoLa someip/1234/5678<br/>event_8778" --> client
    provider -- "LoLa someip/2345/0001<br/>event_8001" --> module
    module -- "SOME/IP events 0x8001<br/>UDP 30620 to 30630" --> someipd
```

- The daemon only does service discovery. `ServiceManager` runs unchanged; `SdOnlyRouter` replaces lg-someip's
  `PacketRouterHost`, sends and receives the SD messages and serializes all `ServiceManager` calls on its
  multiplexer thread.
- Clients talk to the daemon via `score::message_passing` with the fixed-size messages in
  [protocol/sd_protocol.h](protocol/sd_protocol.h); each connection gets a client id that is used as lg-someip
  application id.
- `SdOnlyRouter` forwards the `ServiceManager` routing callbacks to the event module
  ([protocol/event_module_protocol.h](protocol/event_module_protocol.h)):
  - `add_route` / `remove_route` (remote offer) become `AddRoute` / `RemoveRoute` with the local UDP port, the
    remote UDP endpoint and the events of the service from the lg-someip configuration.
  - `add_subscribe_route` / `remove_subscribe_route` for local clients (SubscribeAck received) become
    `AddSubscription` / `RemoveSubscription` per event.
  - `add_route` for an offer of a local client (`app_id` != 0) becomes `AddProvidedService` with the configured
    UDP port, the major version and the events (fields marked) from the lg-someip configuration;
    `remove_route` (StopOffer, also sent by `SdFacade` when a provider disconnects) removes it again.
  - `add_subscribe_route` / `remove_subscribe_route` for remote subscribers (`app_id` 0, IPv4 UDP endpoint) become
    `AddRemoteSubscriber` / `RemoveRemoteSubscriber` per event. lg-someip repeats `add_subscribe_route` on every
    renewal; the event module ignores known subscribers.
- Remote Subscribe of a provided service: the `ServiceManager` forwards it as internal Subscribe to the providing
  client; `SdFacade` acknowledges it on the client's behalf (internal SubscribeAck), only then the `ServiceManager`
  sends the SubscribeAck and calls `add_subscribe_route`.
- The event module offers one LoLa `GenericSkeleton` per SOME/IP service instance (`someip/<service>/<instance>`)
  with one event per SOME/IP event (`event_<event id>`) while the remote service is available, and forwards
  subscribed notifications. A sample (`EventSample`) carries the serialized payload with its length, session,
  interface version and message type; the payload is copied once from the socket into the LoLa slot.
- The LoLa deployment ([test/config/mw_com_config.json](test/config/mw_com_config.json)) must list every
  service instance and event the event module offers; it is shared by the event module and the consumers.
- TX: for a provided service the event module creates a `GenericProxy` on the provider's LoLa skeleton (same naming,
  same `EventSample` layout, only size and payload used) as soon as it is found, and sends each sample as SOME/IP
  notification (session per event, interface version = configured major) to all remote subscribers. The POSIX module
  uses `sendmsg` with the header and payload taken from the LoLa slot; a receive handler wakes its receive loop.
  The QNX io-sock module uses kernel `sosend` with `MEXTADD` (see below). For fields the latest sample is held and
  sent to every new subscriber. Held samples are released and the events unsubscribed before the proxy is destroyed,
  otherwise LoLa terminates the process.
- Limitations: UDP and IPv4 only, no SOME/IP-TP, no multicast eventgroups, payload up to 1400 bytes, no resync
  after an event module restart (restart the SD daemon as well), skeletons are never recreated, remote subscriptions
  are acknowledged without asking the providing application, no methods.

### Daemon internals

```mermaid
flowchart LR
    client(["Clients<br/>(score::message_passing)"])

    subgraph daemon["someip_sd_daemon"]
        direction LR
        facade["SdFacade<br/>message_passing server,<br/>per-client state<br/>(available, subscriptions)"]
        sm["lg-someip ServiceManager<br/>SD state machine"]
        router["SdOnlyRouter<br/>implements lgsomeip::ServiceRouter<br/>multiplexer thread, task queue,<br/>timers, SD UDP endpoints"]
    end

    net(["Network<br/>SD UDP 30491"])
    module(["someip_event_module<br/>(score::message_passing)"])

    client -- "Register / RequestService / Subscribe / OfferService ..." --> facade
    facade -- "Availability / SubscriptionState (Notify)" --> client

    facade -- "Post(task): work runs on the multiplexer thread" --> router
    facade -- "on_internal_message(MessageSD)<br/>FindService / Subscribe / OfferService / SubscribeAck,<br/>TTL 0 = release / unsubscribe / stop offer<br/>on_disconnected_application(client_id)" --> sm

    router -- "SetInternalMessageHandler callback<br/>(client_id, SD bytes: Offer / SubscribeAck / Subscribe)" --> facade

    router -- "on_external_message(endpoint, MessageSD, reboot)" --> sm
    sm -- "ServiceRouter API (see table)" --> router
    router -- "EventModuleClient<br/>AddRoute / AddSubscription /<br/>AddProvidedService / AddRemoteSubscriber / Remove*" --> module

    router <-- "SD multicast + unicast" --> net
```

`ServiceManager` → `SdOnlyRouter` (`lgsomeip::ServiceRouter`):

| Group | Calls | `SdOnlyRouter` |
| --- | --- | --- |
| Lifecycle | `init`, `start`, `stop` | Sets up the network device, multiplexer, task pipe and SD sockets; starts/stops the multiplexer |
| Infrastructure | `get_multiplexer`, `get_service_discovery_{multicast,unicast}_endpoint`, `make_address` | Returns its own objects |
| Timers (QNX) | `message_passing_set_timer`, `message_passing_kill_timer` | One thread per timer; the handler runs on the multiplexer thread via `Post` |
| SD output | `send_external_sd_message` | Sends via the unicast endpoint (to the target address or the multicast group) |
| | `send_internal_message` | Passed to the `InternalMessageHandler`, i.e. to `SdFacade` |
| Data routing | `add_route`, `remove_route` | Remote offer (`app_id` 0): `AddRoute` / `RemoveRoute` (IPv4 UDP endpoint, local port, configured events); own offer: `AddProvidedService` / `RemoveRoute` |
| | `find_connection` | Always returns 1 (no TCP yet) |
| | `add_subscribe_route`, `remove_subscribe_route` | Local client: `AddSubscription` / `RemoveSubscription`; remote subscriber: `AddRemoteSubscriber` / `RemoveRemoteSubscriber` per event; multicast routes are only logged |
| Other | `set_instance_id`, `check_and_send_magic_cookies`, `reboot_route` | No-op / logged |

Calls into `ServiceManager`:

| Caller | Call | Purpose |
| --- | --- | --- |
| `SdOnlyRouter` | `on_external_message(endpoint, sd, reboot)` | SD message received from the network, filtered (own sender, port) and checked for reboot |
| `SdFacade` | `on_internal_message(sd)` | Client request as internal SD message; the client id is put into the client part of the request id |
| `SdFacade` | `on_disconnected_application(client_id)` | Cleanup after a client disconnected |
| `main` | constructor `(name, config, router)`, `init`, `start`, `stop`, `get_configuration` | Lifecycle |

Threading:

- `SdFacade`'s message_passing callbacks never call the `ServiceManager` directly but `Post` to the multiplexer
  thread, so all `ServiceManager` calls are serialized there, like in lg-someip's own daemon.
- `send_internal_message` is called synchronously from within the `ServiceManager`; `SdFacade` posts it again, so
  `HandleServiceManagerMessage` does not run re-entrantly inside the `ServiceManager`.
- Exception: the `ServiceManager` calls `add_route` from its own worker thread (`SomeipSvcConn`).

## Integration tests

```sh
bazel test //experimental/someip_sd/test:someip_sd_docker_test                            # Linux
bazel test --config=qnx_x86_64 //experimental/someip_sd/test:someip_sd_dual_qemu_test     # QNX
bazel test --config=qnx_x86_64 //experimental/someip_sd/iosock_event_module:kernel_transport_test
bazel test //experimental/someip_sd/test:scenario_lifecycle_test
```

Both run the scenario in [test/someip_sd_scenario.py](test/someip_sd_scenario.py) with the configs in
[test/config](test/config): the SD side (10.0.3.1) runs the event module, `someip_sd_daemon`, the test client and
the test provider; the peer side (10.0.3.2) runs `someipd`, `someip_sd_peer` and `someip_sd_peer_consumer`.
On Linux the event module is the original `someip_event_module` process. QNX packages and loads only
`mods-someip-event.so`, and waits for its readiness log before starting the SD daemon.

- RX: availability, subscription acknowledgement, the LoLa offer of the event module, the events received via LoLa
  and that no events arrive after the client unsubscribed (`--runtime`).
- TX: the peer consumer discovers the provided service, receives 5 notifications with increasing counters (the first
  one is the initial field value), receives nothing after its unsubscribe while the provider keeps publishing, and
  sees the StopOffer when the provider ends; the event module removes the provided service afterwards.
- Background processes write to `/tmp/someip_sd_*.log`; on the QNX serial console their output would interleave
  with the command parsing of the ITF.

- QNX: `dual_qemu` environment with the inter-VM network. Requires the QNX toolchain setup described in the root
  `.bazelrc`. Scenario-owned processes are tracked by PID and stopped on success and failure. The io-sock module
  stays loaded until the isolated VM is destroyed.
- Linux: the ITF Docker container plus a second container started by the test, both attached to a bridge network
  with subnet 10.0.3.0/24 (must not be in use on the host).

## QNX io-sock event module

[iosock_event_module](iosock_event_module) implements the same seven routing commands, LoLa naming, `EventSample`
layout, route/subscriber matching, field caching, session counters and UDP/IPv4 limitations as the POSIX module.
The SD daemon and applications do not need a new control protocol or deployment configuration.

```sh
bazel build --config=qnx_x86_64 //experimental/someip_sd/iosock_event_module:mods_someip_event
# After installing the module and existing configs under /opt/someip_sd:
mount -T io-sock /opt/someip_sd/bin/mods-someip-event.so
slog2info | grep -F "[someip_event]"
# Start the SD daemon after "event task started" appears.
```

- The C glue owns all event-transport sockets: `socreate`, `sosetopt`, `sobind`, `soreceive` and `sosend`.
  The C++ part uses the existing LoLa runtime and QNX `score::message_passing`, not POSIX socket calls for event
  traffic. The separate SD daemon retains its own SD transport.
- Routing commands enter a bounded, backpressured control queue. A single kthread serializes route changes,
  LoLa service discovery, RX and TX. It drains a bounded number of datagrams per port and waits approximately
  10 ms between iterations, so idle or busy RX sockets do not prevent TX or new routing commands from progressing.
- RX obtains a datagram mbuf chain from `soreceive`, reads small SOME/IP headers, then copies each matching
  notification payload **directly from that chain into the allocated LoLa slot**. There is no intermediate
  datagram/payload buffer. Each notification uses only the first eligible route in instance-ID order, with no
  fallback to another LoLa interface if allocation or sending fails. Lookup searches only the packet's service-ID
  range. Concatenated messages and mbuf-chain boundaries are supported.
- TX builds a separate 16-byte header mbuf and attaches the original LoLa payload using **MEXTADD**.
  No LoLa-to-socket payload copy is made. Shared ownership retains the non-copyable LoLa sample for every recipient
  and any cached field value. Each mbuf callback drops only its own reference.
- Samples also retain their proxy owner. Removing a provided service stops new sends and drops its field cache,
  but cannot destroy a proxy that still owns in-flight samples. The last sample is released before the proxy
  unsubscribes and is destroyed. Slot exhaustion applies backpressure rather than creating a payload-copy queue.
- This is not NIC-to-LoLa zero-copy: RX still performs the direct mbuf-to-LoLa copy. MEXTADD lifetime completion
  is not a hardware-transmit completion guarantee, and physical NIC DMA suitability or stack/driver-internal
  copying is not established by the QEMU tests.
- **Lifecycle:** this SDK rejects generic-module detach (`umount` reports `Not supported`; io-sock logs
  `unloading ... not supported`). The event module deliberately rejects unload/quiesce and stays resident until
  io-sock exits. The QNX tests use isolated VMs and do not pretend that VM destruction is module unloading.
  Do not attempt to switch modules in a running stack. As before, there is no SD routing resynchronization after
  an event-module/stack restart.

The focused transport test bypasses SD and drives the unchanged routing protocol directly. It checks source port
and endpoint filtering, invalid/oversized messages, concatenated 1400-byte messages across a large datagram, empty
payloads, wildcard subscriptions, unsubscribe, remove/re-offer, byte-exact TX framing, two-recipient fan-out,
initial-field replay, renewal deduplication, and 64 alternating empty/1400-byte sample publications through a
ten-slot deployment. It also checks that network sample references are released after StopOffer.

## io-sock module feasibility check

[iosock_poc](iosock_poc) checks whether the event module can run inside io-sock with LoLa and
`score::message_passing`. It remains a standalone feasibility test, separate from the event module:

```sh
bazel test --config=qnx_x86_64 //experimental/someip_sd/iosock_poc:iosock_poc_test
```

- `mods-iosock-poc.so` consists of the FreeBSD module glue ([module/io_sock_module.c](iosock_poc/module/io_sock_module.c),
  compiled against the SDP's `usr/include/devs` headers with the flags of `mods.mk`, see
  [iosock_module.bzl](iosock_poc/iosock_module.bzl)) and a C++ part ([module/poc_plugin.cpp](iosock_poc/module/poc_plugin.cpp))
  that only uses QNX / POSIX APIs. On `MOD_LOAD` it starts a regular thread that initializes the mw::com runtime,
  starts a `message_passing` server (reply = request + 1) and offers a LoLa `GenericSkeleton` sending a counter.
  The glue also opens a kernel UDP socket (`socreate`, port 30700) served by a kthread: every datagram
  (`soreceive`) is copied into an allocated LoLa slot. Its payload is attached directly to an mbuf with
  `MEXTADD` and echoed with `sosend`, without a TX `uio` copy. An opaque context owns the `SampleAllocateePtr`
  until the mbuf's external-storage free callback publishes the sample via LoLa. Shutdown waits for pending
  TX contexts before destroying the skeleton.
- The test loads it with `mount -T io-sock`, then a regular process ([consumer/consumer.cpp](iosock_poc/consumer/consumer.cpp))
  sends a request via `message_passing`, receives the LoLa counter, sends 5 UDP datagrams to the module and expects
  the echoes as well as the datagrams via LoLa, checking their contents.
- For application TX, the same process offers a separate LoLa skeleton (`iosock/tx`). The module discovers it
  and subscribes through a `GenericProxy`. The kernel thread takes a sample, attaches its payload to an mbuf
  with `MEXTADD` and sends it to the fixed loopback UDP port 30701. The mbuf context holds the received
  `SamplePtr<void>` until the free callback destroys it; no payload copy is made between LoLa and `sosend`.
  The test sends 10 different payloads through 5 sample slots and verifies every UDP payload, exercising slot reuse.
  This is a transport feasibility test, not SOME/IP framing or SD-driven remote subscriber routing.
- Result in QEMU: all of it works. The worker runs in the io-sock process after io-sock dropped to uid 33; LoLa
  `Allocate` / `Send` can be called from the kthread.
- Findings:
  - io-sock resolves the module's symbols itself; `cc_shared_library` drops the `-lfsnotify` linkopt of baselibs,
    so it is added explicitly (otherwise `inotify_*` are unresolved and loading fails with `Exec format error`).
  - The QNX SDK only forward-declares `struct uio`; the glue mirrors the FreeBSD 13.5 definition for
    `soreceive` / `sosend` with kernel buffers.
  - A `GenericProxy` that outlives `main()` aborts the consumer on exit (rc 134); it must be destroyed before the
    static mw::com runtime objects.
  - `MEXTADD` on ordinary LoLa shared memory works in the QEMU loopback test. This does not establish DMA
    suitability or absence of copies inside the network stack or driver. The free callback signals the end of
    the mbuf reference lifetime, not necessarily hardware transmission completion.
  - Not covered yet: physical NIC transmission, unloading the module and whether the public QNX SDP contains
    `usr/include/devs` (the local setup uses the
    safe-posix-platform SDP). Real-target secpol checks are deferred.

## Configuration

[test/config](test/config) holds the lg-someip configurations of both sides and the mw::com configuration
(`mw_com_config.json`, `logging.json`) of the SD side; the integration tests use them unchanged.

- SD uses port 30491 instead of the standard 30490, so the prototype can run next to an existing SOME/IP stack
  (e.g. `someipd_posix` on the target). Both configurations must use the same SD port and multicast group.
- `unicast` is 10.0.3.1 (SD side) and 10.0.3.2 (peer side), the addresses of the test networks.

## lg-someip

Fetched as `@lg_someip` (see `MODULE.bazel`) with the build file and patch in
[third_party/lg_someip](../../third_party/lg_someip). The build follows the upstream CMake defaults, i.e. on QNX
local IPC and the `ServiceManager` timer use QNX message passing (`ENABLE_QNX_MESSAGE_PASSING`); `SdOnlyRouter`
implements the corresponding timer callbacks.

| Target | Used by |
| --- | --- |
| `@lg_someip//:lgsomeip` | `someip_sd_peer`, `someip_sd_peer_consumer` |
| `@lg_someip//:daemon_core` (`ServiceManager`, `PacketRouterHost`) | `someip_sd_daemon` (injects `SdOnlyRouter`), `someipd` |

`0001-fix-subscribe-ack-multicast-reset.patch` fixes a no-op `multicast == nullptr` in `ServiceManager`, so a
SubscribeEventgroupAck with a unicast endpoint option is no longer treated as multicast (candidate for an upstream
fix). The QNX 8 build issues (`SO_VLANPRIO`, `__progname`) are fixed upstream since `d09656e`.

## Running on a target

On Linux, build for the target, copy `someip_event_module`, `someip_sd_daemon`, `someip_sd_test_client` and
`test/config/{sd_daemon,mw_com_config,logging}.json` to the target (binaries in `bin/`, configs in `etc/`) and set
`unicast` to the target's address on the interface towards the peer. All three processes must run on the same
machine; start the event module first.

On QNX, install `mods-someip-event.so` instead of the POSIX event-module executable and load it as described above.
The module uses `/opt/someip_sd/etc/mw_com_config.json` and logs to slog2 with the prefix `[someip_event]`
(`slog2info | grep -F "[someip_event]"`).
Do not run both event-module implementations in the same stack; they use the same control service and UDP ports.

```sh
./bin/someip_event_module ./etc/mw_com_config.json
./bin/someip_sd_daemon ./etc/sd_daemon.json
./bin/someip_sd_test_client --service 0x1234 --instance 0x5678 --eventgroup 0x4465 --event 0x8778 \
    --mw-com-config ./etc/mw_com_config.json
```

The peer side (`someipd` and `someip_sd_peer` with `test/config/lg_peer.json`, `unicast` set accordingly) runs on another
machine in the same subnet; start `someipd` first, the provider registers with it.

Expected test client output: the service becomes available, the subscription is acknowledged, the LoLa service
`someip/1234/5678` is found and event `0x8778` arrives every second via LoLa with an incrementing counter as payload.
With `--runtime <seconds>` the client unsubscribes afterwards, checks for 3 s that no further events arrive
(`result: OK, no events after unsubscribe`) and exits with 0; without it, it runs until SIGINT/SIGTERM.
