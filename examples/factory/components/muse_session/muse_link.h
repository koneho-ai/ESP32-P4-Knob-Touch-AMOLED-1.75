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
 * Slim factory version of the SDK's muse_link.h (see VENDORED.md).
 *
 * In the SDK, Muse runs inside Home Link, which owns the radios, the setup
 * credentials and the Hatch account, and registers an ops table at boot.
 * This firmware has no Link: the Muse app owns its own device token (see
 * muse_vm), Wi-Fi belongs to the factory wifi_service, and there is no
 * Link Noise session to borrow. The session core only ever calls the two
 * Hatch-account hooks below, and only on the fallback path when the app has
 * no token of its own — so both are stubs that report "no Link". The
 * session's own-token path (muse_settings_hatch_token + fetch_vms) is the
 * only one that runs here.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Paired to a Muse account through Link — always false here. */
bool muse_link_hatch_linked(void);
/* Fills the VM from Link's account — always false here (no Link). */
bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name,
                        size_t name_cap, char **vm_token);

#ifdef __cplusplus
}
#endif
