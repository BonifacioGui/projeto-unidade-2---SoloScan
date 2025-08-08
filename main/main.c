#include <stdio.h>
#include <string.h>
#include <ctype.h>            // para isalnum
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" // Necessário para o sistema de eventos
#include "driver/gpio.h"
#include "driver/adc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

// --- Suas Configurações ---
#define WIFI_SSID           "PROXXIMA- Guilherme --286460-2G"
#define WIFI_PASS           "Virus020695"
#define TELEGRAM_TOKEN      "8465024409:AAGLsUPJ80Rz6irB57jPBG2WK17jr4v-3mc"
#define TELEGRAM_CHAT_ID    "6106877868"
#define THRESHOLD_SECO      3000
// -------------------------

#define SENSOR_PIN          ADC1_CHANNEL_6 // GPIO34
#define LED_PIN             GPIO_NUM_2

static const char *TAG = "TELEGRAM_BOT";

// --- Sistema de Eventos para aguardar a conexão Wi-Fi ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
// --- Fim do sistema ---

// Função para url encode simples
void url_encode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0;
    while (*src && i + 4 < dst_len) { // +4 para garantir espaço para %XX
        if (isalnum((unsigned char)*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            dst[i++] = *src;
        } else if (*src == ' ') {
            dst[i++] = '+';  // opcional: pode usar + para espaços
        } else {
            i += snprintf(&dst[i], 4, "%%%02X", (unsigned char)*src);
        }
        src++;
    }
    dst[i] = '\0';
}

// Nova função que "ouve" os eventos do Wi-Fi
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "Falha ao conectar ao Wi-Fi. Tentando novamente...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! Endereço IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Função de inicialização do Wi-Fi (agora mais robusta)
void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, }, };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Esperando pela conexão Wi-Fi...");

    // Espera até conectar no Wi-Fi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// Função para enviar mensagem ao Telegram com encode da mensagem
void telegram_send_message(const char* message) {
    char encoded_msg[512];
    char url[1024];

    // Codifica mensagem para URL segura
    url_encode(message, encoded_msg, sizeof(encoded_msg));

    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
        TELEGRAM_TOKEN, TELEGRAM_CHAT_ID, encoded_msg);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Mensagem enviada para o Telegram com sucesso!");
    } else {
        ESP_LOGE(TAG, "Erro ao enviar mensagem para o Telegram: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

// Função principal
void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    wifi_init_sta(); // Só retorna após conexão Wi-Fi

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_PIN, ADC_ATTEN_DB_11);

    telegram_send_message("✅ Dispositivo conectado e pronto para monitorar a planta!");

    bool ultimo_estado_seco = false;

    while (1) {
        int valor_umidade = adc1_get_raw(SENSOR_PIN);
        ESP_LOGI(TAG, "Leitura do sensor: %d", valor_umidade);

        if (valor_umidade >= THRESHOLD_SECO) {
            gpio_set_level(LED_PIN, 1);
            if (!ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta está seca! Enviando alerta...");
                telegram_send_message("🚨 Sua planta está seca! Hora de regar. 🌱");
                ultimo_estado_seco = true;
            }
        } else {
            gpio_set_level(LED_PIN, 0);
            if (ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta foi regada. Resetando alerta.");
                ultimo_estado_seco = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300000)); // 5 minutos
    }
}
