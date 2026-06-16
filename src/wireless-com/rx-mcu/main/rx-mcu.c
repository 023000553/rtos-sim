/**
 * @file rx-mcu.c
 * @brief UDP receiver example using ESP-IDF and FreeRTOS.
 *
 * This application connects an ESP32 to a WiFi network and listens
 * for incoming UDP packets from a remote node.
 *
 * Target:
 * - ESP32
 * - ESP-IDF 6.0
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

#define WIFI_SSID                  "MSI 0507"
#define WIFI_PASSWORD              "12345678"

#define LOCAL_UDP_PORT             (3333U)

#define RX_TASK_STACK_SIZE         (4096U)
#define RX_TASK_PRIORITY           (5U)

/* Bits del grupo de eventos para el estado de la conexión */
#define WIFI_CONNECTED_BIT         BIT0
#define WIFI_FAIL_BIT              BIT1

static const char* TAG = "UDP_RX";

/* Grupo de eventos de FreeRTOS para sincronizar la IP */
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
                ESP_LOGI(TAG, "WiFi iniciado. Conectando...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Conectado al Punto de Acceso (AP). Esperando IP...");
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
        ESP_LOGI(TAG, "IP Asignada exitosamente: " IPSTR, IP2STR(&event->ip_info.ip));
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
            /* Configuración para asegurar compatibilidad alta */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Espera de forma síncrona y eficiente hasta que los eventos ocurran */
    EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conexión WiFi establecida con éxito.");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Fallo crítico: No se pudo conectar al SSID: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Error inesperado esperando eventos de conexión");
    }
}

/**
 * @brief Receives UDP packets and prints their contents.
 */
static void RxTask(void* pvParameters)
{
    int32_t socketFd;
    int32_t receivedBytes;
    struct sockaddr_in localAddress;
    struct sockaddr_in sourceAddress;
    socklen_t sourceAddressLength;
    char receiveBuffer[128];

    (void)pvParameters;

    memset(&localAddress, 0, sizeof(localAddress));
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(LOCAL_UDP_PORT);
    localAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socketFd < 0)
    {
        ESP_LOGE(TAG, "No se pudo crear el socket UDP");
        vTaskDelete(NULL);
    }

    if (bind(socketFd, (struct sockaddr*)&localAddress, sizeof(localAddress)) < 0)
    {
        ESP_LOGE(TAG, "Error en el bind del socket UDP");
        (void)close(socketFd);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Escuchando en puerto UDP: %u", (unsigned int)LOCAL_UDP_PORT);

    for (;;)
    {
        sourceAddressLength = sizeof(sourceAddress);

        receivedBytes = recvfrom(
            socketFd,
            receiveBuffer,
            sizeof(receiveBuffer) - 1U,
            0,
            (struct sockaddr*)&sourceAddress,
            &sourceAddressLength);

        if (receivedBytes < 0)
        {
            ESP_LOGE(TAG, "Error al recibir paquete UDP");
            continue;
        }

        receiveBuffer[receivedBytes] = '\0';

        ESP_LOGI(TAG, "RX <- %s desde %s:%u",
                 receiveBuffer,
                 inet_ntoa(sourceAddress.sin_addr),
                 ntohs(sourceAddress.sin_port));
    }
}

/**
 * @brief Application entry point.
 */
void app_main(void)
{
    BaseType_t taskStatus;
    esp_err_t status;

    ESP_LOGI(TAG, "Iniciando Receptor UDP");

    status = nvs_flash_init();
    if ((status == ESP_ERR_NVS_NO_FREE_PAGES) || (status == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        status = nvs_flash_init();
    }
    ESP_ERROR_CHECK(status);

    /* Inicializar e intentar bloquear la ejecución hasta tener red */
    WifiInit();

    /* Crear la tarea UDP */
    taskStatus = xTaskCreate(
        RxTask,
        "RxTask",
        RX_TASK_STACK_SIZE,
        NULL,
        RX_TASK_PRIORITY,
        NULL);

    if (taskStatus != pdPASS)
    {
        ESP_LOGE(TAG, "Error creando la tarea de recepción");
    }
    else
    {
        ESP_LOGI(TAG, "Tarea de recepción UDP creada exitosamente");
    }
}