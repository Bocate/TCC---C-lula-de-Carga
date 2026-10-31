#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <IskakINO_LiquidCrystal_I2C.h>
#include "HX711.h"
#include <math.h>

// ======================================================
// WIFI
// ======================================================

const char* ssid = "TCC";
const char* password = "lucas2024";

// ======================================================
// MQTT
// ======================================================

const char* mqtt_server = "broker.mqtt.cool";
const int mqtt_port = 1883;
const char* mqtt_topic = "balanca/peso";

// ======================================================
// OBJETOS
// ======================================================

WiFiClient espClient;
PubSubClient client(espClient);

LiquidCrystal_I2C lcd(16, 2);

HX711 scale;

// ======================================================
// CALIBRAÇÃO DA BALANÇA
// ======================================================

float fator_calibracao = 420.0;

// ======================================================
// PINOS
// ======================================================

// LCD I2C
const int SDA_PIN = 21;
const int SCL_PIN = 18;

// HX711
const int HX711_DT = 13;
const int HX711_SCK = 14;

// Botão de tara (um terminal no GPIO 4, o outro no GND)
const int BOTAO_TARA = 4;

// ======================================================
// VARIÁVEIS
// ======================================================

int Peso;

// ======================================================
// LEITURAS RECENTES
// ======================================================

const int TAMANHO_LEITURAS = 10;
float valores_recentes[TAMANHO_LEITURAS];
int quantidade_valores = 0;

// -1 = nenhum peso válido foi enviado ainda
int ultimoPesoEnvio = -1;

// ======================================================
// CONEXÃO WIFI
// ======================================================

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Conectando ao WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("ERRO: nao foi possivel conectar ao WiFi.");
  }
}

// ======================================================
// CALLBACK MQTT
// ======================================================

void callback(char* topic, byte* payload, unsigned int length) {
  // Não estamos recebendo comandos MQTT.
}

// ======================================================
// RECONEXÃO MQTT
// ======================================================

void reconnect() {
  while (!client.connected()) {
    Serial.print("MQTT conectando...");

    if (client.connect("ESP32_BALANCA")) {
      Serial.println(" conectado!");
    } else {
      Serial.print(" erro, rc=");
      Serial.print(client.state());
      Serial.println(" tentando novamente em 5 segundos...");
      delay(5000);
    }
  }
}

// ======================================================
// ADICIONAR LEITURA AO HISTÓRICO
// ======================================================

void adicionarValor(float valor) {
  if (quantidade_valores < TAMANHO_LEITURAS) {
    valores_recentes[quantidade_valores] = valor;
    quantidade_valores++;
  } else {
    for (int i = 0; i < TAMANHO_LEITURAS - 1; i++) {
      valores_recentes[i] = valores_recentes[i + 1];
    }
    valores_recentes[TAMANHO_LEITURAS - 1] = valor;
  }
}

// ======================================================
// DESVIO PADRÃO
// ======================================================

float stddev() {
  if (quantidade_valores <= 0) {
    return 0.0;
  }

  float mean = 0.0;
  for (int i = 0; i < quantidade_valores; i++) {
    mean += valores_recentes[i];
  }
  mean /= quantidade_valores;

  float variance = 0.0;
  for (int i = 0; i < quantidade_valores; i++) {
    float diff = valores_recentes[i] - mean;
    variance += diff * diff;
  }
  variance /= quantidade_valores;

  return sqrt(variance);
}

// ======================================================
// TARA (usada pelo Serial e pelo botão)
// ======================================================

void tararBalanca(const char* origem) {
  Serial.print("\n>>> EXECUTANDO TARA VIA ");
  Serial.print(origem);
  Serial.println("... <<<");

  lcd.setCursor(0, 1);
  lcd.print("Tarando...      ");

  scale.tare(); // Zera novamente com o que estiver em cima

  quantidade_valores = 0;
  ultimoPesoEnvio = -1;

  delay(1000);
  Serial.println(">>> TARA CONCLUIDA! <<<");
}

// ======================================================
// VERIFICA SERIAL E BOTÃO
// Retorna true se uma tara foi executada
// ======================================================

bool verificarTara() {
  static bool estadoAnterior = HIGH;
  static unsigned long ultimoAcionamento = 0;

  // ---- Serial ----
  if (Serial.available() > 0) {
    char comando = Serial.read();
    if (comando == 't' || comando == 'T') {
      tararBalanca("SERIAL");
      return true;
    }
  }

  // ---- Botão (INPUT_PULLUP: apertado = LOW) ----
  bool estado = digitalRead(BOTAO_TARA);

  if (estado == LOW && estadoAnterior == HIGH &&
      millis() - ultimoAcionamento > 50) {   // debounce de 50 ms
    estadoAnterior = estado;
    tararBalanca("BOTAO");
    ultimoAcionamento = millis();
    return true;
  }

  estadoAnterior = estado;
  return false;
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println("        BALANCA ESP32-S3");
  Serial.println("================================");
  Serial.println("DICA: aperte o botao ou digite 't' + Enter no Serial para TARAR.");

  // Botão de tara (resistor interno de pull-up)
  pinMode(BOTAO_TARA, INPUT_PULLUP);

  // I2C
  Serial.println("Inicializando I2C...");
  Wire.begin(SDA_PIN, SCL_PIN);

  // LCD
  Serial.println("Inicializando LCD...");
  lcd.begin();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("PESO:");

  lcd.setCursor(0, 1);
  lcd.print("Inicializando");

  // HX711 (Configuração Correta de Calibração)
  Serial.println("Inicializando HX711...");
  scale.begin(HX711_DT, HX711_SCK);

  scale.set_scale();                 // Reseta a escala para leitura bruta
  scale.tare();                      // Zera a balança vazia (Offset inicial)
  scale.set_scale(fator_calibracao); // Aplica o fator de calibração

  Serial.println("Balanca estabilizada e tarada!");

  // WIFI
  setup_wifi();

  // MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println();
  Serial.println("Sistema iniciado!");
  Serial.println();
}

// ======================================================
// LOOP
// ======================================================

void loop() {

  // Verifica comando de tara (Serial ou botão)
  verificarTara();

  // Verifica WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Tentando reconectar...");
    setup_wifi();
  }

  // MQTT
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  // ====================================================
  // LEITURA DA BALANÇA (Nativa e Calibrada em Gramas)
  // ====================================================
  float PesoReal = scale.get_units(5); // Pega a média de 5 leituras calibradas

  // Arredonda para o grama mais próximo
  Peso = round(PesoReal);

  // 5 g ou menos (incluindo negativos) = Zero
  if (Peso <= 5) {
    Peso = 0;
  }

  // Adiciona ao histórico para o desvio padrão
  adicionarValor(PesoReal);

  // LCD
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(Peso);
  lcd.print(" g");

  // Serial
  Serial.print("Peso: ");
  Serial.print(Peso);
  Serial.print(" g");
  Serial.print(" | Desvio padrao: ");
  Serial.print(stddev(), 2);
  Serial.println(" g");

  // ====================================================
  // LÓGICA DE ENVIO MQTT
  // ====================================================
  if (Peso == 0) {
    if (ultimoPesoEnvio == -1) {
      Serial.println("Balanca vazia - nao envia 0");
    }
    else if (ultimoPesoEnvio != 0) {
      char msg[10];
      itoa(0, msg, 10);
      client.publish(mqtt_topic, msg);

      Serial.println(">> OBJETO RETIRADO");
      Serial.println(">> ENVIADO AO MQTT: 0 g");

      ultimoPesoEnvio = 0;
    }
    else {
      Serial.println("Balanca vazia - 0 ja foi enviado");
    }
  }
  else {
    // Só envia se mudar mais do que 5g do último envio
    if (ultimoPesoEnvio == -1 || abs(Peso - ultimoPesoEnvio) > 5) {
      char msg[10];
      itoa(Peso, msg, 10);
      client.publish(mqtt_topic, msg);

      Serial.print(">> ENVIADO AO MQTT: ");
      Serial.print(Peso);
      Serial.println(" g");

      ultimoPesoEnvio = Peso;
    }
    else {
      Serial.print("Peso igual ou variacao menor que 5g: ");
      Serial.print(Peso);
      Serial.println(" g");
    }
  }

  // ====================================================
  // ESPERA 2 s, MAS CONFERINDO O BOTÃO/SERIAL
  // (substitui o delay(2000) para o botão não ser perdido)
  // ====================================================
  unsigned long inicio = millis();
  while (millis() - inicio < 2000) {
    if (verificarTara()) {
      break; // tarou: volta pro início do loop e lê o novo peso
    }
    delay(10);
  }
}