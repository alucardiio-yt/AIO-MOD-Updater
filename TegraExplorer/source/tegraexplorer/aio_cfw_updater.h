#pragma once

#include <utils/types.h>

bool aio_cfw_update_pending(void);
void aio_cfw_update_run(bool patched_or_mariko);
