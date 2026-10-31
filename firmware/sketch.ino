// Central de Seguranca IoT - ESP32 + PIR + Telegram + Firebase (Wokwi)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>

#define BOT_TOKEN "SEU_TOKEN_AQUI"
#define CHAT_ID   "SEU_CHAT_ID_AQUI"
// so o endereco, sem https:// e sem barra no final
#define FIREBASE_HOST "seu-projeto-default-rtdb.firebaseio.com"

#define PIN_PIR     13
#define PIN_LED_VD  26
#define PIN_LED_VM  27
#define PIN_BUZZER  25
#define PIN_BOTAO   14

const char* LOCAL = "Sala de Equipamentos";
const unsigned long COOLDOWN_MS = 10000;  // intervalo minimo entre notificacoes
const unsigned long ALARME_MS   = 8000;   // duracao do alarme apos o movimento
const unsigned long DEBOUNCE_MS = 50;
const unsigned long ESTADO_MS   = 2000;   // envio periodico do estado (alem dos envios imediatos)
const unsigned long COMANDO_MS  = 1000;   // consulta de comandos da dashboard
const int FREQ_ALARME = 2000;             // Hz do beep

WiFiClientSecure client;                  // Telegram
WiFiClientSecure fbClient;                // Firebase (conexao mantida aberta)
HTTPClient fbHttp;                        // reaproveitado: evita novo handshake a cada envio
UniversalTelegramBot bot(BOT_TOKEN, client);
QueueHandle_t filaTelegram;               // mensagens aguardando envio
QueueHandle_t filaFirebase;               // eventos aguardando envio

struct Evento {
  char tipo[16];
  int ocorrencias;
  unsigned long uptime;
};

bool armado = false;
bool alarme = false;
bool telegramOk = false;
bool buzzerLigado = false;
volatile bool pirAtual = false;
volatile bool forcaEstado = true;         // pede envio imediato do estado
volatile int comandoPendente = 0;         // 1 = armar, 2 = desarmar (vem da dashboard)
unsigned long inicioAlarme = 0;
unsigned long ultimoEnvio = 0;
bool jaEnviou = false;
int ocorrencias = 0;

bool botaoAnterior = HIGH;
unsigned long ultimoDebounce = 0;
bool pirAnterior = false;

String horario() {
  unsigned long s = millis() / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(buf);
}

// ---------------------------------------------------------------- Telegram
void tarefaTelegram(void* param) {
  for (;;) {
    String* msg = nullptr;
    if (xQueueReceive(filaTelegram, &msg, portMAX_DELAY) == pdTRUE && msg) {
      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
      unsigned long t0 = millis();
      if (bot.sendMessage(CHAT_ID, *msg, "")) {
        Serial.printf("Telegram: MENSAGEM ENVIADA (%lu ms)\n", millis() - t0);
      } else {
        Serial.println("Telegram: FALHA NO ENVIO (confira BOT_TOKEN, CHAT_ID e se enviou /start ao bot)");
      }
      delete msg;
    }
  }
}

void enviar(const String& texto) {
  Serial.println("Enviando notificacao...");
  String* msg = new String(texto);
  if (xQueueSend(filaTelegram, &msg, 0) != pdTRUE) {
    Serial.println("Fila cheia, mensagem descartada");
    delete msg;
  }
}

// ---------------------------------------------------------------- Firebase
// metodo: "GET", "PUT" ou "POST". Devolve o codigo HTTP (200 = ok).
// A conexao segura fica aberta entre as chamadas (keep-alive): so a primeira paga o handshake.
int fbRequest(const char* metodo, const String& caminho, const String& corpo, String* resposta = nullptr) {
  String url = String("https://") + FIREBASE_HOST + caminho + ".json";
  fbHttp.setReuse(true);
  fbHttp.setTimeout(5000);
  fbHttp.setConnectTimeout(5000);
  if (!fbHttp.begin(fbClient, url)) {
    Serial.println("Firebase: begin() falhou, URL: " + url);
    return -1;
  }
  fbHttp.addHeader("Content-Type", "application/json");
  int code = fbHttp.sendRequest(metodo, corpo);
  String body;
  if (code > 0) body = fbHttp.getString();   // le a resposta inteira para poder reaproveitar a conexao
  if (resposta) *resposta = body;
  if (code < 0) {
    Serial.printf("Firebase: %s (heap livre: %u)\n", fbHttp.errorToString(code).c_str(), ESP.getFreeHeap());
    fbClient.stop();                          // descarta a conexao quebrada
  }
  fbHttp.end();
  return code;
}

String estadoJson() {
  String j = "{\"armado\":";
  j += armado ? "true" : "false";
  j += ",\"alarme\":";
  j += alarme ? "true" : "false";
  j += ",\"pir\":";
  j += pirAtual ? "true" : "false";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"ocorrencias\":" + String(ocorrencias);
  j += ",\"atualizadoEm\":{\".sv\":\"timestamp\"}}";   // horario do servidor
  return j;
}

void tarefaFirebase(void* param) {
  unsigned long ultEstado = 0, ultComando = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long agora = millis();

      // 1) estado primeiro: a dashboard ve a mudanca o mais cedo possivel
      if (forcaEstado || agora - ultEstado >= ESTADO_MS) {
        forcaEstado = false;
        ultEstado = agora;
        int c = fbRequest("PUT", "/central/estado", estadoJson());
        if (c != 200) Serial.printf("Firebase: falha ao enviar estado (HTTP %d)\n", c);
      }

      // 2) eventos do historico (intrusao, armado, desarmado)
      Evento ev;
      while (xQueueReceive(filaFirebase, &ev, 0) == pdTRUE) {
        String corpo = String("{\"tipo\":\"") + ev.tipo + "\",\"ocorrencias\":" + ev.ocorrencias +
                       ",\"uptime\":" + ev.uptime + ",\"ts\":{\".sv\":\"timestamp\"}}";
        int c = fbRequest("POST", "/central/eventos", corpo);
        Serial.printf("Firebase: evento %s (HTTP %d)\n", ev.tipo, c);
      }

      // 3) comando vindo da dashboard
      if (agora - ultComando >= COMANDO_MS) {
        ultComando = agora;
        String resp;
        int c = fbRequest("GET", "/central/comando", "", &resp);
        if (c == 200 && resp.indexOf("armar") >= 0) {
          // "desarmar" contem "armar": testa primeiro
          comandoPendente = (resp.indexOf("desarmar") >= 0) ? 2 : 1;
          fbRequest("PUT", "/central/comando", "\"\"");   // limpa o comando
          Serial.printf("Firebase: comando recebido: %s\n", resp.c_str());
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void evento(const char* tipo) {
  Evento ev;
  strncpy(ev.tipo, tipo, sizeof(ev.tipo) - 1);
  ev.tipo[sizeof(ev.tipo) - 1] = 0;
  ev.ocorrencias = ocorrencias;
  ev.uptime = millis() / 1000;
  if (xQueueSend(filaFirebase, &ev, 0) != pdTRUE) Serial.println("Fila do Firebase cheia");
}

// ---------------------------------------------------------------- Hardware
void buzzer(bool ligar) {
  if (ligar == buzzerLigado) return;
  buzzerLigado = ligar;
  if (ligar) tone(PIN_BUZZER, FREQ_ALARME);
  else noTone(PIN_BUZZER);
}

void beep(int freq, int ms) {
  tone(PIN_BUZZER, freq);
  delay(ms);
  noTone(PIN_BUZZER);
}

void atualizaLeds() {
  digitalWrite(PIN_LED_VD, armado ? LOW : HIGH);
  digitalWrite(PIN_LED_VM, armado ? HIGH : LOW);
}

void imprimeStatus(bool pir) {
  Serial.println("================================");
  Serial.println(" CENTRAL DE SEGURANCA IoT");
  Serial.println("================================");
  Serial.printf("Wi-Fi: %s\n", WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");
  Serial.printf("Telegram: %s\n", telegramOk ? "CONECTADO" : "INDISPONIVEL");
  Serial.println("Firebase: ENVIANDO ESTADO");
  Serial.printf("Sistema: %s\n", armado ? "ARMADO" : "DESARMADO");
  Serial.printf("PIR: %s\n", pir ? "MOVIMENTO" : "SEM MOVIMENTO");
  Serial.printf("LED: %s\n", armado ? "VERMELHO" : "VERDE");
  Serial.printf("Buzzer: %s\n\n", alarme ? "LIGADO" : "DESLIGADO");
}

// usado pelo botao fisico e pela dashboard
void setArmado(bool v) {
  if (v == armado) return;
  armado = v;
  alarme = false;
  buzzer(false);
  atualizaLeds();
  forcaEstado = true;                // dashboard atualiza antes do beep e do Telegram
  evento(armado ? "ARMADO" : "DESARMADO");
  beep(armado ? 2500 : 1200, 150);   // agudo ao armar, grave ao desarmar
  imprimeStatus(digitalRead(PIN_PIR));
  if (armado) {
    jaEnviou = false;
    enviar("SISTEMA ARMADO\nA central de seguranca foi ativada.\nMonitoramento iniciado.");
  } else {
    enviar("SISTEMA DESARMADO\nA central de seguranca foi desativada.");
  }
}

void disparaAlarme() {
  ocorrencias++;
  alarme = true;
  inicioAlarme = millis();
  digitalWrite(PIN_LED_VM, HIGH);
  buzzer(true);
  forcaEstado = true;                // manda o alarme ao banco na hora
  evento("INTRUSAO");                // toda ocorrencia vai para o historico

  Serial.println("--------------------------------");
  Serial.println("MOVIMENTO DETECTADO!");
  Serial.println("--------------------------------");
  Serial.printf("Sistema: ARMADO\nLED vermelho: LIGADO\nBuzzer: LIGADO\nOcorrencia n. %d (%s)\n\n",
                ocorrencias, horario().c_str());

  // controle de notificacoes: o Telegram so recebe se passou o cooldown
  if (!jaEnviou || millis() - ultimoEnvio >= COOLDOWN_MS) {
    String msg = "ALERTA DE INTRUSAO\n\n";
    msg += "Sistema: CENTRAL DE SEGURANCA\n";
    msg += String("Local: ") + LOCAL + "\n\n";
    msg += "Movimento detectado pelo sensor PIR.\n\n";
    msg += "Estado: ARMADO\nAcao: ALARME ATIVADO\n";
    msg += "Horario: " + horario() + "\n";
    msg += "Ocorrencias: " + String(ocorrencias);
    enviar(msg);
    ultimoEnvio = millis();
    jaEnviou = true;
  } else {
    Serial.printf("Notificacao suprimida (cooldown, faltam %lus)\n\n",
                  (COOLDOWN_MS - (millis() - ultimoEnvio)) / 1000);
  }
}

// padrao sonoro: BEEP-BEEP-BEEP (200ms ligado / 200ms desligado)
void tocaBuzzer() {
  buzzer((millis() / 200) % 2 == 0);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED_VD, OUTPUT);
  pinMode(PIN_LED_VM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  atualizaLeds();

  Serial.print("Conectando ao Wi-Fi");
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  client.setInsecure();     // sem verificar certificado: bem mais rapido
  fbClient.setInsecure();

  filaTelegram = xQueueCreate(5, sizeof(String*));
  filaFirebase = xQueueCreate(10, sizeof(Evento));
  // Firebase sobe primeiro: o primeiro envio nao espera o teste do Telegram
  xTaskCreatePinnedToCore(tarefaFirebase, "firebase", 16384, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(tarefaTelegram, "telegram", 16384, nullptr, 1, nullptr, 0);

  telegramOk = bot.getMe();
  imprimeStatus(false);
  beep(1500, 100);  // beep de inicializacao (teste do buzzer)
}

void loop() {
  // botao fisico com debounce: alterna ARMADO/DESARMADO (Opcao A)
  bool leitura = digitalRead(PIN_BOTAO);
  if (leitura != botaoAnterior && millis() - ultimoDebounce > DEBOUNCE_MS) {
    ultimoDebounce = millis();
    botaoAnterior = leitura;
    if (leitura == LOW) setArmado(!armado);
  }

  // comando vindo da dashboard
  if (comandoPendente) {
    int c = comandoPendente;
    comandoPendente = 0;
    setArmado(c == 1);
  }

  bool pir = digitalRead(PIN_PIR);
  pirAtual = pir;

  if (armado) {
    // borda de subida: so conta novo movimento quando o sensor sai de LOW para HIGH
    if (pir && !pirAnterior) disparaAlarme();

    if (alarme) {
      tocaBuzzer();
      if (millis() - inicioAlarme >= ALARME_MS) {
        alarme = false;
        buzzer(false);
        forcaEstado = true;
        Serial.println("Alarme encerrado. Retomando monitoramento.\n");
      }
    }
  }
  pirAnterior = pir;
  delay(10);
}
