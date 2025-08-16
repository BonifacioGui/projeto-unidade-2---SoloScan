#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "nvs_flash.h"
#include "nvs.h" // Biblioteca para salvar na memória não-volátil
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

// CONFIGURAÇÕES DE REDE
#define WIFI_SSID           "NOME_DA_SUA_REDE_WIFI"
#define WIFI_PASS           "SENHA_DA_SUA_REDE_WIFI"
// Configurações Telegram são opcionais. Deixe em branco se não for usar
#define TELEGRAM_TOKEN      ""
#define TELEGRAM_CHAT_ID    ""

// CONFIGURAÇÕES DO PROTOCOLO MQTT 
#define MQTT_BROKER_URL     "mqtt://broker.hivemq.com"
#define MQTT_BASE_TOPIC     "soloscan/planta"
#define MQTT_TOPIC_STATUS   MQTT_BASE_TOPIC "/status"
#define MQTT_TOPIC_LEITURA  MQTT_BASE_TOPIC "/leitura_raw"
#define MQTT_TOPIC_PERCENT  MQTT_BASE_TOPIC "/umidade_percentual"
#define MQTT_TOPIC_ALERTA   MQTT_BASE_TOPIC "/alerta"
#define MQTT_TOPIC_SET_TIPO MQTT_BASE_TOPIC "/set_tipo"

#define SENSOR_MIN_MOLHADO  1406
#define SENSOR_MAX_SECO     3817

#define SENSOR_PIN          ADC1_CHANNEL_6 // O sensor está no pino GPIO34
#define LED_PIN             GPIO_NUM_2     

static const char *TAG = "SOLOSCAN_PRO"; 
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_event_group; 
static esp_mqtt_client_handle_t mqtt_client; 

static int g_threshold_percent_seco = 35;

// Essa função garante que a configuração persista mesmo se o dispositivo for reiniciado.
void save_threshold_to_nvs(int threshold) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro (%s) abrindo NVS para escrita!", esp_err_to_name(err));
    } else {
        err = nvs_set_i32(my_handle, "threshold", threshold);
        if(err == ESP_OK) {
            ESP_LOGI(TAG, "Novo threshold (%d) salvo na memória NVS", threshold);
            nvs_commit(my_handle);
        } else {
            ESP_LOGE(TAG, "Falha ao salvar threshold na NVS!");
        }
        nvs_close(my_handle);
    }
}

// lê o limite de alerta da memória NVS quando o ESP32 liga
void load_threshold_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS não encontrada. Usando threshold padrão (%d).", g_threshold_percent_seco);
    } else {
        int32_t required_value;
        err = nvs_get_i32(my_handle, "threshold", &required_value);
        if (err == ESP_OK) {
            g_threshold_percent_seco = required_value;
            ESP_LOGI(TAG, "Threshold (%d) carregado da memória NVS.", g_threshold_percent_seco);
        } else {
            ESP_LOGW(TAG, "Nenhum threshold salvo na memória. Usando padrão (%d).", g_threshold_percent_seco);
        }
        nvs_close(my_handle);
    }
}

// Converte uma string para o formato URL seguro, codificando caracteres especiais.
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

// Gerencia eventos de Wi-Fi, como reconexão automática e status de conexão.
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

// Configura, inicia e aguarda a conexão Wi-Fi ser estabelecida.
void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
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

// Envia uma notificação para o Telegram via requisição HTTP.
void telegram_send_message(const char* message) {
    // Verifica se as credenciais do Telegram foram preenchidas, caso contrário, pula o envio.
    if (strcmp(TELEGRAM_TOKEN, "O_TOKEN_DO_SEU_BOT_DO_TELEGRAM") == 0 || strcmp(TELEGRAM_CHAT_ID, "O_ID_DO_SEU_CHAT_COM_O_BOT") == 0) {
        ESP_LOGW(TAG, "Token/Chat ID do Telegram não configurado. Pulando envio.");
        return;
    }
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

// Gerencia a conexão MQTT e processa mensagens de configuração recebidas.
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    char topic_buffer[100];
    char data_buffer[100];

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado ao broker MQTT!");
        // Assina o tópico de configuração para poder receber comandos remotos.
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_SET_TIPO, 0);
        ESP_LOGI(TAG, "Assinando o tópico de configuração: %s", MQTT_TOPIC_SET_TIPO);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Desconectado do broker MQTT.");
        break;
    // Caso: Uma mensagem foi recebida em um tópico que assinamos.
    case MQTT_EVENT_DATA:
        // Copia o tópico e a mensagem para buffers locais.
        strncpy(topic_buffer, event->topic, event->topic_len);
        topic_buffer[event->topic_len] = '\0';
        strncpy(data_buffer, event->data, event->data_len);
        data_buffer[event->data_len] = '\0';
        
        ESP_LOGI(TAG, "Mensagem recebida no tópico %s: %s", topic_buffer, data_buffer);

        // Verifica se a mensagem é para o tópico de configuração.
        if (strcmp(topic_buffer, MQTT_TOPIC_SET_TIPO) == 0) {
            int new_threshold;
            char response_msg[200];
            char plant_type[50];

            // Compara o conteúdo da mensagem para determinar o novo limite de alerta.
            if (strcmp(data_buffer, "padrao") == 0) {
                new_threshold = 35;
                strcpy(plant_type, "Planta Padrão");
            } else if (strcmp(data_buffer, "cacto") == 0) {
                new_threshold = 20;
                strcpy(plant_type, "Cacto/Suculenta");
            } else if (strcmp(data_buffer, "samambaia") == 0) {
                new_threshold = 50;
                strcpy(plant_type, "Samambaia (Amante de Água)");
            } else {
                // Se o comando for inválido, envia um aviso e não faz nada.
                ESP_LOGW(TAG, "Tipo de planta desconhecido: %s", data_buffer);
                sprintf(response_msg, "⚠️ SoloScan: Comando '%s' não reconhecido. Use 'padrao', 'cacto' ou 'samambaia'.", data_buffer);
                telegram_send_message(response_msg);
                return;
            }

            // Apenas atualiza se o novo valor for diferente do atual.
            if (new_threshold != g_threshold_percent_seco) {
                g_threshold_percent_seco = new_threshold;
                save_threshold_to_nvs(g_threshold_percent_seco); // Salva a nova configuração na memória.
                // Envia uma mensagem de confirmação para o Telegram.
                sprintf(response_msg, "✅ SoloScan reconfigurado!\nTipo: %s\nAlerta de rega abaixo de: %d%%", plant_type, g_threshold_percent_seco);
                telegram_send_message(response_msg);
            } else {
                ESP_LOGI(TAG, "O tipo de planta já era o mesmo. Nenhuma alteração feita.");
            }
        }
        break;
    // Caso: Ocorreu um erro na conexão MQTT.
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        break;
    }
}

// FUNÇÃO DE CONVERSÃO DO SENSOR
int map_to_percentage(int value) {
    // Garante que o valor lido esteja dentro dos limites de calibração.
    if (value < SENSOR_MIN_MOLHADO) value = SENSOR_MIN_MOLHADO;
    if (value > SENSOR_MAX_SECO) value = SENSOR_MAX_SECO;
    // Aplica uma "regra de três" invertida para calcular a porcentagem.
    return 100 - ((value - SENSOR_MIN_MOLHADO) * 100) / (SENSOR_MAX_SECO - SENSOR_MIN_MOLHADO);
}

// FUNÇÃO PRINCIPAL 
void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    
    // Inicializa a memória flash não-volátil (NVS).
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_threshold_from_nvs(); // Carrega a última configuração salva.
    wifi_init_sta(); // Inicia o Wi-Fi e espera conectar.

    // Configura os pinos do LED e do sensor.
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_PIN, ADC_ATTEN_DB_11);
    
    // Configura e inicia o cliente MQTT.
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URL, };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "Aguardando 3 minutos para estabilização do sensor...");
    vTaskDelay(pdMS_TO_TICKS(180000)); // Pausa de 3 minutos.

    bool ultimo_estado_seco; // Variável para rastrear o estado anterior.
    char buffer[256]; // Buffer para formatar as mensagens.

    // Faz a primeira leitura de umidade.
    int valor_inicial_raw = adc1_get_raw(SENSOR_PIN);
    int umidade_percentual = map_to_percentage(valor_inicial_raw);
    ESP_LOGI(TAG, "Leitura inicial: %d | Porcentagem: %d%% | Limite de Alerta: %d%%", valor_inicial_raw, umidade_percentual, g_threshold_percent_seco);
    
    // Publica os dados iniciais nos tópicos MQTT.
    sprintf(buffer, "%d", valor_inicial_raw);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_LEITURA, buffer, 0, 1, 0);
    sprintf(buffer, "%d%%", umidade_percentual);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_PERCENT, buffer, 0, 1, 0);

    // Determina o estado inicial e publica o status/alerta correspondente.
    if (umidade_percentual < g_threshold_percent_seco) {
        ultimo_estado_seco = true;
        gpio_set_level(LED_PIN, 1); // Acende o LED.
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "SECO", 0, 1, 0);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "REGAR", 0, 1, 0);
        sprintf(buffer, "SoloScan Iniciado! 🚨\nSua planta já está seca, com apenas %d%% de umidade.", umidade_percentual);
    } else {
        ultimo_estado_seco = false;
        gpio_set_level(LED_PIN, 0); // Apaga o LED.
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "UMIDO", 0, 1, 0);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "OK", 0, 1, 0);
        sprintf(buffer, "SoloScan Iniciado! 🌱\nSua planta está com ótimos %d%% de umidade. Não precisa regar agora. ✅", umidade_percentual);
    }
    telegram_send_message(buffer); // Envia a mensagem de status inicial para o Telegram.
    
    // Inicia o ciclo de monitoramento.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Pausa o programa por 30 segundos. (só para testes)

        // Faz uma nova leitura de umidade.
        int valor_umidade_raw = adc1_get_raw(SENSOR_PIN);
        umidade_percentual = map_to_percentage(valor_umidade_raw);
        ESP_LOGI(TAG, "Leitura: %d | Porcentagem: %d%% | Limite de Alerta: %d%%", valor_umidade_raw, umidade_percentual, g_threshold_percent_seco);

        // Publica os dados de leitura no MQTT a cada ciclo.
        sprintf(buffer, "%d", valor_umidade_raw);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_LEITURA, buffer, 0, 1, 0);
        sprintf(buffer, "%d%%", umidade_percentual);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_PERCENT, buffer, 0, 1, 0);

        // Só notifica na MUDANÇA de estado.
        if (umidade_percentual < g_threshold_percent_seco) {
            gpio_set_level(LED_PIN, 1);
            // Se o estado anterior NÃO era seco, significa que a planta acabou de secar.
            if (!ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta secou! Enviando alertas...");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "SECO", 0, 1, 0);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "REGAR", 0, 1, 0);
                sprintf(buffer, "🚨 Alerta SoloScan: A umidade da sua planta caiu para %d%%. Hora de regar! 🌱", umidade_percentual);
                telegram_send_message(buffer);
                ultimo_estado_seco = true; // Atualiza o estado para "seco".
            }
        } else {
            gpio_set_level(LED_PIN, 0);
            // Se o estado anterior ERA seco, significa que a planta acabou de ser regada.
            if (ultimo_estado_seco) {
                ESP_LOGI(TAG, "Planta foi regada. Resetando alertas.");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "UMIDO", 0, 1, 0);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_ALERTA, "OK", 0, 1, 0);
                sprintf(buffer, "✅ SoloScan: Obrigado por regar! A umidade voltou para %d%%. ✨💧", umidade_percentual);
                telegram_send_message(buffer);
                ultimo_estado_seco = false; // Atualiza o estado para "úmido".
            }
        }
    }
}
