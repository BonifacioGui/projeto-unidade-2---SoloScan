SoloScan: Sistema de monitoramento de umidade do solo com ESP-32, MQTT e Telegram
Autor: Guilherme Bonifácio Feitosa | gbf1@discente.ifpe.edu.br
Orientador: Prof. Dr. David A. Nascimento | david.nascimento@garanhuns.ifpe.edu.br

Descrição do Projeto
O SoloScan é um sistema de Internet das Coisas (IoT) projetado para o monitoramento contínuo da umidade do solo em vasos de plantas. Utilizando um microcontrolador ESP32 e um sensor capacitivo, o projeto coleta dados e os transmite para facilitar o cuidado com as plantas. O sistema oferece tanto uma interface técnica para análise de dados via MQTT quanto notificações diretas e opcionais ao usuário via Telegram. Uma funcionalidade avançada permite a reconfiguração remota do limiar de alerta através de comandos MQTT, adaptando o monitoramento para diferentes tipos de plantas.

Funcionalidades
Publicação de Dados via MQTT: Envia o status da umidade, a leitura analógica bruta, a umidade em porcentagem e alertas para tópicos MQTT distintos.

Configuração Remota via MQTT: Permite alterar o limiar de alerta de umidade remotamente, enviando mensagens para o tópico soloscan/planta/set_tipo.

Persistência de Dados: Salva o limiar de alerta na memória não-volátil (NVS) do ESP32, garantindo que a configuração persista entre reinicializações.

Notificações via Telegram (Opcional): Alerta o usuário no celular quando o estado da umidade muda.

Calibração Customizável: Os valores de referência do sensor (mínimo e máximo) podem ser ajustados no código para maior precisão.

Lógica Anti-Spam: Garante que alertas sejam enviados apenas na mudança de estado da umidade.

Feedback Visual: O LED integrado na placa ESP32 acende para indicar que a planta precisa de água.

Hardware e Software
Hardware: Placa ESP32, Sensor de Umidade Capacitivo, Protoboard e Jumpers.

Software e Serviços:

Framework: ESP-IDF (v5.4.2)

IDE: Visual Studio Code com a extensão ESP-IDF

Broker MQTT: broker.hivemq.com (público para testes)

Notificações: Telegram (opcional)

Ferramenta de Teste: MQTT Explorer

Como Rodar o Projeto
1. Pré-requisitos

Ambiente de desenvolvimento ESP-IDF configurado.

Este repositório clonado localmente.

2. Configuração do Projeto (menuconfig)

Abra a pasta do projeto no VS Code e acesse o menuconfig (ícone de engrenagem ⚙️).

Garanta que as seguintes opções estão ativadas:

Component config ---> ESP-MQTT ---> [*] Enable MQTT Client

Component config ---> ESP-TLS ---> [*] ESP-TLS Official CA Certificate Bundle (necessário para o Telegram)

Importante: Para evitar crashes, ajuste o tamanho da stack:

Component config ---> ESP System Settings ---> Main task stack size (ajuste para 8192)

3. Personalização do Código

Abra o arquivo main/main.c e edite a seção de configurações no topo.

Wi-Fi: Preencha os campos obrigatórios WIFI_SSID e WIFI_PASS.

Calibração: Ajuste os valores de SENSOR_MIN_MOLHADO e SENSOR_MAX_SECO com base nos testes do seu sensor.

Telegram (Opcional):

Para ativar, preencha os campos TELEGRAM_TOKEN e TELEGRAM_CHAT_ID. (Use o @BotFather para criar um bot e obter o token, e o @userinfobot para obter seu Chat ID).

Para desativar, simplesmente deixe esses dois campos em branco ("").

4. Compilação e Gravação

Com o projeto configurado, compile e grave o firmware na placa ESP32 utilizando o fluxo padrão do ESP-IDF (idf.py flash monitor ou os controles do VS Code).

Uso da Configuração Remota
Para alterar o perfil da planta remotamente, utilize uma ferramenta como o MQTT Explorer para publicar uma mensagem no seguinte tópico:

Tópico: soloscan/planta/set_tipo

As mensagens (payloads) válidas são:

padrao: Define o limiar de alerta para 35% de umidade.

cacto: Define o limiar para 20% (ideal para suculentas).

samambaia: Define o limiar para 50% (ideal para plantas que gostam de solo mais úmido).

Após a publicação, o dispositivo irá salvar a nova configuração e confirmar a alteração com uma mensagem no Telegram (se ativado).