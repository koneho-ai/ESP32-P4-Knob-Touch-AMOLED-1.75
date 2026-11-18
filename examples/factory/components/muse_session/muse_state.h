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
 * Slim factory version of the SDK's muse_state.h (see VENDORED.md).
 *
 * The SDK's UI state (mode, caption, level, progress, ...) lives in the
 * Muse app's own state machine and MuseUi here. The vendored chat code
 * only needs the caption page size, which muse_factory_glue.c answers
 * from the Muse UI's reply-label geometry.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* The reply caption's page: how many characters across, how many lines. */
void muse_state_page(const char *text, int *cols, int *lines);

#ifdef __cplusplus
}
#endif
