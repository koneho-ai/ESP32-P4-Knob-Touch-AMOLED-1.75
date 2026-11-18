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
 * Slim factory version of the SDK's muse_wifi.h (see VENDORED.md).
 *
 * The SDK drives its own Wi-Fi station from its settings store. Here Wi-Fi
 * belongs to the factory wifi_service (join/scan/persist/reconnect, with
 * Settings owning the UI). The session core only asks whether Wi-Fi is up.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when the factory Wi-Fi service reports CONNECTED (has an IP). */
bool muse_wifi_connected(void);

#ifdef __cplusplus
}
#endif
