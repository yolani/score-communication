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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_MODULE_SPIKE_PLUGIN_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_MODULE_SPIKE_PLUGIN_H

/* C interface between the FreeBSD module glue (spike_module.c) and the C++ part (spike_plugin.cpp).
 * Source and primitive values are those of spike_protocol.h. */

#ifdef __cplusplus
extern "C" {
#endif

/* C++ part */
int spike_plugin_start(void);
unsigned int spike_plugin_primitive(void);
unsigned int spike_plugin_source(void);
void spike_plugin_note_signal(void);
/* timed_out: the wait ended by its timeout, not by a signal. */
void spike_plugin_note_wake(int timed_out);
void spike_plugin_note_echo(void);
void spike_semaphore_post(void);
/* Returns 1 if signaled, 0 on timeout. */
int spike_semaphore_wait(unsigned int timeout_ms);

/* C part: wakes the worker with the current primitive; callable from every source context. */
void spike_signal(void);

#ifdef __cplusplus
}
#endif

#endif /* SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_WAKEUP_SPIKE_MODULE_SPIKE_PLUGIN_H */
