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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_WAKEUP_SIGNAL_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_EVENT_MODULE_WAKEUP_SIGNAL_H

/* Wakes the module's kernel thread. A POSIX semaphore is the only primitive that works from socket upcalls, LoLa
 * and message_passing threads alike: taskqueue_enqueue() and wakeup() hang io-sock when called from non-kernel
 * threads (see experimental/someip_sd/iosock_wakeup_spike). */

#ifdef __cplusplus
extern "C" {
#endif

/* Reasons for a wakeup, combined as bit mask. */
#define SOMEIP_WAKEUP_CONTROL 0x1U
#define SOMEIP_WAKEUP_TX 0x2U
#define SOMEIP_WAKEUP_RX 0x4U

/* Returns 0 on success. */
int someip_wakeup_init(void);
/* Any thread or context; only atomics and sem_post(). */
void someip_wakeup_signal(unsigned int reasons);
/* Kernel thread only. Returns the reasons signaled since the previous call, 0 if timeout_ms elapsed without. */
unsigned int someip_wakeup_wait(unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
