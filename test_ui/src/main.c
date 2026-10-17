/* Test materiel : LED clignote 5 s au demarrage puis s'eteint ;
 * 1 cran a droite = flash court (80 ms), 1 cran a gauche = flash long (600 ms).
 * Encodeur A = GPIO1 (D0), B = GPIO2 (D1) avec rappels internes ; LED = GPIO8 (D9), actif haut. */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define PIN_A    1
#define PIN_B    2
#define PIN_LED  8
#define FLASH_SHORT_MS 80
#define FLASH_LONG_MS  600

static int8_t quad_step(uint8_t prev, uint8_t cur)
{
    /* Table de quadrature : index (prev<<2)|cur, +1 horaire, -1 anti-horaire, 0 invalide/identique */
    static const int8_t t[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    return t[(prev << 2) | cur];
}

void app_main(void)
{
    gpio_config_t in = { .pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B), .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    gpio_config_t out = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);

    printf("test_ui: LED clignote 5 s\n");
    for (int i = 0; i < 25; i++) {            /* 5 s : 0,1 s allume / 0,1 s eteint */
        gpio_set_level(PIN_LED, 1); vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_LED, 0); vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("test_ui: pret, tourner l'encodeur\n");

    uint8_t prev = (gpio_get_level(PIN_A) << 1) | gpio_get_level(PIN_B);
    int acc = 0;
    int64_t led_off_us = 0;
    for (;;) {
        uint8_t cur = (gpio_get_level(PIN_A) << 1) | gpio_get_level(PIN_B);
        if (cur != prev) {
            acc += quad_step(prev, cur);
            prev = cur;
            if (cur == 3) {                    /* position de repos : un cran complet */
                int64_t now = esp_timer_get_time();
                if (acc >= 2) {
                    printf("cran DROITE (flash court)\n");
                    gpio_set_level(PIN_LED, 1); led_off_us = now + FLASH_SHORT_MS * 1000;
                } else if (acc <= -2) {
                    printf("cran GAUCHE (flash long)\n");
                    gpio_set_level(PIN_LED, 1); led_off_us = now + FLASH_LONG_MS * 1000;
                }
                acc = 0;
            }
        }
        if (led_off_us && esp_timer_get_time() >= led_off_us) { gpio_set_level(PIN_LED, 0); led_off_us = 0; }
        vTaskDelay(1);
    }
}
