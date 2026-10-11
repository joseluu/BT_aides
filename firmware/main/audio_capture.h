#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Trame G.722 64 kbit/s de 20 ms : 320 echantillons a 16 kHz -> 160 octets. */
#define AUDIO_G722_FRAME_BYTES 160

/* Trames des deux oreilles pour la meme tranche de 20 ms. */
struct audio_frame_pair {
    uint8_t left[AUDIO_G722_FRAME_BYTES];
    uint8_t right[AUDIO_G722_FRAME_BYTES];
};

/* Demarre la chaine PCM1808 (I2S esclave 48 kHz, stereo) -> decimation
 * 16 kHz -> deux encodeurs G.722, dans une tache epinglee sur le coeur 1.
 * Demarre en mode mono. */
esp_err_t audio_capture_start(void);

/* A appeler a chaque (re)demarrage du flux ASHA : vide la file de trames et
 * reinitialise les deux encodeurs a la prochaine frontiere de trame (les
 * decodeurs des aides repartent de zero). */
void audio_capture_restart_stream(void);

/* Abaisse lineairement le niveau des deux voies jusqu'au silence en `ms`
 * millisecondes, avant un STOP : l'aide coupe net au STOP mais remonte le son
 * en douceur au START. Le gain revient a 1 au prochain
 * audio_capture_restart_stream(). */
void audio_capture_fade_out(uint32_t ms);

/* true : voie gauche -> trame gauche, voie droite -> trame droite (deux aides).
 * false : les deux trames portent le melange (G+D)/2 (une seule aide).
 * Applique a la prochaine frontiere de trame. */
void audio_capture_set_stereo(bool stereo);

/* Recupere la paire de trames suivante ; false si rien n'arrive dans le delai. */
bool audio_capture_get_frame_pair(struct audio_frame_pair *out, TickType_t wait);
