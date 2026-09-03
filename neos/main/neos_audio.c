/*
 * The click.
 *
 * A touchscreen gives no resistance and no travel, so the only thing that can
 * tell you the glass registered your finger is a sound. That is the entire
 * scope of this file: one short waveform, played on the press edge of every
 * touch in the system, with a switch to turn it off.
 *
 * The codec is brought up lazily and only when the setting is on. Bringing it
 * up is not free - it configures an I2S channel and powers the speaker
 * amplifier through the io expander - and a tablet whose owner has never asked
 * for sound should not be paying for either.
 *
 * The ES8388 and the I2S plumbing come from the BSP; nothing here talks to the
 * part directly. What is here is the waveform and the decision about when to
 * play it.
 */
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"

#include "neos_audio.h"
#include "neos_settings.h"
#include "neos_sys.h"

static const char *TAG = "audio";

#define SETTING_TAPS "tapsound"

/*
 * 48 kHz mono 16-bit, because that is what bsp_audio_init(NULL) configures the
 * I2S channel for. Asking the codec for a rate the channel is not clocked at
 * plays the right samples at the wrong speed, which for a click is the
 * difference between a tick and a thud.
 */
#define RATE_HZ   48000
#define CLICK_MS  12
#define TONE_HZ   2400
#define CLICK_N   (RATE_HZ / 1000 * CLICK_MS)

/*
 * Loud enough to hear over a room, quiet enough not to be the thing people
 * remember about the tablet. On the codec's own 0-100 scale.
 */
#define CLICK_VOL 55

static esp_codec_dev_handle_t s_spk;
static int16_t               *s_click;
static TaskHandle_t           s_player;
static bool                   s_on;

/* ------------------------------------------------------------------ */

/*
 * A decaying sine rather than a square.
 *
 * A square at this length is all edge and reads as a pop through a small
 * speaker; the envelope is what makes it a click. The decay is chosen so the
 * tail is inaudible by the end of the buffer - a click that is still ringing
 * when the next one starts sounds like a rattle, and at typing speed the next
 * one is about eighty milliseconds away.
 */
static bool build_click(void)
{
    if (s_click) {
        return true;
    }
    s_click = heap_caps_malloc(CLICK_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_click) {
        return false;
    }
    for (int i = 0; i < CLICK_N; i++) {
        const float t   = (float)i / (float)RATE_HZ;
        const float env = expf(-t * 380.0f);
        const float v   = sinf(2.0f * (float)M_PI * (float)TONE_HZ * t) * env;
        s_click[i] = (int16_t)(v * 11000.0f);
    }
    return true;
}

/*
 * One task, one buffer, no queue.
 *
 * The notification is the whole synchronisation: the touch task gives it and
 * returns, and a notification that arrives while the write is in progress is
 * either coalesced into the next wait or dropped. Dropping is right - a click
 * is feedback about something that has already happened, and one played late
 * is worse than one not played.
 */
static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_on && s_spk && s_click) {
            esp_codec_dev_write(s_spk, s_click, CLICK_N * sizeof(int16_t));
        }
    }
}

/** Bring the codec up. Idempotent; false if the part will not play. */
static bool codec_up(void)
{
    if (s_spk) {
        return true;
    }
    if (!build_click()) {
        ESP_LOGE(TAG, "no memory for the click waveform");
        return false;
    }

    /* This also powers the speaker rail - see bsp_audio_codec_speaker_init. */
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "no ES8388 - tap sounds unavailable");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .channel_mask    = 1,
        .sample_rate     = RATE_HZ,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "the codec would not open at %d Hz", RATE_HZ);
        esp_codec_dev_delete(s_spk);
        s_spk = NULL;
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk, CLICK_VOL);

    if (!s_player) {
        xTaskCreate(player_task, "click", 3072, NULL, 6, &s_player);
    }
    ESP_LOGI(TAG, "tap sounds on: %d ms at %d Hz, volume %d",
             CLICK_MS, TONE_HZ, CLICK_VOL);
    return true;
}

/*
 * Switching off closes the codec but leaves the task and the waveform.
 *
 * The task is 3 KB and the waveform 1.2 KB, against a toggle a user may well
 * flip twice while deciding. What matters for power is the amplifier and the
 * I2S clock, and closing the device drops both.
 */
static void codec_down(void)
{
    if (!s_spk) {
        return;
    }
    esp_codec_dev_close(s_spk);
    esp_codec_dev_delete(s_spk);
    s_spk = NULL;
    ESP_LOGI(TAG, "tap sounds off");
}

/* ------------------------------------------------------------------ */

void neos_audio_init(void)
{
    uint8_t on = 0;
    if (!neos_setting_u8(SETTING_TAPS, &on) || !on) {
        return;      /* never asked for: nothing is brought up */
    }
    s_on = codec_up();
    if (!s_on) {
        ESP_LOGW(TAG, "tap sounds were on but the codec did not come up");
    }
}

void neos_audio_click(void)
{
    if (s_on && s_player) {
        xTaskNotifyGive(s_player);
    }
}

bool neos_tap_sound(void)
{
    return s_on;
}

bool neos_tap_sound_set(bool on)
{
    if (on == s_on) {
        return true;
    }
    if (on) {
        if (!codec_up()) {
            return false;      /* not saved: the setting would be a lie */
        }
        s_on = true;
    } else {
        s_on = false;          /* before the close, so nothing writes into it */
        codec_down();
    }
    neos_setting_set_u8(SETTING_TAPS, s_on ? 1 : 0);
    return true;
}
