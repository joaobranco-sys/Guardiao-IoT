#include <Arduino.h>
#include <Wire.h>
#include <Keypad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include "mbedtls/aes.h"

constexpr uint8_t OLED_ADDR = 0x3C;
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int PIN_RFID = 4;
constexpr int PIN_PIR = 34;
constexpr int PIN_VIOLACAO = 35;
constexpr int PIN_SERVO = 18;
constexpr int PIN_BUZZER = 19;
constexpr int PIN_LED_VERDE = 16;
constexpr int PIN_LED_VERMELHO = 17;

constexpr unsigned long TEMPO_SESSAO = 15000;
constexpr unsigned long TEMPO_PORTA_ABERTA = 10000;
constexpr unsigned long TEMPO_SOM_ALARME = 2000;
constexpr unsigned long TEMPO_BLOQUEIO = 30000;
constexpr uint8_t MAX_TENTATIVAS = 3;
const String SENHA_CORRETA = "1234";

const byte LINHAS = 4;
const byte COLUNAS = 4;
char teclas[LINHAS][COLUNAS] = {
  {'1', '2', '3', 'A'}, {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}
};
byte pinosLinhas[LINHAS] = {13, 12, 14, 27};
byte pinosColunas[COLUNAS] = {26, 25, 33, 32};

Keypad teclado = Keypad(makeKeymap(teclas), pinosLinhas, pinosColunas, LINHAS, COLUNAS);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Servo fechadura;

enum class Estado { AGUARDANDO_RFID, AGUARDANDO_SENHA, ACESSO_LIBERADO, BLOQUEADO, ALARME };
Estado estado = Estado::AGUARDANDO_RFID;

String senhaDigitada;
uint8_t tentativas = 0;
unsigned long inicioEstado = 0;
bool ultimoRfid = HIGH;
bool ultimaPresenca = false;
bool violacaoRegistrada = false;
int nivelSeguroViolacao = HIGH;
unsigned long ultimoSegundoExibido = ULONG_MAX;

const uint8_t CHAVE_AES[16] = {
  0x47, 0x75, 0x61, 0x72, 0x64, 0x69, 0x61, 0x6F,
  0x49, 0x6F, 0x54, 0x32, 0x30, 0x32, 0x36, 0x21
};

void mostrar(const String &titulo, const String &linha1 = "", const String &linha2 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(titulo);
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(linha1);
  display.setCursor(0, 38);
  display.println(linha2);
  display.display();
}

String hexificar(const uint8_t *dados, size_t tamanho) {
  const char *hex = "0123456789ABCDEF";
  String saida;
  saida.reserve(tamanho * 2);
  for (size_t i = 0; i < tamanho; i++) {
    saida += hex[dados[i] >> 4];
    saida += hex[dados[i] & 0x0F];
  }
  return saida;
}

String cifrarEvento(const String &texto) {
  size_t tamanho = texto.length();
  size_t preenchido = ((tamanho / 16) + 1) * 16;
  uint8_t entrada[128] = {0};
  uint8_t saida[128] = {0};
  if (preenchido > sizeof(entrada)) return "EVENTO_MUITO_LONGO";
  memcpy(entrada, texto.c_str(), tamanho);
  uint8_t padding = preenchido - tamanho;
  for (size_t i = tamanho; i < preenchido; i++) entrada[i] = padding;

  // IV fixo somente para a demonstração. Em produção, use IV aleatório e envie-o com o ciphertext.
  uint8_t iv[16] = {0x43, 0x45, 0x53, 0x41, 0x52, 0x2D, 0x49,
                    0x4F, 0x54, 0x2D, 0x49, 0x56, 0x2D, 0x30, 0x31};
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, CHAVE_AES, 128);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, preenchido, iv, entrada, saida);
  mbedtls_aes_free(&aes);
  return hexificar(saida, preenchido);
}

void registrarEvento(const String &tipo) {
  String json = "{\"evento\":\"" + tipo + "\",\"uptime_ms\":" + String(millis()) + "}";
  Serial.println("[MQTT SIMULADO] topico=guardiao/eventos");
  Serial.println("payload_aes128cbc=" + cifrarEvento(json));
}

void definirEstado(Estado novo) {
  estado = novo;
  inicioEstado = millis();
}

void bipAcesso() {
  tone(PIN_BUZZER, 1500);
  delay(120);
  noTone(PIN_BUZZER);
}

void doisBipesErro() {
  for (uint8_t i = 0; i < 2; i++) {
    tone(PIN_BUZZER, 650);
    delay(110);
    noTone(PIN_BUZZER);
    if (i == 0) delay(90);
  }
}

void sistemaSeguro() {
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERMELHO, LOW);
  noTone(PIN_BUZZER);
  fechadura.write(0);
  senhaDigitada = "";
  definirEstado(Estado::AGUARDANDO_RFID);
  mostrar("GUARDIAO IOT", "Aproxime o cartao", "RFID: botao azul");
}

void acionarAlarme(const String &motivo) {
  definirEstado(Estado::ALARME);
  fechadura.write(0);
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERMELHO, HIGH);
  tone(PIN_BUZZER, 1200);
  mostrar("*** ALARME ***", motivo, "Desarme: chave + *");
  registrarEvento("ALARME_" + motivo);
}

void liberarAcesso() {
  tentativas = 0;
  definirEstado(Estado::ACESSO_LIBERADO);
  fechadura.write(90);
  digitalWrite(PIN_LED_VERDE, HIGH);
  digitalWrite(PIN_LED_VERMELHO, LOW);
  noTone(PIN_BUZZER);
  bipAcesso();
  mostrar("ACESSO LIBERADO", "Porta destravada", "Fecha em 5 segundos");
  registrarEvento("ACESSO_AUTORIZADO");
}

void negarAcesso() {
  tentativas++;
  digitalWrite(PIN_LED_VERMELHO, HIGH);
  registrarEvento("SENHA_INCORRETA");
  if (tentativas >= MAX_TENTATIVAS) {
    definirEstado(Estado::BLOQUEADO);
    tone(PIN_BUZZER, 900);
    mostrar("SISTEMA BLOQUEADO", "3 tentativas falharam", "Aguarde 30 segundos");
    registrarEvento("BLOQUEIO_BRUTE_FORCE");
  } else {
    doisBipesErro();
    senhaDigitada = "";
    mostrar("ACESSO NEGADO", "Tentativa " + String(tentativas) + "/3", "Digite novamente");
    delay(700); // curto feedback visual; a lógica principal permanece não bloqueante
    digitalWrite(PIN_LED_VERMELHO, LOW);
    inicioEstado = millis();
    mostrar("DIGITE A SENHA", "Senha: ", "# confirma | * cancela");
  }
}

void lerSensoresPrioritarios() {
  // A posição encontrada ao iniciar é considerada segura. O alarme dispara
  // somente quando a chave virtual muda para a outra posição.
  bool violacao = digitalRead(PIN_VIOLACAO) != nivelSeguroViolacao;
  bool presenca = digitalRead(PIN_PIR) == HIGH;

  if (violacao && !violacaoRegistrada && estado != Estado::ACESSO_LIBERADO) {
    violacaoRegistrada = true;
    acionarAlarme("PORTA VIOLADA");
  }
  if (!violacao) violacaoRegistrada = false;

  if (presenca && !ultimaPresenca && estado != Estado::ACESSO_LIBERADO) {
    acionarAlarme("MOVIMENTO DETECTADO");
  }
  ultimaPresenca = presenca;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RFID, INPUT_PULLUP);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_VIOLACAO, INPUT);
  nivelSeguroViolacao = digitalRead(PIN_VIOLACAO);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  fechadura.setPeriodHertz(50);
  fechadura.attach(PIN_SERVO, 500, 2400);
  sistemaSeguro();
  Serial.println("Guardiao IoT iniciado. Senha de demonstracao: 1234#");
}

void loop() {
  lerSensoresPrioritarios();
  unsigned long agora = millis();
  bool rfid = digitalRead(PIN_RFID);
  // Considera o RFID ativo durante todo o tempo em que o botão estiver pressionado.
  // Isso evita perder cliques rápidos na simulação do Wokwi.
  bool rfidPressionado = (rfid == LOW);
  ultimoRfid = rfid;
  char tecla = teclado.getKey();

  switch (estado) {
    case Estado::AGUARDANDO_RFID:
      if (rfidPressionado) {
        registrarEvento("RFID_VALIDO");
        liberarAcesso();
      } else if (tecla == '*') {
        senhaDigitada = "";
      } else if (tecla == '#') {
        senhaDigitada == SENHA_CORRETA ? liberarAcesso() : negarAcesso();
      } else if (tecla && senhaDigitada.length() < 8) {
        senhaDigitada += tecla;
        String mascara;
        for (size_t i = 0; i < senhaDigitada.length(); i++) mascara += '*';
        mostrar("DIGITE A SENHA", "Senha: " + mascara, "# confirma | * cancela");
      }
      break;

    case Estado::AGUARDANDO_SENHA:
      if (agora - inicioEstado >= TEMPO_SESSAO) {
        registrarEvento("TIMEOUT_AUTENTICACAO");
        sistemaSeguro();
      } else if (tecla == '*') {
        sistemaSeguro();
      } else if (tecla == '#') {
        senhaDigitada == SENHA_CORRETA ? liberarAcesso() : negarAcesso();
      } else if (tecla && senhaDigitada.length() < 8) {
        senhaDigitada += tecla;
        String mascara;
        for (size_t i = 0; i < senhaDigitada.length(); i++) mascara += '*';
        mostrar("DIGITE A SENHA", "Senha: " + mascara, "# confirma | * cancela");
      }
      break;

    case Estado::ACESSO_LIBERADO:
      if (agora - inicioEstado >= TEMPO_PORTA_ABERTA) {
        registrarEvento("PORTA_TRAVADA");
        sistemaSeguro();
      }
      break;

    case Estado::BLOQUEADO: {
      unsigned long decorrido = agora - inicioEstado;
      if (decorrido >= TEMPO_SOM_ALARME) {
        noTone(PIN_BUZZER);
      }
      if (decorrido >= TEMPO_BLOQUEIO) {
        tentativas = 0;
        sistemaSeguro();
      } else if ((decorrido / 1000) != ultimoSegundoExibido) {
        ultimoSegundoExibido = decorrido / 1000;
        unsigned long restante = (TEMPO_BLOQUEIO - decorrido + 999) / 1000;
        mostrar("SISTEMA BLOQUEADO", "Anti-brute-force", "Restam " + String(restante) + " s");
      }
      break;
    }

    case Estado::ALARME:
      if (agora - inicioEstado >= TEMPO_SOM_ALARME) {
        noTone(PIN_BUZZER);
      }
      if (tecla == '*' && digitalRead(PIN_VIOLACAO) == LOW) {
        registrarEvento("ALARME_DESARMADO");
        tentativas = 0;
        sistemaSeguro();
      }
      break;
  }
}
