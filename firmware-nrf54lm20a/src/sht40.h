/* sht40.h — SHT40 temperature + humidity (external, XIAO I²C header). */
#pragma once

#include "measurement.h"

/* Read the SHT40 into m (sets m->sht_ok). The sensor is located on first use
 * by probing address SHT40_ADDR on every enabled I²C bus. */
void sht40_read(struct measurement *m);
