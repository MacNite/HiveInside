#pragma once

#include "hive_config.h"

#include <stdbool.h>

void ota_init(void);
bool ota_is_active(void);
void ota_link_connected(void);
void ota_link_disconnected(void);
