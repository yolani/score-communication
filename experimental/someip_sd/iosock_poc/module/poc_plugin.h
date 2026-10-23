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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_MODULE_POC_PLUGIN_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_MODULE_POC_PLUGIN_H

/* C interface between the FreeBSD module glue (C, kernel headers) and the C++ part (POSIX, mw::com). */

#ifdef __cplusplus
extern "C" {
#endif

/* Called on MOD_LOAD; starts a worker thread and returns immediately. Returns 0 on success. */
int iosock_poc_start(void);

/* Called on MOD_UNLOAD / MOD_SHUTDOWN / MOD_QUIESCE; stops and joins the worker thread. */
void iosock_poc_stop(void);

void* iosock_poc_prepare_tx(const unsigned char* data, unsigned int size, const unsigned char** payload);
void iosock_poc_release_tx(void* context);
void* iosock_poc_take_app_tx(const unsigned char** payload, unsigned int* size);
void iosock_poc_release_app_tx(void* context);

#ifdef __cplusplus
}
#endif

#endif /* SCORE_EXPERIMENTAL_SOMEIP_SD_IOSOCK_POC_MODULE_POC_PLUGIN_H */
