#pragma once

#include "hive_config.h"

#include <stdbool.h>

void ota_init(void);
bool ota_is_active(void);

#if ENABLE_THROUGHPUT_SPIKE
/* Stand down the six-second "connected but never sent BEGIN" guard for the
 * current link. Only the throughput spike (src/throughput.c) calls this: its
 * measurement runs for tens of seconds on a connection that legitimately never
 * sends BEGIN, and the guard would drop the link mid-run. Compiled out of every
 * normal build, so the guard is unconditional in anything that ships. */
void ota_release_arm_timeout(void);
#endif
