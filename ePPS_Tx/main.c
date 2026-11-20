/*
 * External-clock Pico 2: remove the onboard crystal and drive XIN with
 * the source 10 MHz reference. Establish the clock and external board/SFP power
 * before connecting USB for diagnostics.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/divider.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/xosc.h"
#include "clock_startup.h"
#include "sfp.h"
#include "picoENC.pio.h"


#define SFP_I2C i2c1
#define SFP_SDA_PIN 10
#define SFP_SCL_PIN 11
#define SFP_DISABLE_PIN 7

#define PPS_IN_GPIO 16              // PPS input sampled through the configured PIO JMP pin.
#define OUT_GPIO 17
//#define PPS_SIM_GPIO 19


void blink_pin_forever(PIO pio, uint sm, uint offset, uint out_pin, uint pps_pin) {
    pio_gpio_init(pio, out_pin);
    pio_sm_set_consecutive_pindirs(pio, sm, out_pin, 1, true);
    pio_sm_config c = picoenc_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, out_pin);
    sm_config_set_jmp_pin(&c, pps_pin);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}


void configure_gpio() {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_init(PPS_IN_GPIO);
    gpio_set_dir(PPS_IN_GPIO, GPIO_IN);
    gpio_init(OUT_GPIO);
    gpio_set_dir(OUT_GPIO, GPIO_OUT);
    #ifdef PPS_SIM_GPIO
        gpio_init(PPS_SIM_GPIO);
        gpio_set_dir(PPS_SIM_GPIO, GPIO_OUT);
    #endif

    gpio_set_function(SFP_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SFP_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SFP_SDA_PIN);
    gpio_pull_up(SFP_SCL_PIN);
}

void configure_pios() {
    // Load the PIO programs.
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &picoenc_program);

    // Configure the state machines and GPIO assignments.
    blink_pin_forever(pio, 0, offset, OUT_GPIO, PPS_IN_GPIO);  
}

bool wait_gpio_high(uint gpio, uint32_t timeout_ms)
{
    uint64_t start = time_us_64();

    while (!gpio_get(gpio)) {
        if (time_us_64() - start >= (uint64_t)timeout_ms * 1000u)
            return false;

        tight_loop_contents();
    }

    return true;
}

int main(void) {
    sfp_t sfp;
    sfp_identity_t identity;
    sfp_status_t status;
    sfp_diagnostics_t diagnostics;
    bool have_identity = false;

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    configure_gpio();
    configure_pios();

    sleep_ms(2000);   /* Allow time for USB serial enumeration. */
    i2c_init(SFP_I2C, 100000);
    sfp_init(&sfp, SFP_I2C, SFP_DISABLE_PIN);

    printf("\n# SFP reader started\n");
    printf("# XIN coarse measurement: %lu kHz; CPU: 80 MHz\n", (unsigned long)clock_startup_xin_khz());

    /* Optional one-time EEPROM write, disabled by default:
     * static const uint8_t message[] = "ABC123TEST";
     * sfp_status_t status = sfp_write_user_area(&sfp, 0, message, sizeof message - 1);
     * printf("EEPROM write: %s\n", sfp_error_string(status));
     * Offset 0 means byte 128 at 0x51, not byte 0 at 0x50.
     */

    while (true) {
        #ifdef PPS_SIM_GPIO
            gpio_put(PPS_SIM_GPIO, 1);
        #endif
        wait_gpio_high(PPS_IN_GPIO, 3000);      // wait for PPS input, timeout after 3 seconds
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        #ifdef PPS_SIM_GPIO
            gpio_put(PPS_SIM_GPIO, 0);
        #endif

        if (!(have_identity)) {
            status = sfp_get_identity(&sfp, &identity);
            printf("SFP Identity:\n");
            if (status == SFP_OK) {
                printf("  vendor: %s\n", identity.vendor);
                printf("  part_number: %s\n", identity.part_number);
                printf("  revision: %s\n", identity.revision);
                printf("  serial: %s\n", identity.serial);
                have_identity = true;
            } else {
                printf("  Error: %s\n", sfp_error_string(status));
            }
        }
        
        status = sfp_get_diagnostics(&sfp, &diagnostics);
        printf("\nDiagnostics:\n");
        if (status == SFP_OK) {
            printf("  temperature_c: %.3f\n", (double)diagnostics.temperature_c);
            printf("  voltage_v: %.4f\n", (double)diagnostics.voltage_v);
            printf("  laser_bias_uA: %.0f\n", (double)diagnostics.laser_bias_uA);
            printf("  tx_power_mW: %.4f\n", (double)diagnostics.tx_power_mW);
            printf("  rx_power_mW: %.4f (%s)\n", (double)diagnostics.rx_power_mW,
                   diagnostics.rx_power_is_average ? "average" : "OMA");
        } else {
            printf("  Error: %s\n", sfp_error_string(status));
        }
        printf("\n");
        sleep_ms(50);   // make the LED blink visible
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
    }
}
