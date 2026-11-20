#ifndef SFP_H
#define SFP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/i2c.h"

#define SFP_ADDR_INFO 0x50u
#define SFP_ADDR_DDMI 0x51u
#define SFP_USER_SIZE 120u

typedef struct {
    i2c_inst_t *i2c;
    unsigned int disable_pin;
    unsigned int timeout_us;
} sfp_t;

typedef struct {
    char vendor[17];
    char part_number[17];
    char revision[5];
    char serial[17];
} sfp_identity_t;

typedef struct {
    float temperature_c;
    float voltage_v;
    float laser_bias_uA;
    float tx_power_mW;
    float rx_power_mW;
    bool rx_power_is_average; /* Otherwise the module reports OMA. */
} sfp_diagnostics_t;

typedef enum {
    SFP_OK = 0,
    SFP_ERR_ARGUMENT,
    SFP_ERR_I2C,
    SFP_ERR_TIMEOUT,
    SFP_ERR_NO_DDM,
    SFP_ERR_CALIBRATION,
    SFP_ERR_ADDRESS_MODE,
    SFP_ERR_NOT_READY,
    SFP_ERR_VERIFY
} sfp_status_t;

/* I2C hardware/pins must be configured first. Toggles TX_DISABLE 0,1,0. */
void sfp_init(sfp_t *sfp, i2c_inst_t *i2c, unsigned int disable_pin);
void sfp_reset(sfp_t *sfp);
const char *sfp_error_string(sfp_status_t status);

/* Raw access, with 8-bit register offsets. Use writes only with known maps. */
sfp_status_t sfp_read_bytes(sfp_t *sfp, uint8_t address, uint8_t start,
                            uint8_t *data, size_t length);
sfp_status_t sfp_write_bytes(sfp_t *sfp, uint8_t address, uint8_t start,
                             const uint8_t *data, size_t length);
sfp_status_t sfp_get_identity(sfp_t *sfp, sfp_identity_t *identity);
/* Supports internally calibrated SFF-8472 modules with direct A2h access. */
sfp_status_t sfp_get_diagnostics(sfp_t *sfp, sfp_diagnostics_t *diagnostics);

/* Offset 0..119 is relative to A2h byte 128, page 0. Selects page 0 first.
 * Writes byte-by-byte, waits 10 ms per byte, and verifies the result.
 * Consult the module datasheet if a different write-cycle time is required.
 * An error can leave a partially completed write; there is no rollback.
 */
sfp_status_t sfp_read_user_area(sfp_t *sfp, size_t offset,
                               uint8_t *data, size_t length);
sfp_status_t sfp_write_user_area(sfp_t *sfp, size_t offset,
                                const uint8_t *data, size_t length);

#endif
