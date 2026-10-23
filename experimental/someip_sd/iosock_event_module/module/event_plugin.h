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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_EVENT_PLUGIN_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_EVENT_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

int someip_event_start(void);
void someip_event_abort_start(void);
void someip_event_ready(void);
void someip_event_poll(void);
void someip_event_datagram(unsigned short local_port,
                           const unsigned char sender_ip[4],
                           unsigned short sender_port,
                           const void* packet,
                           unsigned int size);
void someip_event_release_tx(void* context);

//TODO: properly split C and C++ interfaces
/* Transport functions run only on the module's kernel thread. */
int someip_event_open_socket(unsigned short port);
void someip_event_copy_packet(const void* packet, unsigned int offset, unsigned int size, void* destination);
/* Always consumes context, including on failure. Header is copied; payload is attached with MEXTADD. */
int someip_event_send(unsigned short local_port,
                      const unsigned char remote_ip[4],
                      unsigned short remote_port,
                      const unsigned char header[16],
                      const unsigned char* payload,
                      unsigned int size,
                      void* context);

#ifdef __cplusplus
}
#endif

#endif
