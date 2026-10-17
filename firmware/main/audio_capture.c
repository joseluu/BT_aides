/*
 * Capture audio TV : breakout PCM1808 -> I2S -> decimation 16 kHz -> G.722.
 *
 * Chaine validee en Phases 3 et 4 dans firmware_i2s_test/ (voir
 * docs/plan_implementation.md) :
 * - Le breakout PCM1808 est MAITRE (quartz 24,576 MHz, pont OP2 -> 48 kHz) :
 *   l'ESP32 est esclave I2S, BCK sur GPIO5, LRCK sur GPIO6, DATA sur GPIO7,
 *   MCLK non cable (diaphonie sur BCK dans la nappe).
 * - Philips stereo, 32 bits par slot, echantillon 24 bits cale a gauche (>> 8).
 * - Decimation par 3 : FIR 80 coefficients (fir_48k_16k.h) via ESP-DSP.
 * - G.722 64 kbit/s : libg722 (components/g722), bit-exact avec ffmpeg.
 *
 * Stereo : voies gauche et droite decimees et encodees separement (une aide
 * par oreille, meme numero de sequence, contenu different - comme Android).
 * En mode mono (une seule aide connectee), les deux encodeurs recoivent le
 * melange (G+D)/2, fait apres decimation (le filtre est lineaire).
 *
 * Les paires de trames {G, D} sont deposees dans une file : c'est l'horloge du
 * quartz audio qui cadence l'envoi ASHA. Si la file deborde (dérive entre le quartz audio et
 * l'horloge BLE, ou congestion radio), la trame la plus ancienne est jetee.
 */

#include <math.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "dsps_fir.h"
#include "g722_encoder.h"
#include "fir_48k_16k.h"
#include "audio_capture.h"

static const char *TAG = "audio";

#define I2S_SAMPLE_RATE_HZ   48000
#define I2S_BCLK_GPIO        GPIO_NUM_5
#define I2S_WS_GPIO          GPIO_NUM_6
#define I2S_DIN_GPIO         GPIO_NUM_7

#define DECIM                3
#define FRAMES_PER_READ      240                       /* 5 ms a 48 kHz */
#define OUT_PER_READ         (FRAMES_PER_READ / DECIM) /* 80 a 16 kHz */
#define G722_FRAME_SAMPLES   320                       /* 20 ms a 16 kHz */
#define QUEUE_DEPTH          6                         /* 120 ms */
#define STATS_PERIOD_FRAMES  500                       /* 10 s */

static QueueHandle_t s_frames;
static volatile bool s_restart_requested;
static volatile bool s_stereo_requested;
static volatile bool s_reset_left_requested, s_reset_right_requested;
/* Fondu sortant demande (duree en echantillons a 16 kHz), 0 = aucun. */
static volatile uint32_t s_fade_request_samples;

/* Tableaux alignes sur 16 octets : exigence de la version ESP32-S3 de
 * dsps_fird_f32. Statiques : pas sur la pile de la tache. */
static float s_fir_delay_l[FIR_48K_16K_TAPS + 4] __attribute__((aligned(16)));
static float s_fir_delay_r[FIR_48K_16K_TAPS + 4] __attribute__((aligned(16)));
static float s_l48[FRAMES_PER_READ] __attribute__((aligned(16)));
static float s_r48[FRAMES_PER_READ] __attribute__((aligned(16)));
static float s_l16[OUT_PER_READ] __attribute__((aligned(16)));
static float s_r16[OUT_PER_READ] __attribute__((aligned(16)));
static int32_t s_raw[FRAMES_PER_READ * 2];
static int16_t s_pcm16_l[G722_FRAME_SAMPLES];
static int16_t s_pcm16_r[G722_FRAME_SAMPLES];

static int16_t
to_pcm16(float x)
{
    float v = x * 32767.0f;
    v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
    return (int16_t)lrintf(v);
}

static double
rms_dbfs(double sum_sq, uint32_t n)
{
    double rms = n ? sqrt(sum_sq / n) : 0.0;
    return rms > 0.0 ? 20.0 * log10(rms) : -INFINITY;
}

static i2s_chan_handle_t
i2s_init(void)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = I2S_SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN_GPIO,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    return rx;
}

static void
audio_task(void *arg)
{
    i2s_chan_handle_t rx = i2s_init();
    fir_f32_t fir_l, fir_r;
    ESP_ERROR_CHECK(dsps_fird_init_f32(&fir_l, fir_48k_16k_coeffs, s_fir_delay_l, FIR_48K_16K_TAPS, DECIM));
    ESP_ERROR_CHECK(dsps_fird_init_f32(&fir_r, fir_48k_16k_coeffs, s_fir_delay_r, FIR_48K_16K_TAPS, DECIM));
    G722_ENC_CTX *g722_l = g722_encoder_new(64000, 0);
    G722_ENC_CTX *g722_r = g722_encoder_new(64000, 0);
    int pcm_count = 0;
    float gain = 1.0f;      /* gain du fondu sortant, remis a 1 au redemarrage du flux */
    float gain_step = 0.0f;
    bool stereo = false;   /* mode de la trame en cours, fige a chaque frontiere */

    uint32_t produced = 0, dropped = 0, read_errors = 0;
    UBaseType_t max_depth = 0;
    double sum_sq_l = 0.0, sum_sq_r = 0.0;
    uint32_t n_sq = 0;

    ESP_LOGI(TAG, "capture demarree (I2S esclave %d Hz stereo -> 16 kHz -> 2 x G.722 64 kbit/s)",
             I2S_SAMPLE_RATE_HZ);

    while (1) {
        size_t bytes_read = 0;
        if (i2s_channel_read(rx, s_raw, sizeof(s_raw), &bytes_read, 1000) != ESP_OK) {
            read_errors++;
            ESP_LOGW(TAG, "aucune trame I2S (breakout PCM1808 alimente et branche ?)");
            continue;
        }
        size_t frames = bytes_read / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames; i++) {
            s_l48[i] = (float)(s_raw[2 * i] >> 8) / 8388608.0f;
            s_r48[i] = (float)(s_raw[2 * i + 1] >> 8) / 8388608.0f;
        }
        int nout = dsps_fird_f32(&fir_l, s_l48, s_l16, frames / DECIM);
        dsps_fird_f32(&fir_r, s_r48, s_r16, frames / DECIM);

        for (int j = 0; j < nout; j++) {
            if (pcm_count == 0) {
                stereo = s_stereo_requested;
            }
            float l = s_l16[j], r = s_r16[j];
            /* Niveaux mesures par voie a l'entree, quel que soit le mode. */
            sum_sq_l += (double)l * l;
            sum_sq_r += (double)r * r;
            n_sq++;
            if (s_fade_request_samples != 0) {
                gain_step = gain / (float)s_fade_request_samples;
                s_fade_request_samples = 0;
            }
            if (gain < 1.0f) {
                l *= gain;
                r *= gain;
            }
            if (gain_step > 0.0f) {
                gain = gain > gain_step ? gain - gain_step : 0.0f;
            }
            if (stereo) {
                s_pcm16_l[pcm_count] = to_pcm16(l);
                s_pcm16_r[pcm_count] = to_pcm16(r);
            } else {
                s_pcm16_l[pcm_count] = s_pcm16_r[pcm_count] = to_pcm16(0.5f * (l + r));
            }
            pcm_count++;
            if (pcm_count < G722_FRAME_SAMPLES) {
                continue;
            }
            pcm_count = 0;
            if (s_restart_requested) {
                s_restart_requested = false;
                g722_encoder_destroy(g722_l);
                g722_encoder_destroy(g722_r);
                g722_l = g722_encoder_new(64000, 0);
                g722_r = g722_encoder_new(64000, 0);
                xQueueReset(s_frames);
                gain = 1.0f;
                gain_step = 0.0f;
            }
            /* Aide qui rejoint un flux en cours : seul l'encodeur de sa voie
             * repart de zero (son decodeur demarre a vide), l'autre n'est pas
             * touche. */
            if (s_reset_left_requested) {
                s_reset_left_requested = false;
                g722_encoder_destroy(g722_l);
                g722_l = g722_encoder_new(64000, 0);
            }
            if (s_reset_right_requested) {
                s_reset_right_requested = false;
                g722_encoder_destroy(g722_r);
                g722_r = g722_encoder_new(64000, 0);
            }
            /* File pleine : on saute ces 20 ms de PCM AVANT l'encodage, pour les
             * DEUX voies a la fois. Ne jamais jeter une trame G.722 deja encodee :
             * le codage est differentiel, le decodeur de l'aide divergerait de
             * l'encodeur ; sauter les deux voies ensemble garde les oreilles en
             * phase. */
            if (uxQueueSpacesAvailable(s_frames) == 0) {
                dropped++;
            } else {
                struct audio_frame_pair pair;
                g722_encode(g722_l, s_pcm16_l, G722_FRAME_SAMPLES, pair.left);
                g722_encode(g722_r, s_pcm16_r, G722_FRAME_SAMPLES, pair.right);
                xQueueSend(s_frames, &pair, 0);
            }
            produced++;
            UBaseType_t depth = uxQueueMessagesWaiting(s_frames);
            if (depth > max_depth) {
                max_depth = depth;
            }

            if (produced % STATS_PERIOD_FRAMES == 0) {
                ESP_LOGI(TAG, "trames produites %" PRIu32 ", jetees %" PRIu32 ", file max %u/%d,"
                         " erreurs I2S %" PRIu32 ", %s, niveau G %.1f dBFS, D %.1f dBFS",
                         produced, dropped, (unsigned)max_depth, QUEUE_DEPTH, read_errors,
                         stereo ? "stereo" : "mono", rms_dbfs(sum_sq_l, n_sq), rms_dbfs(sum_sq_r, n_sq));
                max_depth = 0;
                sum_sq_l = sum_sq_r = 0.0;
                n_sq = 0;
            }
        }
    }
}

esp_err_t
audio_capture_start(void)
{
    s_frames = xQueueCreate(QUEUE_DEPTH, sizeof(struct audio_frame_pair));
    if (s_frames == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Coeur 1 : la pile NimBLE et le controleur radio sont sur le coeur 0. */
    if (xTaskCreatePinnedToCore(audio_task, "audio_capture", 8192, NULL, 12, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void
audio_capture_restart_stream(void)
{
    s_restart_requested = true;
    /* Vide aussi la file tout de suite : sans cela, la tache d'envoi pourrait
     * prendre une trame encodee avec l'ancien etat avant que le producteur
     * n'ait traite la demande (au plus une trame peut encore passer). */
    xQueueReset(s_frames);
}

void
audio_capture_reset_channel(bool right)
{
    if (right) {
        s_reset_right_requested = true;
    } else {
        s_reset_left_requested = true;
    }
}

void
audio_capture_fade_out(uint32_t ms)
{
    uint32_t n = ms * (I2S_SAMPLE_RATE_HZ / DECIM / 1000);
    s_fade_request_samples = n > 0 ? n : 1;
}

void
audio_capture_set_stereo(bool stereo)
{
    s_stereo_requested = stereo;
}

bool
audio_capture_get_frame_pair(struct audio_frame_pair *out, TickType_t wait)
{
    return xQueueReceive(s_frames, out, wait) == pdTRUE;
}
