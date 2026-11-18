# VENDORED.md — SDK code vendored into MuseBlePairing

Source repo: `muse-gadget-sdk` (fork `koneho-ai/muse-gadget-sdk`), commit
`6a44074` ("Add Sony Bravia TVs skill (#434)"), read 2026-11-18.
License: Apache-2.0 (Meta Platforms, Inc.). Original license headers are
preserved in every file; each file carries an additional `VENDORED` note
recording its origin and what changed.

## Vendored files (in `MuseBlePairing/vendored/`)

| File | SDK origin | Changes |
|---|---|---|
| `link_pairing.c` (905 lines, was 1075) | `esp32/main/link_pairing.c` | Community-only adaptation — see below. Wire protocol untouched. |
| `link_pairing.h` | `esp32/main/link_pairing.h` | Removed `link_pairing_sign_factory_test()` (eFuse-only). Rest identical. |
| `pairing_transcript.c` | `esp32/main/pairing_transcript.c` | **Verbatim** (only the VENDORED note added). |
| `pairing_transcript.h` | `esp32/main/pairing_transcript.h` | **Verbatim** (only the VENDORED note added). |

## What changed in `link_pairing.c` (and why)

The SDK supports two pairing modes: manufacturer attestation (a fleet
ECDSA key in a protected eFuse block) and community pairing (no
attestation, `pairing_auth: "none"`). This board has no provisioned
pairing eFuse key, and the Muse app pairs community devices with auth
`"none"` — so the eFuse path was removed:

- Deleted `resolve_pairing_signer()`, `pairing_signer()`,
  `hardware_sign_hash()`, `sign_transcript_ecdsa()`, and
  `link_pairing_sign_factory_test()`.
- Replaced the signer machinery with constants: `pairing_is_community()`
  → true, `pairing_auth_epoch()` → 0, `pairing_auth()` →
  `PAIRING_COMMUNITY_AUTH` (`"none"`). This is exactly what the SDK
  yields on a community board.
- Removed the `device_proof` signing block in
  `link_pairing_handle_client_hello()` and the conditional `device_proof`
  field in the `pairing_ready` reply (the SDK omits it for community
  devices too).
- Removed includes: `psa/crypto.h`,
  `psa_crypto_driver_esp_ecdsa*.h`, `esp_efuse.h`,
  `pairing_signer_policy.h`.
- Removed the `s_signer` state variable and the
  `pairing_signer() == PAIRING_SIGNER_UNAVAILABLE` hello check.

**Untouched:** the transcript construction, ECDH/HKDF key derivation,
AES-GCM record encryption/decryption (including nonce/AAD construction),
the session state machine, all timeouts, all error strings, the
generation/token scheme, and the `pairing_confirmed` SDK-token behavior.
The `s_device_proof_b64` buffer is still declared and zeroed (dead
storage, kept so `clear_session_locked()` stays identical to the SDK's).

## What was deliberately NOT vendored

| SDK file | Why not |
|---|---|
| `esp32/main/ble_server.c` | Ported, not vendored: rewritten as `muse_ble_pairing.c` on Bluedroid (NimBLE cannot coexist with the factory's Bluedroid host). Wire behavior kept identical; see INTEGRATION.md §4 and the deviation notes in the file header. |
| `esp32/main/pairing_signer_policy.c` | Only used by the removed eFuse path. |
| `esp32/main/stack_monitor.h`, `helper_task.h` | SDK task-health helpers; this component uses plain `xTaskCreate`/`vTaskDelete`. |
| `esp32/main/factory_test.{c,h}` | SDK factory-test GATT service; not part of pairing. |
| `esp32/main/ble_basic.{c,h}` | The "companion" phone-setup service; out of scope for a voice app. |
| `esp32/main/config_store.c`, `identity.c`, `vm_api.c`, `app.c` | App-side concerns (NVS store, identity derivation, token mint, provisioning flow). They belong to the MuseApp component (Phases 1–2), behind the `muse_provisioning.h` contract. |

## Updating

If the SDK's pairing protocol changes (new `PAIRING_VERSION`, new record
format), re-vendor `link_pairing.c` + `pairing_transcript.*` from the new
SDK commit, re-apply the community-only adaptation above, and update the
commit hash recorded here and in each file's VENDORED note. The Muse app
speaks the SDK's protocol; this component must track it exactly.
