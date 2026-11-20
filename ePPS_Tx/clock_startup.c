#include "clock_startup.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/ticks.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/structs/xosc.h"
#include "hardware/regs/xosc.h"
#include "hardware/regs/otp_data.h"

#if !PICO_RP2350
#error This clock startup is for RP2350 only
#endif

/* A coarse boot check, not an accurate frequency measurement. */
#define XIN_MIN_KHZ 8000u
#define XIN_MAX_KHZ 14000u
#define STARTUP_TIMEOUT_MS 50u
#define RETRY_DELAY_MS 500u

static uint32_t measured_xin_khz;
static uint32_t lposc_hz;

uint32_t clock_startup_xin_khz(void) {
    return measured_xin_khz;
}

/* TIMER0 counts LPOSC cycles during startup, not microseconds. Do not call
 * sleep_ms(), time_us_64(), or initialize USB until normal ticks are restored.
 */
static uint32_t startup_ticks(void) {
    return timer_hw->timerawl;
}

static bool timeout_ms(uint32_t start, uint32_t ms) {
    uint32_t ticks = (uint32_t)(((uint64_t)lposc_hz * ms + 999u) / 1000u);
    return (uint32_t)(startup_ticks() - start) >= ticks;
}

static void startup_wait_ms(uint32_t ms) {
    /* Give each bounded flash/delay a fresh watchdog budget, but do not
     * refresh inside the wait: a stopped timer must still cause a reset.
     */
    watchdog_update();
    uint32_t start = startup_ticks();
    while (!timeout_ms(start, ms)) tight_loop_contents();
}

static void retry_boot(unsigned int reason) {
    /* Show a visible fault code before the configured retry delay.
     * 1: XOSC not stable; 2: FC0 already busy; 3: FC0 measurement timeout;
     * 4: missing/stopped/out-of-range measured signal.
     */
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    startup_wait_ms(300);
    for (unsigned int i = 0; i < reason; ++i) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        startup_wait_ms(120);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        startup_wait_ms(180);
    }
    startup_wait_ms(RETRY_DELAY_MS);
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

static uint32_t get_lposc_hz(void) {
    uint16_t calibration = 0;
    otp_cmd_t cmd = {.flags = OTP_DATA_LPOSC_CALIB_ROW | OTP_CMD_ECC_BITS};
    int rc = rom_func_otp_access((uint8_t *)&calibration, sizeof calibration, cmd);
    /* Factory frequency at room temperature. No OTP data is written. */
    if (rc == 0 && calibration >= 20000u && calibration <= 50000u)
        return calibration;
    return 32768u;
}

/* This replaces the SDK's clock initialization BEFORE main(). A test inside
 * main() would be too late: default SDK startup waits for XOSC first.
 * The CPU stays on ROSC while LPOSC supplies an independent reference for
 * the counter and startup timeouts. Neither depends on XIN being present.
 */
void runtime_init_clocks(void) {
    /* CMake disables the GPIO coprocessor: per-core coprocessor setup runs
     * after this hardware initializer. GPIO must use SIO register accesses.
     */
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    /* Visible entry marker even if a later check immediately rejects XIN.
     * No timer or external reference is required for this approximate delay.
     */
    busy_wait_at_least_cycles(24000000u);
    clocks_hw->resus.ctrl = 0;
    /* Match the ROSC-only diagnostic that has now run on the board: make
     * clk_ref live before the SDK temporarily routes clk_sys through it.
     */
    clock_configure_undivided(clk_ref,
        CLOCKS_CLK_REF_CTRL_SRC_VALUE_ROSC_CLKSRC_PH, 0, 12000000u);
    clock_configure_undivided(clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_ROSC_CLKSRC, 12000000u);

    lposc_hz = get_lposc_hz();
    clock_configure_undivided(clk_ref,
        CLOCKS_CLK_REF_CTRL_SRC_VALUE_LPOSC_CLKSRC, 0, lposc_hz);
    tick_start(TICK_TIMER0, 1);
    tick_start(TICK_WATCHDOG, 1);

    /* 100000 LPOSC ticks: roughly 2..5 seconds. This is a startup-only
     * guard against a stalled PLL/mux. Restore normal ticks before main(). */
    watchdog_enable(100, false);

    /* Equivalent XOSC enable sequence, but with a bounded wait. The regular
     * xosc_init() waits forever if no external clock is present.
     * 39 * 256 cycles is approximately 1 ms at 10 MHz.
     */
    xosc_hw->ctrl = XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ;
    xosc_hw->startup = 39;
    hw_set_bits(&xosc_hw->ctrl,
        XOSC_CTRL_ENABLE_VALUE_ENABLE << XOSC_CTRL_ENABLE_LSB);
    uint32_t start = startup_ticks();
    while (!(xosc_hw->status & XOSC_STATUS_STABLE_BITS)) {
        if (timeout_ms(start, STARTUP_TIMEOUT_MS)) retry_boot(1);
    }

    /* Hardware FC0, with a bounded wait even if XIN stops or is missing.
     * REF_KHZ is integer-valued; correct its truncation using LPOSC Hz and
     * retain the counter's five fractional result bits until conversion.
     */
    fc_hw_t *fc = &clocks_hw->fc0;
    start = startup_ticks();
    while (fc->status & CLOCKS_FC0_STATUS_RUNNING_BITS) {
        if (timeout_ms(start, STARTUP_TIMEOUT_MS)) retry_boot(2);
    }
    uint32_t reference_khz = lposc_hz / 1000u;
    fc->ref_khz = reference_khz;
    fc->min_khz = 0;
    fc->max_khz = CLOCKS_FC0_MAX_KHZ_BITS;
    fc->delay = 1;
    fc->interval = 10;
    fc->src = CLOCKS_FC0_SRC_VALUE_XOSC_CLKSRC;
    start = startup_ticks();
    while (!(fc->status & CLOCKS_FC0_STATUS_DONE_BITS)) {
        if (timeout_ms(start, STARTUP_TIMEOUT_MS)) retry_boot(3);
    }
    uint32_t result = fc->result;
    uint32_t status = fc->status;
    fc->src = CLOCKS_FC0_SRC_VALUE_NULL;
    measured_xin_khz = (uint32_t)(((uint64_t)result * lposc_hz) /
        ((uint64_t)reference_khz * 1000u * (1u << CLOCKS_FC0_RESULT_KHZ_LSB)));
    if ((status & CLOCKS_FC0_STATUS_DIED_BITS) ||
        measured_xin_khz < XIN_MIN_KHZ || measured_xin_khz > XIN_MAX_KHZ)
        retry_boot(4);

    /* 10 MHz * 160 / 5 / 4 = 80 MHz; VCO = 1600 MHz (SDK maximum).
     * 10 MHz * 120 / 5 / 5 = 48 MHz USB; VCO = 1200 MHz.
     */
    pll_init(pll_sys, PLL_SYS_REFDIV, PLL_SYS_VCO_FREQ_HZ,
             PLL_SYS_POSTDIV1, PLL_SYS_POSTDIV2);
    pll_init(pll_usb, PLL_USB_REFDIV, PLL_USB_VCO_FREQ_HZ,
             PLL_USB_POSTDIV1, PLL_USB_POSTDIV2);
    clock_configure_undivided(clk_ref,
        CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, 0, XOSC_HZ);
    clock_configure_undivided(clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, SYS_CLK_HZ);
    clock_configure_undivided(clk_usb, 0,
        CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_CLK_HZ);
    clock_configure_undivided(clk_adc, 0,
        CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_CLK_HZ);
    clock_configure_undivided(clk_peri, 0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, SYS_CLK_HZ);
    clock_configure_undivided(clk_hstx, 0,
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, SYS_CLK_HZ);

    watchdog_disable();
    for (int i = 0; i < (int)TICK_COUNT; ++i)
        tick_start((tick_gen_num_t)i, XOSC_HZ / 1000000u);
}
