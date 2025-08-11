#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

// --- Suas Configurações ---
#define WIFI_SSID           "PROXXIMA- Guilherme --286460-2G"
#define WIFI_PASS           "Virus020695"
#define THRESHOLD_SECO      3000

// --- Configurações Telegram ---
#define TELEGRAM_TOKEN      "8465024409:AAGLsUPJ80Rz6irB57jPBG2WK17jr4v-3mc"
#define TELEGRAM_CHAT_ID    "6106877868"

// --- Configurações MQTT ---
#define MQTT_BROKER_URL     "mqtt://broker.hivemq.com"
#define MQTT_BASE_TOPIC     "soloscan/planta"
#define MQTT_TOPIC_STATUS   MQTT_BASE_TOPIC "/status"
#define MQTT_TOPIC_LEITURA  MQTT_BASE_TOPIC "/leitura_raw" // Renomeado para indicar valor bruto
#define MQTT_TOPIC_PERCENT  MQTT_BASE_TOPIC "/umidade_percentual" // ✅ NOVO TÓPICO
#define MQTT_TOPIC_ALERTA   MQTT_BASE_TOPIC "/alerta"

// ✅ NOVAS CONFIGURAÇÕES DE CALIBRAÇÃO
#define SENSOR_MIN_MOLHADO  1099 // O valor que você mediu na água
#define SENSOR_MAX_SECO     4095 // O valor que você mediu no ar
// -------------------------

#define SENSOR_PIN          ADC1_CHANNEL_6 // GPIO34
#define LED_PIN             GPIO_NUM_2

static const char *TAG = "SOLOSCAN_HYBRID";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static esp_mqtt_client_handle_t mqtt_client;

// (As funções auxiliares não mudam)
void url_encode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0;
    while (*src && i + 4 < dst_len) {
        if (isalnum((unsigned char)*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            dst[i++] = *src;
        } else {
            i += snprintf(&dst[i], 4, "%%%02X", (unsigned char)*src);
        }
        src++;
    }
    dst[i] = '\0';
}
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, }, };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Esperando pela conexão Wi-Fi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}
void telegram_send_message(const char* message) {
    char encoded_msg[512];
    char url[1024];
    url_encode(message, encoded_msg, sizeof(encoded_msg));
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s", TELEGRAM_TOKEN, TELEGRAM_CHAT_ID, encoded_msg);
    esp_http_client_config_t config = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Mensagem enviada para o Telegram com sucesso!");
    } else {
        ESP_LOGE(TAG, "Erro ao enviar mensagem para o Telegram: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Conectado ao broker MQTT com sucesso!");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED: Desconectado do broker MQTT.");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR: Erro na conexão MQTT.");
        break;
    default:
        ESP_LOGI(TAG, "Outro evento do MQTT: event_id:%ld", event_id);
        break;
    }
}

/**
 * @brief ✅ NOVA FUNÇÃO: Converte o valor bruto do sensor para uma porcentagem (0-100%).
 * Leva em conta a relação inversa (valor baixo = úmido = 100%).
 */
int map_to_percentage(int value) {
    // Garante que o valor não saia dos limites de calibração
    if (value < SENSOR_MIN_MOLHADO) value = SENSOR_MIN_MOLHADO;
    if (value > SENSOR_MAX_SECO) value = SENSOR_MAX_SECO;

    // Mapeia a faixa do sensor (ex: 1099-4095) para a faixa de porcentagem (100-0)
    return 100 - ((value - SENSOR_MIN_MOLHADO) * 100) / (SENSOR_MAX_SECO - SENSOR_MIN_MOLHADO);
}

// Função principal do programa
void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    wifi_init_sta();

    // Configurações dos pinos
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_PIN, ADC_ATTEN_DB_11);
    
    // Inicia o cliente MQTT
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URL, };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "Aguardando 2 minutos para estabilização do sensor...");
    vTaskDelay(pdMS_TO_TICKS(30000)); // 2 minutos 120000

    bool ultimo_estado_seco;
    char buffer[256]; // Buffer genérico para mensagens

    // Faz a leitura inicial e publica/envia o status
    int valor_inicial_raw = adc1_get_raw(SENSOR_PIN);
    int umidade_percentual = map_to_percentage(valor_inicial_raw); // ✅ Converte para %
    ESP_LOGI(TAG, "Leitura inicial: %d | Porcentagem: %d%%", valor_inicial_raw, umidade_percentual);
    
    // Publica os dados iniciais no MQTT
    sprintf(buffer, "%d", valor_inicial_raw);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_LEITURA, buffer, 0, 1, 0);
    sprintf(buffer, "%d%%", umidade_percentual);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_PERCENT, buffer, 0, 1, 0);

    if (valor_inicial_raw >= THRESHOLD_SECO) {
        ultimo_estado_seco = true;
        gpio_set_level(LED_PIN, 1);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "SECO", 0, 1, 0);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "REGAR", 0, 1, 0);
        // ✅ MUDANÇA: Mensagem do Telegram mais amigável
        sprintf(buffer, "SoloScan Iniciado! 🚨\nSua planta já está seca, com apenas %d%% de umidade.", umidade_percentual);
    } else {
        ultimo_estado_seco = false;
        gpio_set_level(LED_PIN, 0);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "O solo esta umido, nao e necessario regar", 0, 1, 0);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "Nenhum alerta no momento", 0, 1, 0);
        // ✅ MUDANÇA: Mensagem do Telegram mais amigável
        sprintf(buffer, "SoloScan Iniciado! 🌱\nSua planta está com ótimos %d%% de umidade. Não precisa regar agora. ✅", umidade_percentual);
    }
    telegram_send_message(buffer);
    
    // Loop principal de monitoramento
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Pausa por 8 horas 28800000

        int valor_umidade_raw = adc1_get_raw(SENSOR_PIN);
        umidade_percentual = map_to_percentage(valor_umidade_raw); // ✅ Converte para %
        ESP_LOGI(TAG, "Leitura: %d | Porcentagem: %d%%", valor_umidade_raw, umidade_percentual);

        // Publica os dados brutos e percentuais no MQTT a cada ciclo
        sprintf(buffer, "%d", valor_umidade_raw);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_LEITURA, buffer, 0, 1, 0);
        sprintf(buffer, "%d%%", umidade_percentual);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_PERCENT, buffer, 0, 1, 0);

        if (valor_umidade_raw >= THRESHOLD_SECO) {
            gpio_set_level(LED_PIN, 1);
            if (!ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta secou! Enviando alertas...");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "SECO", 0, 1, 0);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "REGAR", 0, 1, 0);
                // ✅ MUDANÇA: Mensagem do Telegram mais amigável
                sprintf(buffer, "🚨 Alerta SoloScan: A umidade da sua planta caiu para %d%%. Hora de regar! 🌱", umidade_percentual);
                telegram_send_message(buffer);
                ultimo_estado_seco = true;
            }
        } else {
            gpio_set_level(LED_PIN, 0);
            if (ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta foi regada. Resetando alertas.");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "O solo esta umido, nao e necessario regar", 0, 1, 0);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "Nenhum alerta no momento", 0, 1, 0);
                // ✅ MUDANÇA: Mensagem do Telegram mais amigável
                sprintf(buffer, "✅ SoloScan: Obrigado por regar! A umidade voltou para %d%%. ✨💧", umidade_percentual);
                telegram_send_message(buffer);
                ultimo_estado_seco = false;
            }
        }
    }
}