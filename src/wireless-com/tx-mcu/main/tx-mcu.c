/**
 * @file tx-mcu.c
 * @brief UDP transmitter example using ESP-IDF and FreeRTOS.
 *
 * This application connects an ESP32 to a WiFi network and periodically
 * transmits UDP packets to a remote node.
 *
 * Target:
 * - ESP32
 * - ESP-IDF 6.x
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

/* Sincronizado con tu red real */
#define WIFI_SSID                  "MSI 0507"
#define WIFI_PASSWORD              "12345678"

/* Sincronizado con la IP real que obtuvo tu receptor en el log anterior */
#define RECEIVER_IP_ADDRESS        "192.168.137.88"
#define RECEIVER_UDP_PORT          (3333U)

#define TX_TASK_STACK_SIZE         (4096U)
#define TX_TASK_PRIORITY           (5U)

#define WIFI_CONNECTED_BIT         BIT0
#define WIFI_FAIL_BIT              BIT1

static const char* TAG = "UDP_TX";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define MAXIMUM_RETRY              5

/**
 * @brief Event handler for WiFi and IP events.
 */
static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi iniciado. Conectando al AP...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Conectado al AP. Esperando IP...");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                if (s_retry_num < MAXIMUM_RETRY) {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGW(TAG, "Reintentando conexión al AP (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            }
            default:
                break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "IP Asignada: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Initializes WiFi in station mode.
 */
static void WifiInit(void)
{
    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t wifiInitCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&wifiInitCfg));

    wifi_config_t wifiConfig =
    {
        .sta =
        {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Bloquea la inicialización hasta asegurar el enlace de red */
    EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conexión WiFi lista para transmitir.");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Fallo crítico: No se pudo conectar al SSID: %s", WIFI_SSID);
    }
}

/**
 * @brief Periodically transmits UDP messages.
 */
static void TxTask(void* pvParameters)
{
    int32_t socketFd;
    int32_t bytesSent;
    struct sockaddr_in destinationAddress;
    uint32_t counter = 0U;
    char messageBuffer[64];

    (void)pvParameters;

    memset(&destinationAddress, 0, sizeof(destinationAddress));
    destinationAddress.sin_family = AF_INET;
    destinationAddress.sin_port = htons(RECEIVER_UDP_PORT);
    destinationAddress.sin_addr.s_addr = inet_addr(RECEIVER_IP_ADDRESS);

    socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socketFd < 0)
    {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Socket UDP de transmisión creado.");

    for (;;)
    {
        (void)snprintf(
            messageBuffer,
            sizeof(messageBuffer),
            "MSG:%lu",
            (unsigned long)counter);

        bytesSent = sendto(
            socketFd,
            messageBuffer,
            strlen(messageBuffer),
            0,
            (struct sockaddr*)&destinationAddress,
            sizeof(destinationAddress));

        if (bytesSent < 0)
        {
            ESP_LOGE(TAG, "sendto falló. código de error (errno)=%d", errno);
        }
        else
        {
            ESP_LOGI(TAG, "TX -> %s (%ld bytes) enviado a %s:%u",
                     messageBuffer,
                     (long)bytesSent,
                     RECEIVER_IP_ADDRESS,
                     RECEIVER_UDP_PORT);
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000U)); // Transmitir cada 1 segundo
    }
}

/**
 * @brief Application entry point.
 */
void app_main(void)
{
    BaseType_t taskStatus;
    esp_err_t status;

    ESP_LOGI(TAG, "Iniciando Transmisor UDP");

    status = nvs_flash_init();
    if ((status == ESP_ERR_NVS_NO_FREE_PAGES) || (status == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        status = nvs_flash_init();
    }
    ESP_ERROR_CHECK(status);

    /* Inicializar conexión de red */
    WifiInit();

    /* Lanzar hilo de transmisión periódica */
    taskStatus = xTaskCreate(
        TxTask,
        "TxTask",
        TX_TASK_STACK_SIZE,
        NULL,
        TX_TASK_PRIORITY,
        NULL);

    if (taskStatus != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create transmission task");
    }
    else
    {
        ESP_LOGI(TAG, "Transmission task created");
    }
}