#ifndef CLOCK_STARTUP_H
#define CLOCK_STARTUP_H

#include <stdint.h>

/* Approximate boot-time measurement, referenced to the internal LPOSC. */
uint32_t clock_startup_xin_khz(void);

#endif
