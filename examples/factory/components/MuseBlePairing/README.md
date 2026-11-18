# Muse BLE pairing for the P4 knob factory firmware (Phase 2.1)

> **UNTESTED ON HARDWARE.** No P4 knob board exists yet. Compile
> verification only.

A Bluedroid BLE GATT server implementing the Muse gadget SDK's **community
pairing v5** service, so the existing Muse app can pair with the Waveshare
ESP32-P4-Knob-Touch-AMOLED-1.75 factory firmware unmodified — while BT
Music (Classic A2DP over Bluedroid) keeps working. The SDK's pairing
server is NimBLE, which cannot coexist with the factory's Bluedroid host,
so the server was ported to Bluedroid; the wire protocol (UUIDs,
advertising, chunk framing, pairing-v5 ECDH/HKDF/AES-GCM records) is
byte-identical to the SDK's.

## Contents

- `MuseBlePairing/` — the component. Copy it to
  `examples/factory/components/MuseBlePairing/`.
  - `muse_ble_pairing.c` — Bluedroid GATT server + command dispatch (new code)
  - `include/muse_ble_pairing.h` — public API for the Muse app
  - `include/muse_provisioning.h` — NVS-store contract (implemented by MuseApp)
  - `vendored/` — pairing-v5 crypto from the SDK (see `VENDORED.md`)
  - `CMakeLists.txt`, `idf_component.yml`
- `INTEGRATION.md` — sdkconfig changes, registration, MuseApp call flow,
  bt_service coexistence contract, protocol reference, and everything
  that could NOT be verified without hardware.
- `VENDORED.md` — what was vendored from the SDK, from which commit,
  and what changed.

## In short

```c
/* After bt_service_init(), once: */
muse_ble_pairing_init(&cfg);          /* cfg: identity + callbacks */
/* When the user opens the pairing screen: */
muse_ble_pairing_start_advertising();
/* on_confirm_required -> show prompt -> arm -> user confirms: */
muse_ble_pairing_arm_confirmation(generation);
muse_ble_pairing_confirm();
/* on_provision -> join Wi-Fi, store tokens, then: */
muse_ble_pairing_send_pairing_status("auth_ok", generation);
muse_ble_pairing_stop_advertising(true);
muse_ble_pairing_set_provisioned(true);
```

See `INTEGRATION.md` for the full story, including the 15-item
unverified list — headlined by whether the ESP32 coprocessor firmware
includes a BLE-capable controller.
