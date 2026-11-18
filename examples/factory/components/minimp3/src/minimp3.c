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

/* minimp3 (CC0, https://github.com/lieff/minimp3), built once for MP3-only decoding. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
/* The factory firmware also links Espressif's esp_audio_codec, whose
 * prebuilt MP3 decoder embeds the same minimp3 implementation — defining
 * mp3dec_init/mp3dec_decode_frame here would collide at link time. Rename
 * our copies; muse_chat_session.cpp applies the same defines before its
 * #include "minimp3.h", so the session calls these. (Sharing the codec's
 * copy was rejected: its embedded minimp3 version is unverifiable from the
 * prebuilt archive, and a struct-layout mismatch would corrupt the decode
 * state. A second ~30 KB decoder in 32 MB of flash is cheap insurance.) */
#define mp3dec_init muse_mp3dec_init
#define mp3dec_decode_frame muse_mp3dec_decode_frame
#include "minimp3.h"
