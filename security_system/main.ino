#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define PIR_PIN     13
#define LED_PIN     2
#define BOT_CHECK_INTERVAL 3000

const char* SSID     = "";
const char* PASS     = "";
const String TOKEN   = "";
const String CHAT_ID = "";

WebServer server(80);

bool armed       = false;
int  pirLast     = LOW;
unsigned long lastCheck = 0;
int  lastMsgId   = 0;

void tgSend(const String& msg) {
  HTTPClient http;
  http.begin("https://api.telegram.org/bot" + TOKEN + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  http.POST("{\"chat_id\":\"" + CHAT_ID + "\",\"text\":\"" + msg + "\"}");
  http.end();
}

void tgPoll() {
  HTTPClient http;
  http.begin("https://api.telegram.org/bot" + TOKEN +
             "/getUpdates?offset=" + String(lastMsgId + 1));
  if (http.GET() != 200) { http.end(); return; }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, http.getString());
  http.end();

  for (JsonObject r : doc["result"].as<JsonArray>()) {
    lastMsgId     = r["update_id"];
    String text   = r["message"]["text"] | "";
    String from   = String(r["message"]["from"]["id"] | 0);

    if (from != CHAT_ID) continue;

    if (text == "/arm") {
      armed = true;
      pirLast = LOW;
      tgSend("ARMED");
    } else if (text == "/disarm") {
      armed = false;
      digitalWrite(LED_PIN, LOW);
      tgSend("DISARMED");
    } else if (text == "/status") {
      tgSend(armed ? "armed" : "disarmed");
    }
  }
}

void handleRoot() {
  String body = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Security</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
    "display:flex;align-items:center;justify-content:center;height:100vh}"
    ".card{border:1px solid #222;padding:40px 50px;text-align:center;width:300px}"
    "h1{font-size:14px;letter-spacing:.2em;text-transform:uppercase;color:#555;margin-bottom:32px}"
    ".status{font-size:11px;letter-spacing:.15em;padding:6px 14px;border-radius:2px;display:inline-block;margin-bottom:28px}"
    ".armed{background:#1a0000;color:#ff3333;border:1px solid #ff3333}"
    ".disarmed{background:#001a00;color:#33ff33;border:1px solid #33ff33}"
    ".btn{display:block;width:100%;padding:10px;font-family:monospace;font-size:12px;"
    "letter-spacing:.1em;text-transform:uppercase;cursor:pointer;border:1px solid #333;"
    "background:#111;color:#999;text-decoration:none;margin-top:8px;transition:all .15s}"
    ".btn:hover{border-color:#666;color:#e0e0e0}"
    ".btn-arm:hover{border-color:#ff3333;color:#ff3333}"
    ".btn-disarm:hover{border-color:#33ff33;color:#33ff33}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>Security System</h1>"
    "<div class='status " + String(armed ? "armed" : "disarmed") + "'>"
    + String(armed ? "ARMED" : "DISARMED") + "</div>"
    "<a href='/arm' class='btn btn-arm'>arm</a>"
    "<a href='/disarm' class='btn btn-disarm'>disarm</a>"
    "</div></body></html>";
  server.send(200, "text/html", body);
}

void handleArm() {
  armed   = true;
  pirLast = LOW;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDisarm() {
  armed = false;
  digitalWrite(LED_PIN, LOW);
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println(WiFi.localIP());

  server.on("/",       handleRoot);
  server.on("/arm",    handleArm);
  server.on("/disarm", handleDisarm);
  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastCheck > BOT_CHECK_INTERVAL) {
    tgPoll();
    lastCheck = millis();
  }

  if (!armed) return;

  int pir = digitalRead(PIR_PIN);
  if (pir == HIGH && pirLast == LOW) {
    digitalWrite(LED_PIN, HIGH);
    tgSend("ALERT: motion detected");
  } else if (pir == LOW && pirLast == HIGH) {
    digitalWrite(LED_PIN, LOW);
  }
  pirLast = pir;
}
