/*
 * External-clock Pico 2: remove the onboard crystal and drive XIN with
 * the received encoded 10 MHz clock. Establish the clock and external board/SFP power
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
#include "picoDEC.pio.h"
#include "picoSQ.pio.h"
#include "picoPOFF.pio.h"


#define SFP_I2C i2c1
#define SFP_SDA_PIN 10
#define SFP_SCL_PIN 11
#define SFP_DISABLE_PIN 7

#define FREQ_IN_GPIO 21         // XOSC clock mirror output, sampled internally by the PIO decoder.
#define FREQ_OUT_GPIO 18        // PIO-regenerated 10 MHz output, 50% duty cycle.
#define PPS_OUT_GPIO 19         // Phase-adjusted PPS output.
#define SYNC_OUT_GPIO 22        // Raw decoded PPS marker feeding the phase-delay state machines.
#define PHASE_PRECEDE_CYCLES (80000000u-20u) // Nominal delay: 1 s minus 250 ns at 80 MHz; calibrate for the link.
#define OUTPUT_WIDTH_CYCLES  (400000u)    // 5ms

void configure_gpio() {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    gpio_init(FREQ_IN_GPIO);
    gpio_set_dir(FREQ_IN_GPIO, GPIO_OUT);
    gpio_init(FREQ_OUT_GPIO);
    gpio_set_dir(FREQ_OUT_GPIO, GPIO_OUT);
    gpio_init(PPS_OUT_GPIO);
    gpio_set_dir(PPS_OUT_GPIO, GPIO_OUT);
    gpio_init(SYNC_OUT_GPIO);
    gpio_set_dir(SYNC_OUT_GPIO, GPIO_OUT);

    gpio_set_function(SFP_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SFP_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SFP_SDA_PIN);
    gpio_pull_up(SFP_SCL_PIN);
}

void demodulate(PIO pio, uint sm, uint offset, uint freq_in, uint sync_pin) {
    pio_gpio_init(pio, sync_pin);
    pio_sm_set_consecutive_pindirs(pio, sm, sync_pin, 1, true);
    pio_sm_config c = picodec_program_get_default_config(offset);
    sm_config_set_in_pins(&c, freq_in);
    sm_config_set_jmp_pin(&c, freq_in);
    sm_config_set_set_pins(&c, sync_pin, 1);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

void square(PIO pio, uint sm, uint offset, uint out_pin) {
    pio_gpio_init(pio, out_pin);
    pio_sm_set_consecutive_pindirs(pio, sm, out_pin, 1, true);
    pio_sm_config c = picosq_program_get_default_config(offset);    
    sm_config_set_set_pins(&c, out_pin, 1);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

void pulseoffset(PIO pio, uint sm1, uint sm2, uint offset, uint sync_pin, uint out_pin) {
    pio_gpio_init(pio, out_pin);
    gpio_set_input_enabled(sync_pin, true);

    uint sms[2] = {sm1, sm2};
    for (uint i = 0; i < 2; ++i) {
        uint sm = sms[i];
    
        pio_sm_set_consecutive_pindirs(pio, sm, out_pin, 1, true);
        pio_sm_config c = picopulseoffset_program_get_default_config(offset);
        sm_config_set_set_pins(&c, out_pin, 1);
        sm_config_set_in_pins(&c, sync_pin);
        // Allow either SM to update the shared output pin
        sm_config_set_out_special(&c, false, false, 0);
        pio_sm_init(pio, sm, offset, &c);
        pio_sm_put_blocking(pio, sm, PHASE_PRECEDE_CYCLES-3);
        pio_sm_put_blocking(pio, sm, OUTPUT_WIDTH_CYCLES-3);
        pio_sm_put_blocking(pio, sm, i);
    }
    pio_enable_sm_mask_in_sync(pio, (1u<<sm1) | (1u<<sm2));
}

void configure_pios() {
    // Load the PIO programs.
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &picodec_program);

    // Configure the state machines and GPIO assignments.
    demodulate(pio, 0, offset, FREQ_IN_GPIO, SYNC_OUT_GPIO);  

    offset = pio_add_program(pio, &picosq_program);
    square(pio, 1, offset, FREQ_OUT_GPIO);  

    pio = pio1;
    offset = pio_add_program(pio, &picopulseoffset_program);
    pulseoffset(pio, 2, 3, offset, SYNC_OUT_GPIO, PPS_OUT_GPIO);
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

    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    configure_gpio();
    configure_pios();
    clock_gpio_init(FREQ_IN_GPIO, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_XOSC_CLKSRC, 1.0f);

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
        wait_gpio_high(SYNC_OUT_GPIO, 3000);      // wait for PPS input, timeout after 3 seconds
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        printf(".");
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
