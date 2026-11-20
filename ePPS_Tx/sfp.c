#include "sfp.h"
#include <string.h>
#include "pico/stdlib.h"
#include "pico/error.h"

static sfp_status_t transfer_result(int result, size_t expected) {
    if (result == (int)expected) return SFP_OK;
    return result == PICO_ERROR_TIMEOUT ? SFP_ERR_TIMEOUT : SFP_ERR_I2C;
}

static bool valid_transfer(sfp_t *sfp, uint8_t address, uint8_t start,
                           const void *data, size_t length) {
    return sfp && sfp->i2c && sfp->timeout_us &&
           (address == SFP_ADDR_INFO || address == SFP_ADDR_DDMI) &&
           data && length > 0 && length <= 256u - (size_t)start;
}

void sfp_reset(sfp_t *sfp) {
    /* Preserves the Python sequence. TX_DISABLE is not a module reset pin. */
    gpio_put(sfp->disable_pin, 0);
    sleep_ms(200);
    gpio_put(sfp->disable_pin, 1);
    sleep_ms(200);
    gpio_put(sfp->disable_pin, 0);
    sleep_ms(200);
}

void sfp_init(sfp_t *sfp, i2c_inst_t *i2c, unsigned int disable_pin) {
    *sfp = (sfp_t){.i2c = i2c, .disable_pin = disable_pin,
                   .timeout_us = 100000};
    gpio_init(disable_pin);
    gpio_put(disable_pin, 0);
    gpio_set_dir(disable_pin, GPIO_OUT);
    sfp_reset(sfp);
}

sfp_status_t sfp_read_bytes(sfp_t *sfp, uint8_t address, uint8_t start,
                            uint8_t *data, size_t length) {
    if (!valid_transfer(sfp, address, start, data, length))
        return SFP_ERR_ARGUMENT;
    /* STOP between transfers matches the supplied MicroPython code. */
    int n = i2c_write_timeout_us(sfp->i2c, address, &start, 1, false,
                                 sfp->timeout_us);
    sfp_status_t status = transfer_result(n, 1);
    if (status != SFP_OK) return status;
    n = i2c_read_timeout_us(sfp->i2c, address, data, length, false,
                            sfp->timeout_us);
    return transfer_result(n, length);
}

sfp_status_t sfp_write_bytes(sfp_t *sfp, uint8_t address, uint8_t start,
                             const uint8_t *data, size_t length) {
    if (!valid_transfer(sfp, address, start, data, length))
        return SFP_ERR_ARGUMENT;
    uint8_t buffer[257];
    buffer[0] = start;
    memcpy(buffer + 1, data, length);
    int n = i2c_write_timeout_us(sfp->i2c, address, buffer, length + 1,
                                 false, sfp->timeout_us);
    /* Raw writes do not wait for EEPROM programming or split EEPROM pages. */
    return transfer_result(n, length + 1);
}

static void copy_ascii(char *dst, const uint8_t *src, size_t length) {
    size_t first = 0;
    while (first < length && (src[first] == ' ' || src[first] == '\t')) ++first;
    while (length > first && (src[length - 1] == ' ' ||
                              src[length - 1] == '\t' || src[length - 1] == 0))
        --length;
    size_t n = length - first;
    memcpy(dst, src + first, n);
    dst[n] = '\0';
}

sfp_status_t sfp_get_identity(sfp_t *sfp, sfp_identity_t *identity) {
    if (!identity) return SFP_ERR_ARGUMENT;
    uint8_t data[64]; /* A0h bytes 20 through 83. */
    sfp_status_t status = sfp_read_bytes(sfp, SFP_ADDR_INFO, 20, data, sizeof data);
    if (status != SFP_OK) return status;
    sfp_identity_t result = {0};
    copy_ascii(result.vendor, data, 16);
    copy_ascii(result.part_number, data + 20, 16);
    copy_ascii(result.revision, data + 36, 4);
    copy_ascii(result.serial, data + 48, 16);
    *identity = result;
    return SFP_OK;
}

static uint16_t be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static sfp_status_t get_ddm_type(sfp_t *sfp, uint8_t *type) {
    sfp_status_t status = sfp_read_bytes(sfp, SFP_ADDR_INFO, 92, type, 1);
    if (status != SFP_OK) return status;
    if (!(*type & 0x40)) return SFP_ERR_NO_DDM;
    if (*type & 0x04) return SFP_ERR_ADDRESS_MODE;
    return SFP_OK;
}

sfp_status_t sfp_get_diagnostics(sfp_t *sfp, sfp_diagnostics_t *diagnostics) {
    if (!diagnostics) return SFP_ERR_ARGUMENT;
    uint8_t type;
    sfp_status_t status = get_ddm_type(sfp, &type);
    if (status != SFP_OK) return status;
    if (!(type & 0x20) || (type & 0x10)) return SFP_ERR_CALIBRATION;

    uint8_t data[15]; /* Measurements 96..105, status at 110. */
    status = sfp_read_bytes(sfp, SFP_ADDR_DDMI, 96, data, sizeof data);
    if (status != SFP_OK) return status;
    if (data[14] & 1) return SFP_ERR_NOT_READY;

    /* Explicit sign extension avoids implementation-defined unsigned casts. */
    int32_t temperature = be16(data);
    if (temperature & 0x8000) temperature -= 65536;
    *diagnostics = (sfp_diagnostics_t){
        .temperature_c = temperature / 256.0f,
        .voltage_v = be16(data + 2) / 10000.0f,
        .laser_bias_uA = be16(data + 4) * 2.0f,
        .tx_power_mW = be16(data + 6) / 10000.0f,
        .rx_power_mW = be16(data + 8) / 10000.0f,
        .rx_power_is_average = (type & 0x08) != 0
    };
    return SFP_OK;
}

static sfp_status_t select_user_page(sfp_t *sfp) {
    uint8_t type, page;
    sfp_status_t status = get_ddm_type(sfp, &type);
    if (status != SFP_OK) return status;
    status = sfp_read_bytes(sfp, SFP_ADDR_DDMI, 127, &page, 1);
    if (status != SFP_OK || page == 0) return status;
    page = 0;
    status = sfp_write_bytes(sfp, SFP_ADDR_DDMI, 127, &page, 1);
    if (status != SFP_OK) return status;
    status = sfp_read_bytes(sfp, SFP_ADDR_DDMI, 127, &page, 1);
    if (status != SFP_OK) return status;
    return page == 0 ? SFP_OK : SFP_ERR_VERIFY;
}

static bool valid_user_range(size_t offset, const void *data, size_t length) {
    return data && offset < SFP_USER_SIZE && length > 0 &&
           length <= SFP_USER_SIZE - offset;
}

sfp_status_t sfp_read_user_area(sfp_t *sfp, size_t offset,
                               uint8_t *data, size_t length) {
    if (!valid_user_range(offset, data, length)) return SFP_ERR_ARGUMENT;
    sfp_status_t status = select_user_page(sfp);
    if (status != SFP_OK) return status;
    return sfp_read_bytes(sfp, SFP_ADDR_DDMI, (uint8_t)(128 + offset), data, length);
}

sfp_status_t sfp_write_user_area(sfp_t *sfp, size_t offset,
                                const uint8_t *data, size_t length) {
    if (!valid_user_range(offset, data, length)) return SFP_ERR_ARGUMENT;
    sfp_status_t status = select_user_page(sfp);
    if (status != SFP_OK) return status;
    for (size_t i = 0; i < length; ++i) {
        /* Single-byte writes avoid module-dependent EEPROM page boundaries. */
        uint8_t address = (uint8_t)(128 + offset + i);
        status = sfp_write_bytes(sfp, SFP_ADDR_DDMI, address, data + i, 1);
        if (status != SFP_OK) return status;
        sleep_ms(10);
        uint8_t actual;
        status = sfp_read_bytes(sfp, SFP_ADDR_DDMI, address, &actual, 1);
        if (status != SFP_OK) return status;
        if (actual != data[i]) return SFP_ERR_VERIFY;
    }
    return SFP_OK;
}

const char *sfp_error_string(sfp_status_t status) {
    switch (status) {
    case SFP_OK: return "OK";
    case SFP_ERR_ARGUMENT: return "invalid argument or address range";
    case SFP_ERR_I2C: return "I2C transfer failed (NACK or short transfer)";
    case SFP_ERR_TIMEOUT: return "I2C timeout";
    case SFP_ERR_NO_DDM: return "module does not advertise digital diagnostics";
    case SFP_ERR_CALIBRATION: return "external or unspecified calibration unsupported";
    case SFP_ERR_ADDRESS_MODE: return "legacy address-change sequence unsupported";
    case SFP_ERR_NOT_READY: return "diagnostic data not ready";
    case SFP_ERR_VERIFY: return "EEPROM/page write verification failed";
    default: return "unknown error";
    }
}
