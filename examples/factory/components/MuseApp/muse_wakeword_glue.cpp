/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Factory-new wake-word glue. See muse_wakeword_glue.h for the contract.
 *
 * Structure mirrors the SDK's esp32/components/muse/muse_wakeword_glue.c:
 * a wake task drains a tap stream buffer, ticks the core once per
 * 320-frame chunk, and hands turns to the app. The differences are the
 * factory seams, documented in the header: turns open through MuseApp
 * control events (not a voice task), the pre-roll seed is fed by the
 * input task after beginTurn() (the session resets its mic buffer at
 * turn start, so a seed fed earlier would be discarded), and listening
 * is gated on app visibility in addition to the opt-in.
 */

#include "muse_wakeword_glue.h"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_MUSE_WAKEWORD_ENABLE

#include "muse_mem.h"
#include "muse_wakeword.h"
#include "ww_runner.h"

namespace esp_brookesia::apps {

namespace {

constexpr const char *TAG = "muse_wake";

constexpr size_t kTapChunks = 25;   /* 500 ms of scheduling slack */
constexpr size_t kItemBytes = sizeof(muse_ww_chunk_t);
static_assert(
    MUSE_WAKEWORD_BACKLOG_CHUNKS >= kTapChunks + 2,
    "the recorder's backlog must cover a full tap plus the chunks read before it starts"
);
static_assert(
    (MUSE_WAKEWORD_BACKLOG_CHUNKS & (MUSE_WAKEWORD_BACKLOG_CHUNKS - 1)) == 0,
    "a power of two keeps the history's slots continuous across the number wrap"
);
constexpr size_t kPreFrames = 16000;   /* 1 s of upload pre-roll */
constexpr size_t kWakeTaskStack = 8192;
constexpr int kWakeTaskPrio = 5;       /* below the input task (8) */
constexpr int kWakeTaskCore = 0;       /* same core as the input task */

constexpr const char *kNvsNamespace = "muse_ww";
constexpr const char *kNvsEnabledKey = "enabled";

/* ------------------------------------------------------------------ */
/* Static state. The wake task is the single reader of the tap and the */
/* single writer of the pre-roll ring, the session core, and the       */
/* runner, applying flag transitions at its loop boundary in           */
/* apply_requests(). That keeps the runner single-threaded (see        */
/* ww_runner.h). Writers: s_enabled and s_disables any task;           */
/* s_active the control task; s_playback, s_ptt_turn and s_pending_*   */
/* the control/input tasks; s_applied_* and the rest the wake task.    */
/* ------------------------------------------------------------------ */

MuseWakewordCallbacks s_callbacks;
ww_runner_t *s_runner = nullptr;
muse_wakeword_core_t s_core;
/* Input task -> wake task. A stream buffer is FreeRTOS's single-writer,
 * single-reader primitive: the writer owns the head and the reader the
 * tail, so neither needs a lock. Only whole chunks are ever written. */
StreamBufferHandle_t s_tap = nullptr;
volatile uint32_t s_tap_drops = 0;   /* frames; input task only writes */
/* Input task only: its numbered copy of the last 32 chunks it tapped,
 * for the recorder's backlog (muse_wakeword_take_backlog). */
muse_ww_history_t s_hist;
muse_ww_chunk_t *s_hist_slots = nullptr;
/* Set by the wake task before it posts a hands-free turn: the number of
 * the first chunk after the seed, which the recorder appends from. */
std::atomic<uint32_t> s_seed_end{0};
std::atomic_bool s_backlog_owed{false};
uint32_t s_last_seq = 0;    /* wake task only: the chunk being ticked */
bool s_have_seq = false;    /* wake task only: s_last_seq is set */
muse_ww_ring_t s_pre;       /* wake task only: upload pre-roll */
int16_t *s_snap = nullptr;  /* kPreFrames, for the seed handoff */
bool s_started = false;
bool s_init_done = false;

/* Input-task staging: accumulates the resampler's variable frame counts
 * into exact 320-frame chunks. Owned by the input task only. */
int16_t s_stage[MUSE_WAKEWORD_CHUNK_FRAMES];
size_t s_stage_fill = 0;

/* The pending pre-roll seed. The wake task writes the snapshot only
 * while s_seed_ready is false... (see tick(): a seed is always consumed
 * or cleared before the next capture can open, as in the SDK, where the
 * voice task takes the seed when the turn opens and the glue clears it
 * when the turn cannot). The input task reads it only while
 * s_seed_ready is true. */
std::atomic_bool s_seed_ready{false};
std::atomic<uint32_t> s_seed_gen{0};
std::atomic<size_t> s_seed_frames{0};

/* Listening = opt-in (s_enabled) AND app visible (s_active). The desired
 * flags are published by the control task; s_applied_listening is the
 * value the wake task last applied to the core and runner. */
volatile bool s_enabled = false;      /* desired listening opt-in */
volatile bool s_active = false;       /* desired: the app is visible */
volatile bool s_applied_listening = false;   /* wake task applies; taps/status read */
/* Disable generation: bumped by every disable. A turn request carries
 * the value from when its wake was heard, and the control task drops
 * the turn once it moves (muse_wakeword_turn_wanted): audio already
 * streamed has left, but nothing more leaves once listening is off,
 * and the note isn't committed or kept. */
std::atomic<uint32_t> s_disables{0};
uint32_t s_arm_gen = 0;   /* wake task only: s_disables when the wake was heard */
volatile bool s_playback = false;     /* desired: own-voice playback active */
volatile bool s_ptt_turn = false;     /* desired: physical PTT turn recording */
std::atomic_bool s_pending_down{false};
std::atomic_bool s_pending_up{false};
bool s_applied_playback = false;      /* wake task only */
bool s_applied_ptt_turn = false;      /* wake task only */
muse_wakeword_stats_t s_stats_snap;   /* wake task writes, status reads */
int s_state_snap = MUSE_WW_IDLE;
volatile bool s_runner_ok = true;     /* wake task writes: the runner can detect */
uint32_t s_capture_chunks = 0;        /* wake task only, for the log */
size_t s_seeded_frames = 0;
bool s_up_owed = false;   /* wake task only: a turn end the control queue refused */
bool s_up_owed_cancel = false;   /* wake task only: the owed end drops the turn */
int64_t s_armed_us = 0;   /* wake task only: when the waiting wake was heard */
volatile bool s_turn_end_req = false;   /* the control task ended the hands-free turn itself */

bool load_opt_in()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, kNvsEnabledKey, &value);
    nvs_close(handle);
    return err == ESP_OK && value != 0;
}

void save_opt_in(bool on)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "could not open NVS for wake-word opt-in");
        return;
    }
    esp_err_t err = nvs_set_u8(handle, kNvsEnabledKey, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save wake-word opt-in: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

void clear_seed_locked()
{
    s_seed_ready.store(false);
    s_backlog_owed.store(false);
}

/* ------------------------------------------------------------------ */
/* Core tick.                                                          */
/* ------------------------------------------------------------------ */

/* One core tick and everything its actions mean for the device. */
void tick(const muse_wakeword_input_t *in)
{
    /* A turn end the control queue refused goes first: without it the
     * turn it ends would record to its 15 s cap. */
    if (s_up_owed) {
        bool accepted = false;
        if (s_up_owed_cancel) {
            if (s_callbacks.onTurnCancel) {
                accepted = s_callbacks.onTurnCancel();
            }
        } else {
            if (s_callbacks.onTurnEnd) {
                accepted = s_callbacks.onTurnEnd();
            }
        }
        if (accepted) {
            s_up_owed = false;
        }
    }
    muse_ww_state_t before = muse_wakeword_core_state(&s_core);
    uint32_t act = muse_wakeword_core_tick(&s_core, in);
    muse_ww_state_t now = muse_wakeword_core_state(&s_core);
    if (now == MUSE_WW_CAPTURING || now == MUSE_WW_CAPTURING_PTT) {
        s_capture_chunks++;
    }
    if (before == MUSE_WW_IDLE && now == MUSE_WW_ARMED) {
        ESP_LOGI(TAG, "wake heard (runner=%s), listening for speech", muse_wakeword_runner_kind());
        s_armed_us = esp_timer_get_time();
        s_arm_gen = s_disables.load();
        if (s_callbacks.onHeard) {
            s_callbacks.onHeard();
        }
    }
    if ((act & MUSE_WW_ACTION_POST_PTT_DOWN) != 0) {
        /* The seed is the pre-roll through this chunk (at most 1 s). The
         * input task may already have tapped later chunks that this task
         * hasn't reached (up to kTapChunks under load): the recorder takes
         * those from its own numbered copy, from s_seed_end on, so the note
         * runs on without a gap or a repeat. Endpointing still trails live
         * audio by that backlog. */
        s_seeded_frames = muse_ww_ring_snapshot(&s_pre, s_snap, kPreFrames);
        s_seed_frames.store(s_seeded_frames);
        s_seed_end.store(s_last_seq + 1);
        s_backlog_owed.store(true);
        s_seed_gen.fetch_add(1);
        s_seed_ready.store(true);
        s_capture_chunks = 0;
        /* Never a turn start ahead of an owed turn end. A start the
         * control queue refused opens no turn: drop the session and its
         * seed, so no stray end or stale pre-roll follows it. */
        if (!s_up_owed && s_callbacks.onTurnStart && s_callbacks.onTurnStart(s_arm_gen)) {
            ESP_LOGI(TAG, "speech started, pre-roll %.2fs", (double)s_seeded_frames / 16000);
        } else {
            muse_wakeword_core_abort(&s_core);
            clear_seed_locked();
        }
    }
    if ((act & MUSE_WW_ACTION_POST_PTT_UP) != 0) {
        bool cancel = (act & MUSE_WW_ACTION_CANCELLED) != 0;
        bool accepted = false;
        if (cancel) {
            if (s_callbacks.onTurnCancel) {
                accepted = s_callbacks.onTurnCancel();
            }
        } else {
            if (s_callbacks.onTurnEnd) {
                accepted = s_callbacks.onTurnEnd();
            }
        }
        if (!accepted) {
            s_up_owed = true;
            s_up_owed_cancel = cancel;
        }
    }
    if ((act & MUSE_WW_ACTION_CONVERTED_TO_PTT) != 0) {
        ESP_LOGI(TAG, "the talk button took over");
    }
    if ((act & MUSE_WW_ACTION_SUBMITTED) != 0) {
        ESP_LOGI(TAG, "wake: captured %.1fs, preroll %.1fs",
                 s_capture_chunks * (double)MUSE_WAKEWORD_CHUNK_FRAMES / 16000,
                 (double)s_seeded_frames / 16000);
    }
    if ((act & MUSE_WW_ACTION_CANCELLED) != 0) {
        if (before == MUSE_WW_ARMED) {
            if (s_callbacks.onCancelled) {
                s_callbacks.onCancelled();
            }
            ESP_LOGI(TAG, "wake ignored (no speech followed)");
        } else {
            ESP_LOGI(TAG, "hands-free note dropped (listening turned off)");
        }
    }
}

/* Wake task only: turn the published desire flags into transitions
 * on the pre-roll ring, core, and runner. Outside init this is the one place
 * those objects are mutated, so no other task can race them. */
void apply_requests()
{
    bool want_listening = s_enabled && s_active;
    if (want_listening != s_applied_listening) {
        if (want_listening) {
            /* Re-enabled while a disable is still closing a capture:
             * let the core post its up/submit first, then reset. */
            if (muse_wakeword_core_state(&s_core) != MUSE_WW_IDLE) {
                muse_wakeword_input_t in = {};
                tick(&in);
            }
            xStreamBufferReset(s_tap);   /* no task blocks on it here: we are its reader */
            s_have_seq = false;          /* the chunks skipped while off weren't lost */
            muse_ww_ring_clear(&s_pre);
            muse_wakeword_core_reset(&s_core);   /* an owed turn end still goes */
            s_pending_down.store(false);
            s_pending_up.store(false);
            ww_runner_reset(s_runner);
        } else {
            muse_ww_ring_clear(&s_pre);
            /* The core closes an open capture on its next ticks with
             * enabled=false; the task keeps ticking until it is idle. */
        }
        s_applied_listening = want_listening;
        ww_runner_set_listening(s_runner, want_listening);
        ESP_LOGI(TAG, "listening %s (runner=%s)", want_listening ? "on" : "off",
                 muse_wakeword_runner_kind());
    }
    if (s_turn_end_req) {
        s_turn_end_req = false;
        /* The turn is over on the control side: close a capture still open
         * here (its turn end would be stray now) and forget an owed one.
         * An arming is a new session, not this turn's. */
        s_up_owed = false;
        muse_ww_state_t st = muse_wakeword_core_state(&s_core);
        if (st == MUSE_WW_CAPTURING || st == MUSE_WW_CAPTURING_PTT) {
            muse_wakeword_core_abort(&s_core);
            ESP_LOGI(TAG, "hands-free turn ended by the recorder");
        }
    }
    if (s_playback != s_applied_playback) {
        s_applied_playback = s_playback;
        ww_runner_set_suppressed(s_runner, s_applied_playback || s_applied_ptt_turn);
        muse_ww_ring_clear(&s_pre);   /* Muse's own output is never pre-roll */
        if (!s_applied_playback) {
            ww_runner_reset(s_runner);   /* no stale model state across turns */
        }
    }
    if (s_ptt_turn != s_applied_ptt_turn) {
        bool turned_off = s_applied_ptt_turn && !s_ptt_turn;
        s_applied_ptt_turn = s_ptt_turn;
        ww_runner_set_suppressed(s_runner, s_applied_playback || s_applied_ptt_turn);
        if (turned_off) {
            muse_ww_ring_clear(&s_pre);
            ww_runner_reset(s_runner);
        }
    }
}

void snapshot_status()
{
    s_stats_snap = muse_wakeword_core_stats(&s_core);
    s_state_snap = static_cast<int>(muse_wakeword_core_state(&s_core));
    bool ok = ww_runner_ok(s_runner);
    if (ok != s_runner_ok) {
        if (ok) {
            ESP_LOGI(TAG, "runner recovered");
        } else {
            ESP_LOGW(TAG, "runner stopped: not listening until it reloads");
        }
        s_runner_ok = ok;
    }
}

muse_wakeword_input_t next_input(const int16_t *pcm)
{
    /* One suppression gate for the core: own-voice playback and a
     * physical turn both mean "discard wakes". Runner listening and
     * suppression were applied with the transitions above. */
    muse_wakeword_input_t in = {};
    in.voiced = muse_wakeword_energy_voiced(
        pcm, MUSE_WAKEWORD_CHUNK_FRAMES, MUSE_WAKEWORD_ENERGY_RMS_DEFAULT
    );
    /* Read and clear at once: a press noted in between must not be lost. */
    in.ptt_down = s_pending_down.exchange(false);
    in.ptt_up = s_pending_up.exchange(false);
    in.playback = s_applied_playback || s_applied_ptt_turn;
    in.enabled = s_applied_listening;
    in.event = ww_runner_feed(s_runner, pcm, MUSE_WAKEWORD_CHUNK_FRAMES);
    return in;
}

void wake_task(void * /*arg*/)
{
    muse_ww_chunk_t item;
    for (;;) {
        apply_requests();
        /* A waiting wake ends on the clock too, whether or not audio came
         * meanwhile (a stalled tap, a reply playing): end it and its pre-roll. */
        if (muse_wakeword_core_state(&s_core) == MUSE_WW_ARMED &&
            esp_timer_get_time() - s_armed_us >=
                static_cast<int64_t>(CONFIG_MUSE_WAKEWORD_START_TIMEOUT_MS) * 1000 &&
            muse_wakeword_core_gap(&s_core, &s_pre)) {
            if (s_callbacks.onCancelled) {
                s_callbacks.onCancelled();
            }
            ESP_LOGI(TAG, "wake ignored (no speech followed)");
        }
        /* The timeout keeps requests applied while the tap is dry. */
        if (xStreamBufferReceive(s_tap, &item, kItemBytes, pdMS_TO_TICKS(10)) != kItemBytes) {
            /* Disabled mid-capture with the tap now dry: one silent
             * tick is what closes the synthetic hold. */
            if (!s_applied_listening && muse_wakeword_core_state(&s_core) != MUSE_WW_IDLE) {
                muse_wakeword_input_t in = {};
                tick(&in);
            } else if (s_up_owed) {
                bool accepted = false;
                if (s_up_owed_cancel) {
                    if (s_callbacks.onTurnCancel) {
                        accepted = s_callbacks.onTurnCancel();
                    }
                } else {
                    if (s_callbacks.onTurnEnd) {
                        accepted = s_callbacks.onTurnEnd();
                    }
                }
                if (accepted) {
                    s_up_owed = false;   /* the tap is dry, but the turn end still goes */
                }
            }
            snapshot_status();
            continue;
        }
        if (s_have_seq && item.seq != s_last_seq + 1) {
            /* The input side dropped chunks while the tap was full: no seed
             * may span the gap or come from before it. */
            ESP_LOGW(TAG, "tap gap: audio dropped before chunk %" PRIu32, item.seq);
            if (muse_wakeword_core_gap(&s_core, &s_pre)) {
                if (s_callbacks.onCancelled) {
                    s_callbacks.onCancelled();
                }
            }
            ww_runner_reset(s_runner);   /* nor the model's history */
        }
        s_have_seq = true;
        if (!s_applied_playback && !s_applied_ptt_turn) {
            muse_ww_ring_push(&s_pre, item.pcm, MUSE_WAKEWORD_CHUNK_FRAMES);
        }
        s_last_seq = item.seq;
        muse_wakeword_input_t in = next_input(item.pcm);
        tick(&in);
        snapshot_status();
    }
}

void tap_chunk(const int16_t *pcm)
{
    /* Numbered and kept for the recorder first, so a chunk the full tap
     * drops still reaches a hands-free note. */
    const muse_ww_chunk_t *item = muse_ww_history_push(&s_hist, pcm);
    /* Whole items or nothing: a send into too little space would be
     * partial, so check first. Space only grows between the check and the
     * send (this is the only writer). On overload the newest chunk drops. */
    if (xStreamBufferSpacesAvailable(s_tap) >= kItemBytes) {
        (void)xStreamBufferSend(s_tap, item, kItemBytes, 0);
    } else {
        s_tap_drops += MUSE_WAKEWORD_CHUNK_FRAMES;
    }
}

}  // namespace

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

const char *muse_wakeword_runner_kind(void)
{
    return "executorch";
}

esp_err_t muse_wakeword_init(const MuseWakewordCallbacks &callbacks)
{
    if (s_init_done) {
        return ESP_OK;
    }
    s_init_done = true;

    s_callbacks = callbacks;

    s_tap = xStreamBufferCreateWithCaps(
        kTapChunks * kItemBytes, kItemBytes, MUSE_BIG_CAPS
    );
    s_hist_slots = static_cast<muse_ww_chunk_t *>(
        heap_caps_malloc(
            MUSE_WAKEWORD_BACKLOG_CHUNKS * sizeof(*s_hist_slots), MUSE_BIG_CAPS
        )
    );
    int16_t *pre_storage = static_cast<int16_t *>(
        heap_caps_malloc(kPreFrames * sizeof(int16_t), MUSE_BIG_CAPS)
    );
    s_snap = static_cast<int16_t *>(
        heap_caps_malloc(kPreFrames * sizeof(int16_t), MUSE_BIG_CAPS)
    );
    ww_config_t cfg = {};
    cfg.sample_rate_hz = 16000;
#if CONFIG_MUSE_WAKEWORD_SENSITIVITY_LOW
    cfg.sensitivity = WW_SENSITIVITY_LOW;
#elif CONFIG_MUSE_WAKEWORD_SENSITIVITY_HIGH
    cfg.sensitivity = WW_SENSITIVITY_HIGH;
#else
    cfg.sensitivity = WW_SENSITIVITY_DEFAULT;
#endif
    cfg.vad = WW_VAD_ENERGY;
    s_runner = (s_tap != nullptr && s_hist_slots != nullptr && pre_storage != nullptr &&
                s_snap != nullptr)
                   ? ww_runner_create(&cfg)
                   : nullptr;
    if (s_runner == nullptr) {
        if (s_tap != nullptr) {
            vStreamBufferDeleteWithCaps(s_tap);
            s_tap = nullptr;
        }
        heap_caps_free(s_hist_slots);
        s_hist_slots = nullptr;
        heap_caps_free(pre_storage);
        heap_caps_free(s_snap);
        s_snap = nullptr;
        ESP_LOGE(TAG, "no memory for wake word");
        return ESP_ERR_NO_MEM;
    }
    muse_ww_ring_init(&s_pre, pre_storage, kPreFrames);
    muse_ww_history_init(&s_hist, s_hist_slots, MUSE_WAKEWORD_BACKLOG_CHUNKS);
    muse_wakeword_config_t core_cfg = {};
    core_cfg.start_timeout_chunks =
        muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_START_TIMEOUT_MS);
    core_cfg.end_silence_chunks =
        muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_END_SILENCE_MS);
    core_cfg.max_capture_chunks =
        muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_MAX_CAPTURE_MS);
    core_cfg.energy_endpointing = true;
    muse_wakeword_core_init(&s_core, &core_cfg);
    s_enabled = load_opt_in();
    s_active = false;
    s_applied_listening = false;
    ww_runner_set_listening(s_runner, false);
    if (xTaskCreatePinnedToCoreWithCaps(
            wake_task, "muse_wake", kWakeTaskStack, nullptr, kWakeTaskPrio,
            nullptr, kWakeTaskCore, MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the wake task");
        ww_runner_destroy(s_runner);
        s_runner = nullptr;
        vStreamBufferDeleteWithCaps(s_tap);
        s_tap = nullptr;
        muse_ww_history_init(&s_hist, nullptr, 0);
        heap_caps_free(s_hist_slots);
        s_hist_slots = nullptr;
        heap_caps_free(pre_storage);
        muse_ww_ring_init(&s_pre, nullptr, 0);
        heap_caps_free(s_snap);
        s_snap = nullptr;
        return ESP_ERR_NO_MEM;   /* s_started stays false: every hook no-ops */
    }
    s_started = true;
    snapshot_status();
    ESP_LOGI(TAG, "wake word ready: runner=%s, listening %s",
             muse_wakeword_runner_kind(),
             s_enabled ? "on (saved opt-in)" : "off (saved opt-in is off)");
    return ESP_OK;
}

void muse_wakeword_on_audio(const int16_t *pcm, size_t frames)
{
    if (!s_started || !s_applied_listening) {
        /* Resume fresh when listening starts again: a chunk split across
         * the off period would be discontinuous audio. */
        s_stage_fill = 0;
        return;
    }
    if (pcm == nullptr || frames == 0) {
        return;
    }
    size_t at = 0;
    while (at < frames) {
        size_t take = frames - at;
        if (take > MUSE_WAKEWORD_CHUNK_FRAMES - s_stage_fill) {
            take = MUSE_WAKEWORD_CHUNK_FRAMES - s_stage_fill;
        }
        memcpy(s_stage + s_stage_fill, pcm + at, take * sizeof(int16_t));
        s_stage_fill += take;
        at += take;
        if (s_stage_fill == MUSE_WAKEWORD_CHUNK_FRAMES) {
            s_stage_fill = 0;
            tap_chunk(s_stage);
        }
    }
}

void muse_wakeword_note_ptt(bool down)
{
    if (down) {
        s_pending_down.store(true);
    } else {
        s_pending_up.store(true);
    }
}

void muse_wakeword_set_enabled(bool on)
{
    if (!s_started) {
        /* Init failed, so nothing listens. Saving "on" would start
         * listening at a later boot nobody asked for; "off" still saves. */
        if (!on) {
            save_opt_in(false);
        }
        return;
    }
    save_opt_in(on);
    /* Publish only. The wake task applies the transition, so a
     * redundant enable cannot reset a live session out from under
     * the control task (which would orphan the synthetic hold). */
    s_enabled = on;
    if (!on) {
        s_disables.fetch_add(1);
    }
}

bool muse_wakeword_enabled(void)
{
    return s_started && s_enabled;
}

void muse_wakeword_set_active(bool active)
{
    s_active = active;
}

bool muse_wakeword_listening(void)
{
    /* A runner whose model failed to load or run hears nothing:
     * not listening until a reset reloads it. */
    /* Off as soon as a disable is requested (s_enabled / s_active), not
     * only once the wake task applies it: the wake task applies it before
     * the next chunk, so nothing more is heard, and the indicator must
     * not say listening. On only once applied. */
    return s_started && s_enabled && s_active && s_applied_listening && s_runner_ok &&
           !s_applied_playback && !s_applied_ptt_turn;
}

void muse_wakeword_set_playback(bool on)
{
    if (!s_started) {
        return;
    }
    s_playback = on;   /* applied by the wake task */
}

void muse_wakeword_set_ptt_turn(bool active)
{
    if (!s_started) {
        return;
    }
    s_ptt_turn = active;   /* applied by the wake task */
}

bool muse_wakeword_turn_wanted(uint32_t disables)
{
    return s_started && s_disables.load() == disables;
}

void muse_wakeword_turn_ended(void)
{
    if (!s_started) {
        return;
    }
    s_turn_end_req = true;   /* applied by the wake task */
}

bool muse_wakeword_take_backlog(
    const int16_t *chunks[], size_t max_chunks, size_t *count_out
)
{
    if (count_out != nullptr) {
        *count_out = 0;
    }
    if (!s_started || !s_backlog_owed.exchange(false)) {
        return true;   /* nothing owed: the seed stands alone */
    }
    bool complete = false;
    *count_out = muse_ww_history_since(
        &s_hist, s_seed_end.load(), chunks, max_chunks, &complete
    );
    if (!complete) {
        ESP_LOGW(TAG, "hands-free note dropped: audio after its seed was lost to overload");
    }
    return complete;
}

bool muse_wakeword_seed_ready(void)
{
    return s_seed_ready.load();
}

uint32_t muse_wakeword_seed_generation(void)
{
    return s_seed_gen.load();
}

const int16_t *muse_wakeword_seed_data(size_t *frames_out)
{
    if (frames_out != nullptr) {
        *frames_out = s_seed_frames.load();
    }
    return s_snap;
}

void muse_wakeword_clear_seed(void)
{
    clear_seed_locked();
}

void muse_wakeword_status(MuseWakewordStatus *out)
{
    if (out == nullptr) {
        return;
    }
    /* Diagnostic snapshot refreshed by the wake task; individual
     * fields are word-sized and may tear across one refresh. */
    *out = MuseWakewordStatus{};
    out->enabled = muse_wakeword_enabled();
    out->listening = muse_wakeword_listening();
    out->runner = muse_wakeword_runner_kind();
    out->state = s_state_snap;
    out->wakes = s_stats_snap.wakes;
    out->cancels = s_stats_snap.cancels;
    out->submits = s_stats_snap.submits;
    out->conversions = s_stats_snap.conversions;
    out->suppressed_wakes = s_stats_snap.suppressed_wakes;
    out->dropped_frames = s_tap_drops;
}

}  // namespace esp_brookesia::apps

#else  /* !CONFIG_MUSE_WAKEWORD_ENABLE */

/* Feature compiled out: every entry point is a no-op, so call sites need
 * no conditionals. */

#include <cstddef>

namespace esp_brookesia::apps {

const char *muse_wakeword_runner_kind(void)
{
    return "off";
}

esp_err_t muse_wakeword_init(const MuseWakewordCallbacks & /*callbacks*/)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void muse_wakeword_on_audio(const int16_t * /*pcm*/, size_t /*frames*/) {}

void muse_wakeword_note_ptt(bool /*down*/) {}

void muse_wakeword_set_enabled(bool /*on*/) {}

bool muse_wakeword_enabled(void)
{
    return false;
}

void muse_wakeword_set_active(bool /*active*/) {}

bool muse_wakeword_listening(void)
{
    return false;
}

void muse_wakeword_set_playback(bool /*on*/) {}

void muse_wakeword_set_ptt_turn(bool /*active*/) {}

bool muse_wakeword_turn_wanted(uint32_t /*disables*/)
{
    return false;
}

void muse_wakeword_turn_ended(void) {}

bool muse_wakeword_take_backlog(
    const int16_t * /*chunks*/[], size_t /*max_chunks*/, size_t *count_out
)
{
    if (count_out != nullptr) {
        *count_out = 0;
    }
    return true;
}

bool muse_wakeword_seed_ready(void)
{
    return false;
}

uint32_t muse_wakeword_seed_generation(void)
{
    return 0;
}

const int16_t *muse_wakeword_seed_data(size_t *frames_out)
{
    if (frames_out != nullptr) {
        *frames_out = 0;
    }
    return nullptr;
}

void muse_wakeword_clear_seed(void) {}

void muse_wakeword_status(MuseWakewordStatus *out)
{
    if (out == nullptr) {
        return;
    }
    *out = MuseWakewordStatus{};
    out->runner = "off";
}

}  // namespace esp_brookesia::apps

#endif  /* CONFIG_MUSE_WAKEWORD_ENABLE */

/* The glue's chunk size must match the vendored core's. */
#if CONFIG_MUSE_WAKEWORD_ENABLE
#include "muse_wakeword.h"
static_assert(
    MUSE_WAKEWORD_CHUNK_FRAMES == MUSE_WW_CHUNK_FRAMES,
    "MUSE_WAKEWORD_CHUNK_FRAMES must match MUSE_WW_CHUNK_FRAMES"
);
#endif
