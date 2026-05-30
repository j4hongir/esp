#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h>

// пины
#define PIR_PIN     13
#define LED_PIN     2
#define BUZZER_PIN  15
#define BUTTON_PIN  4
#define DHT_PIN     5
#define DHT_TYPE    DHT22

// сеть и бот
const char*  ssid      = "Wokwi-GUEST";
const char*  password  = "";
const String BOT_TOKEN = "bottoken";
const String CHAT_ID   = "chatid";

// константы времени
const unsigned long COOLDOWN_MS  = 10000UL;
const unsigned long DEBOUNCE_MS  = 50UL;
const unsigned long BOT_DELAY_MS = 3000UL;

// объекты
DHT                  dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure     securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

// состояние системы
enum State { IDLE, ARMED, ALARM, COOLDOWN };
State currentState = IDLE;

// настройки
unsigned long alarmDuration = 10000UL;
bool          buzzerEnabled = true;

// расписание
bool   scheduleActive     = false;
String scheduleArmTime    = "";
String scheduleDisarmTime = "";
String lastProcessedMin   = "";

// таймеры
unsigned long alarmStart    = 0;
unsigned long cooldownStart = 0;
unsigned long lastBotCheck  = 0;

// кнопка
int           buttonState     = HIGH;
int           lastButtonState = HIGH;
unsigned long lastDebounce    = 0;

// pir
int lastPirState = LOW;

// прототипы
void handleButton();
void checkSchedule();
void handleTelegram(int n);
void initNTP();
String timeStr();
String shortTimeStr();

// время

void initNTP() {
  configTime(5 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // utc+5
  unsigned long t = millis();
  while (time(nullptr) < 100000UL && millis() - t < 10000UL) delay(500);
}

String timeStr() {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  char buf[10];
  strftime(buf, sizeof(buf), "%H:%M:%S", ti);
  return String(buf);
}

String shortTimeStr() {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", ti);
  return String(buf);
}

// инициализация

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN,    INPUT_PULLDOWN);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);
  dht.begin();

  Serial.println("подключение к wi-fi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("wi-fi подключен");

  securedClient.setInsecure();
  initNTP();

  bot.sendMessage(CHAT_ID, "перезагрузка. время: " + timeStr(), "");
  Serial.println("система готова");
}

// главный цикл

void loop() {
  handleButton();
  checkSchedule();

  if (millis() - lastBotCheck > BOT_DELAY_MS) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    while (n) {
      handleTelegram(n);
      n = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }

  switch (currentState) {

    case IDLE:
      break;

    case ARMED: {
      int pir = digitalRead(PIR_PIN);
      if (pir == HIGH && lastPirState == LOW) {
        currentState = ALARM;
        alarmStart   = millis();
        digitalWrite(LED_PIN, HIGH);
        bot.sendMessage(CHAT_ID, "тревога! движение обнаружено. " + timeStr(), "");
      }
      lastPirState = pir;
      break;
    }

    case ALARM: {
      bool tick = (millis() / 250) % 2 == 0;
      digitalWrite(LED_PIN, tick ? HIGH : LOW);
      if (buzzerEnabled) tone(BUZZER_PIN, tick ? 1000 : 1500);

      if (millis() - alarmStart > alarmDuration) {
        currentState  = COOLDOWN;
        cooldownStart = millis();
        digitalWrite(LED_PIN, LOW);
        noTone(BUZZER_PIN);
        bot.sendMessage(CHAT_ID, "сирена выключена (таймаут).", "");
      }
      break;
    }

    case COOLDOWN:
      if (millis() - cooldownStart > COOLDOWN_MS) {
        currentState = ARMED;
        lastPirState = digitalRead(PIR_PIN);
        bot.sendMessage(CHAT_ID, "система снова активна.", "");
      }
      break;
  }
}

// физическая кнопка

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) lastDebounce = millis();

  if (millis() - lastDebounce > DEBOUNCE_MS && reading != buttonState) {
    buttonState = reading;
    if (buttonState == LOW && currentState != IDLE) {
      currentState = IDLE;
      digitalWrite(LED_PIN, LOW);
      noTone(BUZZER_PIN);
      bot.sendMessage(CHAT_ID, "кнопка нажата. охрана сброшена.", "");
    }
  }

  lastButtonState = reading;
}

// расписание

void checkSchedule() {
  if (!scheduleActive) return;

  String cur = shortTimeStr();
  if (cur == lastProcessedMin) return;
  lastProcessedMin = cur;

  if (cur == scheduleArmTime && currentState == IDLE) {
    currentState = ARMED;
    lastPirState = digitalRead(PIR_PIN);
    bot.sendMessage(CHAT_ID, "расписание: охрана включена.", "");
  } else if (cur == scheduleDisarmTime && currentState != IDLE) {
    currentState = IDLE;
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    bot.sendMessage(CHAT_ID, "расписание: охрана выключена.", "");
  }
}

// обработчик telegram

void handleTelegram(int n) {
  for (int i = 0; i < n; i++) {
    String chat = bot.messages[i].chat_id;
    if (chat != CHAT_ID) continue;

    String text = bot.messages[i].text;
    Serial.println("tg: " + text);

    if (text == "/arm") {
      currentState = ARMED;
      lastPirState = digitalRead(PIR_PIN);
      bot.sendMessage(chat, "охрана включена. " + timeStr(), "");
    }
    else if (text == "/disarm") {
      currentState = IDLE;
      digitalWrite(LED_PIN, LOW);
      noTone(BUZZER_PIN);
      bot.sendMessage(chat, "охрана выключена. " + timeStr(), "");
    }
    else if (text.startsWith("/alarm ")) {
      String arg = text.substring(7);
      if (arg == "on") {
        buzzerEnabled = true;
        bot.sendMessage(chat, "сирена вкл.", "");
      } else if (arg == "off") {
        buzzerEnabled = false;
        noTone(BUZZER_PIN);
        bot.sendMessage(chat, "сирена выкл.", "");
      } else if (arg.startsWith("time ")) {
        int secs = arg.substring(5).toInt();
        if (secs > 0) {
          alarmDuration = (unsigned long)secs * 1000UL;
          bot.sendMessage(chat, "длительность сирены: " + String(secs) + " сек.", "");
        } else {
          bot.sendMessage(chat, "ошибка: укажи число секунд > 0.", "");
        }
      } else {
        bot.sendMessage(chat, "неверный аргумент. см. /help", "");
      }
    }
    else if (text.startsWith("/noise ")) {
      int vol = text.substring(7).toInt();
      if (vol >= 0 && vol <= 100) {
        bot.sendMessage(chat, "громкость: " + String(vol) + "%", "");
      } else {
        bot.sendMessage(chat, "ошибка: значение от 0 до 100.", "");
      }
    }
    else if (text.startsWith("/schedule ")) {
      String arg = text.substring(10);
      if (arg == "off") {
        scheduleActive = false;
        bot.sendMessage(chat, "расписание отключено.", "");
      } else {
        int sp = arg.indexOf(' ');
        if (sp > 0) {
          scheduleArmTime    = arg.substring(0, sp);
          scheduleDisarmTime = arg.substring(sp + 1);
          scheduleActive     = true;
          bot.sendMessage(chat, "расписание: вкл " + scheduleArmTime + ", выкл " + scheduleDisarmTime, "");
        } else {
          bot.sendMessage(chat, "формат: /schedule HH:MM HH:MM", "");
        }
      }
    }
    else if (text == "/status") {
      const char* states[] = { "выключена", "активна", "тревога", "откат" };
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      String env = (isnan(t) || isnan(h))
        ? "ошибка датчика"
        : String(t, 1) + "C, влажность " + String(h, 0) + "%";

      String msg = "статус: " + String(states[currentState]) + "\n"
                 + "время: "  + timeStr() + "\n"
                 + "темп: "   + env + "\n"
                 + "сирена: " + String(buzzerEnabled ? "вкл" : "выкл")
                 + ", " + String(alarmDuration / 1000) + " сек\n"
                 + "расписание: " + (scheduleActive
                     ? scheduleArmTime + " - " + scheduleDisarmTime
                     : String("выкл"));
      bot.sendMessage(chat, msg, "");
    }
    else if (text == "/help" || text == "/start") {
      String msg =
        "команды:\n"
        "/arm                     - включить охрану\n"
        "/disarm                  - выключить охрану\n"
        "/status                  - состояние системы\n"
        "/alarm on|off            - вкл/выкл сирену\n"
        "/alarm time {сек}        - длительность тревоги\n"
        "/noise {0-100}           - громкость сирены\n"
        "/schedule HH:MM HH:MM   - авто вкл/выкл\n"
        "/schedule off            - сбросить расписание";
      bot.sendMessage(chat, msg, "");
    }
    else {
      bot.sendMessage(chat, "неизвестная команда. /help - список команд.", "");
    }
  }
}
