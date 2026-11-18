/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/*
 * Slim factory version of the SDK's muse_settings.h (see VENDORED.md).
 *
 * The SDK's settings store (volume, speaker, Wi-Fi, Hatch token, ...) is
 * replaced by the Muse app's own NVS keys in the muse_vm component. The
 * session core only reads the four Hatch getters below; the values come
 * from muse_vm's RAM cache (the session task's stack lives in PSRAM, where
 * reading flash would crash — see muse_credentials.h). Setters live in
 * muse_vm (muse_provisioning.h / muse_credentials.h), not here.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_HOST_MAX 63
#define MUSE_VM_MAX 63
#define MUSE_TOKEN_MAX 1023

/* The Noise host override (empty: the session uses hatch.metaaivm.com). */
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]);
/* The preferred VM id (empty: the account's default VM). */
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]);
/* The device access token. Never log it. */
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_settings_hatch_token_len(void);

#ifdef __cplusplus
}
#endif
