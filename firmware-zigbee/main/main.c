#include <stdint.h>
#include <inttypes.h>
#include "esp_timer.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zha.h"
#include "ezbee/zcl/cluster/window_covering.h"

#define COVER_COUNT 4
#define COVER_EP_BASE 10
#define COMMAND_QUEUE_LENGTH 8
#define ZIGBEE_STORAGE_PARTITION "nvs"

static const char *TAG = "PFX_ZB_SED";

typedef struct {
    uint8_t endpoint;
    uint8_t command;
    int64_t received_us;
} cover_command_t;

static QueueHandle_t s_command_queue;
static TaskHandle_t s_retry_task;

static const char *command_name(uint8_t command)
{
    switch (command) {
    case EZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN_ID:
        return "OPEN";
    case EZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE_ID:
        return "CLOSE";
    case EZB_ZCL_CMD_WINDOW_COVERING_STOP_ID:
        return "STOP";
    default:
        return "UNSUPPORTED";
    }
}

static void command_worker(void *arg)
{
    cover_command_t command;

    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "SIMULATION endpoint=%u command=%s received_us=%" PRId64 "; no RF transmission",
                     command.endpoint, command_name(command.command), command.received_us);
        }
    }
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id != EZB_ZCL_CORE_WINDOW_COVERING_MOVEMENT_CB_ID || message == NULL) {
        return;
    }

    ezb_zcl_window_covering_movement_message_t *movement = message;
    uint8_t command_id = movement->in.header->cmd_id;
    bool supported = command_id == EZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN_ID ||
                     command_id == EZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE_ID ||
                     command_id == EZB_ZCL_CMD_WINDOW_COVERING_STOP_ID;

    if (!supported || movement->info.dst_ep < COVER_EP_BASE ||
        movement->info.dst_ep >= COVER_EP_BASE + COVER_COUNT) {
        movement->out.result = EZB_ZCL_STATUS_UNSUP_CMD;
        ESP_LOGW(TAG, "Rejected Window Covering command 0x%02x", command_id);
        return;
    }

    cover_command_t command = {
        .endpoint = movement->info.dst_ep,
        .command = command_id,
        .received_us = esp_timer_get_time(),
    };

    if (xQueueSend(s_command_queue, &command, 0) == pdTRUE) {
        movement->out.result = EZB_ZCL_STATUS_SUCCESS;
    } else {
        movement->out.result = EZB_ZCL_STATUS_INSUFFICIENT_SPACE;
        ESP_LOGW(TAG, "Command queue full; endpoint=%u command=%s",
                 command.endpoint, command_name(command.command));
    }
}

static esp_err_t create_cover_endpoints(void)
{
    ezb_af_device_desc_t device_desc = ezb_af_create_device_desc();
    if (device_desc == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0; index < COVER_COUNT; index++) {
        uint8_t endpoint_id = COVER_EP_BASE + index;
        ezb_zha_window_covering_config_t config = EZB_ZHA_WINDOW_COVERING_CONFIG();
        config.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
        config.window_covering_cfg.window_covering_type =
            EZB_ZCL_WINDOW_COVERING_WINDOW_COVERING_TYPE_ROLLERSHADE;

        ezb_af_ep_desc_t endpoint = ezb_zha_create_window_covering(endpoint_id, &config);
        if (endpoint == NULL) {
            return ESP_ERR_NO_MEM;
        }

        ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(
            endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
        ezb_zcl_basic_cluster_desc_add_attr(
            basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0c" "OpenProfalux");
        ezb_zcl_basic_cluster_desc_add_attr(
            basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x0c" "ZigbeeBridge");

        /* No sensor: keep every position attribute absent, including helper defaults. */
        ezb_zcl_cluster_desc_t cover = ezb_af_endpoint_get_cluster_desc(
            endpoint, EZB_ZCL_CLUSTER_ID_WINDOW_COVERING, EZB_ZCL_CLUSTER_SERVER);
        const uint16_t positions[] = {0x0003, 0x0004, 0x0008, 0x0009};
        for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
            ezb_zcl_attr_desc_t attr = ezb_zcl_cluster_remove_attr_desc(cover, positions[i], EZB_ZCL_STD_MANUF_CODE);
            if (attr != NULL) {
                ezb_zcl_free_attr_desc(attr);
            }
        }
        ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device_desc, endpoint));
    }

    ESP_ERROR_CHECK(ezb_af_device_desc_register(device_desc));
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    return ESP_OK;
}

/* Blocked when joined: no periodic application wakeups. SDK calls use its lock. */
static void commissioning_retry_worker(void *arg)
{
    uint32_t mode;
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &mode, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (esp_zigbee_lock_acquire(portMAX_DELAY)) {
            esp_err_t err = ezb_bdb_start_top_level_commissioning(mode);
            esp_zigbee_lock_release();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Commissioning retry deferred: %s", esp_err_to_name(err));
                xTaskNotify(s_retry_task, mode, eSetValueWithOverwrite);
            }
        }
    }
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_ERROR_CHECK(ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION));
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status =
            *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "BDB startup status=0x%02x factory_new=%d", status, ezb_bdb_is_factory_new());
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                ESP_ERROR_CHECK(ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING));
            } else {
                ESP_LOGI(TAG, "Restored network, channel=%u address=0x%04x",
                         ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            }
        } else {
            xTaskNotify(s_retry_task, EZB_BDB_MODE_INITIALIZATION, eSetValueWithOverwrite);
        }
        break;
    }
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status =
            *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "Network steering %s", status == EZB_BDB_STATUS_SUCCESS ? "complete" : "failed");
        if (status != EZB_BDB_STATUS_SUCCESS) {
            xTaskNotify(s_retry_task, EZB_BDB_MODE_NETWORK_STEERING, eSetValueWithOverwrite);
        }
        break;
    }
    default:
        break;
    }

    return true;
}

static void zigbee_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,
                .keep_alive = CONFIG_OPF_ZB_POLL_INTERVAL_MS,
            },
        },
        .platform_config = {
            .storage_partition_name = ZIGBEE_STORAGE_PARTITION,
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ezb_nwk_set_rx_on_when_idle(false);
    ESP_ERROR_CHECK(create_cover_endpoints());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t nvs_status = nvs_flash_init();
    /* Fail closed: never erase network credentials on an initialization error. */
    ESP_ERROR_CHECK(nvs_status);

    esp_pm_config_t power_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&power_config));

    ESP_LOGI(TAG, "SED simulation, endpoints 10..13, polling=%d ms", CONFIG_OPF_ZB_POLL_INTERVAL_MS);

    s_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(cover_command_t));
    configASSERT(s_command_queue != NULL);
    configASSERT(xTaskCreate(command_worker, "cover_commands", 2048, NULL, 4, NULL) == pdPASS);
    configASSERT(xTaskCreate(commissioning_retry_worker, "zb_retry", 3072, NULL, 3, &s_retry_task) == pdPASS);
    configASSERT(xTaskCreate(zigbee_task, "zigbee_main", 4096, NULL, 5, NULL) == pdPASS);
}
