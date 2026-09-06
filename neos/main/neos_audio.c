/*
 * The click, and the speaker underneath it.
 *
 * A touchscreen gives no resistance and no travel, so the only thing that can
 * tell you the glass registered your finger is a sound. That was this file's
 * entire scope for a long time: one short waveform, played on the press edge
 * of every touch in the system, with a switch to turn it off.
 *
 * An app that generates its own sound - an emulator whose piezo output is
 * half of what the thing was - needs the same codec, and the choice was
 * between a second path to the part and one owner with two customers. This is
 * the second: taps and an app's stream are independent wants for one open
 * handle, either brings it up, and it goes down when neither wants it. What
 * an app cannot have is the part to itself while the rest of the system also
 * writes to it, so the click stands down for the duration.
 *
 * The codec is still brought up lazily. Bringing it up is not free - it
 * configures an I2S channel and powers the speaker amplifier through the io
 * expander - and a tablet whose owner has never asked for sound and is
 * running nothing that makes any should not be paying for either.
 *
 * The ES8388 and the I2S plumbing come from the BSP; nothing here talks to the
 * part directly. What is here is the waveform and the arbitration.
 */
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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

_Static_assert(NEOS_AUDIO_RATE == RATE_HZ,
               "the rate apps are promised is the rate the channel is clocked at");

/*
 * Loud enough to hear over a room, quiet enough not to be the thing people
 * remember about the tablet. On the codec's own 0-100 scale.
 */
#define CLICK_VOL 55

static esp_codec_dev_handle_t s_spk;
static int16_t               *s_click;
static TaskHandle_t           s_player;
static bool                   s_on;

/*
 * The second thing that can want the speaker.
 *
 * Taps and an app's stream are independent wants for one codec: either can
 * bring it up, and it stays up while either still wants it. The alternative -
 * a stream that only works when tap sounds happen to be switched on - would
 * make an unrelated setting into a dependency of every app that makes a noise.
 */
static bool s_stream;

/*
 * What the cushion is measured against.
 *
 * esp_codec_dev has no "how much room is left" call, so the depth of the DMA
 * queue is not observable directly. It does not have to be: while writes block
 * until the hardware has taken them, sound handed over minus time elapsed is
 * the same quantity, and both of those are countable here. s_t0 is set on the
 * first write of a stream rather than on open, because an app that opens the
 * codec and then spends a second loading a ROM has not underrun - it has not
 * started.
 */
static int64_t  s_t0;
static uint64_t s_frames;

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
        /*
         * s_stream is what keeps this task and neos_audio_write() from being
         * two writers into one codec: while an app holds the stream the click
         * is dropped here rather than interleaved into the app's buffers.
         */
        if (s_on && !s_stream && s_spk && s_click) {
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

    /* This also powers the speaker rail - see bsp_audio_codec_speaker_init. */
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "no ES8388 - this tablet does not play sound");
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
    ESP_LOGI(TAG, "codec up at %d Hz", RATE_HZ);
    return true;
}

/*
 * Drop the codec, but only once nothing wants it.
 *
 * Called from both owners, so it has to check both: switching tap sounds off
 * under a running game must not take the game's speaker away, and an app
 * closing its stream must not silence the clicks of the launcher it returns
 * to. The task and the waveform survive either way - the task is 3 KB and the
 * waveform 1.2 KB, against a toggle a user may well flip twice while
 * deciding, and what actually costs power is the amplifier and the I2S clock,
 * which closing the device drops.
 */
static void codec_down(void)
{
    if (!s_spk || s_on || s_stream) {
        return;
    }
    esp_codec_dev_close(s_spk);
    esp_codec_dev_delete(s_spk);
    s_spk = NULL;
    ESP_LOGI(TAG, "codec down");
}

/** Everything the click needs on top of the codec: the waveform and the task. */
static bool taps_up(void)
{
    if (!build_click()) {
        ESP_LOGE(TAG, "no memory for the click waveform");
        return false;
    }
    if (!codec_up()) {
        return false;
    }
    if (!s_player) {
        xTaskCreate(player_task, "click", 3072, NULL, 6, &s_player);
    }
    ESP_LOGI(TAG, "tap sounds on: %d ms at %d Hz, volume %d",
             CLICK_MS, TONE_HZ, CLICK_VOL);
    return true;
}

/* ------------------------------------------------------------------ */

void neos_audio_init(void)
{
    uint8_t on = 0;
    if (!neos_setting_u8(SETTING_TAPS, &on) || !on) {
        return;      /* never asked for: nothing is brought up */
    }
    s_on = taps_up();
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
        if (!taps_up()) {
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

/* ------------------------------------------------------------------ */
/* The app stream                                                      */
/* ------------------------------------------------------------------ */

/*
 * An app gets the same codec the click uses, at the same rate, and holds it
 * for as long as it is running. There is no mixer and no second channel: the
 * part plays one stream, and the only question worth answering is which one.
 * While an app is making noise, that is the app's.
 */

bool neos_audio_open(void)
{
    if (s_stream) {
        return true;
    }
    if (!codec_up()) {
        return false;
    }
    s_stream = true;
    s_t0 = 0;
    s_frames = 0;
    ESP_LOGI(TAG, "app stream open");
    return true;
}

int neos_audio_write(const int16_t *frames, int n)
{
    if (!s_stream || !s_spk) {
        return -1;
    }
    if (!frames || n <= 0) {
        return 0;
    }
    /*
     * Blocks in the codec until the I2S DMA has taken the lot, which is what
     * makes this usable as a frame clock. An app that hands over 7.8 ms of
     * sound gets 7.8 ms of real time back for free and needs no timer of its
     * own; one that cannot keep up finds out here, as a stall, rather than by
     * having a buffer quietly dropped underneath it.
     */
    if (s_t0 == 0) {
        s_t0 = esp_timer_get_time();
    }
    if (esp_codec_dev_write(s_spk, (void *)frames, n * (int)sizeof(int16_t)) != ESP_OK) {
        return -1;
    }
    s_frames += (uint64_t)n;
    return n;
}

/*
 * How far ahead of the speaker the app is.
 *
 * Sound handed over, as a duration, minus the time since the first block went
 * in. While the app is keeping up this settles at the depth of the codec's own
 * queue and stays there, because neos_audio_write() blocks and so the app can
 * never get further ahead than the hardware will hold. What it is for is the
 * other case: an app about to do something expensive can ask whether it is
 * being paid for out of the cushion or out of the sound.
 *
 * A stall drives it to zero and no further. Once the queue has run dry the
 * time already lost is not a debt the app can repay - the speaker played
 * silence and that silence is spent - so the baseline moves up instead and the
 * next block starts from nothing. Letting it go negative would make an app
 * that glitched once look permanently behind.
 */
int32_t neos_audio_lead_us(void)
{
    if (!s_stream || !s_spk || s_t0 == 0) {
        return 0;
    }
    const int64_t played  = esp_timer_get_time() - s_t0;
    const int64_t written = (int64_t)(s_frames * 1000000u / NEOS_AUDIO_RATE);
    const int64_t lead    = written - played;

    if (lead <= 0) {
        s_t0 = esp_timer_get_time() - written;   /* re-base: the gap is spent */
        return 0;
    }
    return (int32_t)lead;
}

bool neos_audio_gain(uint8_t percent)
{
    if (!s_stream || !s_spk) {
        return false;
    }
    if (percent > 100) {
        percent = 100;
    }
    return esp_codec_dev_set_out_vol(s_spk, percent) == ESP_OK;
}

void neos_audio_close(void)
{
    if (!s_stream) {
        return;
    }
    s_stream = false;          /* before the close, for codec_down's guard */
    if (s_spk) {
        /*
         * Back to the system's level rather than to whatever the app left.
         * Volume is not the app's to keep: the next thing out of this speaker
         * is a tap click somewhere else entirely.
         */
        esp_codec_dev_set_out_vol(s_spk, CLICK_VOL);
    }
    codec_down();
    ESP_LOGI(TAG, "app stream closed");
}

void neos_audio_app_release(void)
{
    if (s_stream) {
        ESP_LOGW(TAG, "app returned holding the stream - closing it");
        neos_audio_close();
    }
}
