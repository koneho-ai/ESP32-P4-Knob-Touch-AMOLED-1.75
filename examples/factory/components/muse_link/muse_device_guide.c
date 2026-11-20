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

// The device guide Link sends to Muse (device.guide, via
// noise_control.cpp's send_device_guide). The SDK defines this symbol from
// main/guide.md through its CMakeLists; the factory defines it here as a
// string literal instead. Adapted from the SDK's guide.md for the P4 Knob:
// no discover/sensors/ble.*/camera (not advertised), recovery through the
// settings page's sign-out (this board has no 5-second button hold), and
// Link runs only while the Muse app is on screen. Keep it under 4,096
// bytes: link.register's guide is capped there.
//
// UNTESTED ON HARDWARE — no board exists yet; compile verification only.

const char muse_device_guide_md[] =
    "# Muse on the Waveshare ESP32-P4 Knob\n"
    "\n"
    "An ESP32-P4 knob board running Muse as an app in the factory firmware,\n"
    "connected to Muse over Wi-Fi. Muse uses it through the commands it\n"
    "lists. Link runs only while the Muse app is on screen: open the app to\n"
    "connect, and commands stop working when another app takes the screen.\n"
    "\n"
    "## What it can do\n"
    "\n"
    "- It has a round 466x466 colour screen Muse can draw on (`display.*`).\n"
    "  An image stays up until `display.show_animation`, a tap, or a talk;\n"
    "  bring the avatar back when the user is done. Keep the subject in the\n"
    "  middle: the round screen cuts off the corners.\n"
    "- `voice.configure` sets the speaker volume, 0 to 100.\n"
    "- `wakeword.disable` turns off hands-free \"Hey Muse\" listening at\n"
    "  once. Only someone at the device can turn it back on.\n"
    "- `device.health` covers its network, uptime and memory.\n"
    "\n"
    "## Status\n"
    "\n"
    "The Muse app shows the state on screen: Connected, Reconnecting, or\n"
    "Error. While another app is on screen, Link is down and the device\n"
    "does not answer commands.\n"
    "\n"
    "## Recovery\n"
    "\n"
    "- If it stops responding, unplug it and plug it back in.\n"
    "- To unpair it, use Sign out in the Muse app's settings page, then set\n"
    "  it up again in the Muse app.\n"
    "\n"
    "## Limits\n"
    "\n"
    "- It has no shell or files Muse can reach: it does only what its\n"
    "  commands offer.\n"
    "- Commands work only while the Muse app is on screen and connected.\n"
    "- It is not a bridge to the home network: no device discovery, no\n"
    "  tunnel, no sensors, no Bluetooth commands, no camera.\n";
