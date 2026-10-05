#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Trame G.722 64 kbit/s de 20 ms : 320 echantillons a 16 kHz -> 160 octets. */
#define AUDIO_G722_FRAME_BYTES 160

/* Demarre la chaine PCM1808 (I2S esclave 48 kHz) -> decimation 16 kHz ->
 * G.722, dans une tache epinglee sur le coeur 1. */
esp_err_t audio_capture_start(void);

/* A appeler a chaque START ASHA : vide la file de trames et reinitialise
 * l'encodeur a la prochaine frontiere de trame (le decodeur de l'aide repart
 * de zero). */
void audio_capture_restart_stream(void);

/* Recupere la trame suivante ; false si aucune trame n'arrive dans le delai. */
bool audio_capture_get_frame(uint8_t out[AUDIO_G722_FRAME_BYTES], TickType_t wait);
