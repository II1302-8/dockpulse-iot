#include "dp_common.h"

#include "sdkconfig.h"

esp_err_t dp_common_get_node_id(uint8_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (uint8_t)CONFIG_DOCKPULSE_NODE_ID;
    return ESP_OK;
}
