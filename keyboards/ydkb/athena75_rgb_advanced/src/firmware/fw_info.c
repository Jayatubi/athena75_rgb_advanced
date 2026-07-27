// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "fw_info.h"
#include "config.h"
#include "sdk/host_api.h"
#include <string.h>

void fw_info_get(app_fw_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->build_num   = (uint32_t)FW_BUILD_NUM;
    out->app_abi     = (uint16_t)ATHENA_APP_ABI_VERSION;
    out->host_api_abi = (uint16_t)ATHENA_APP_ABI_VERSION;
    strncpy(out->board, "Athena75 RGB", sizeof(out->board) - 1u);
}
