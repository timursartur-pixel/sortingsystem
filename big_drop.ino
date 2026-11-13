/*
 * IWSR (Intelligent Waste Sorter for Recycling) - Main Firmware
 * Platform: ESP32 (Deneyap Kart 1A v2)
 * Description: Captures image, classifies via Gemini API, controls sorting servos.
 * Features: Telegram bot, asynchronous queues, persistent TLS connections.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <ESP32Servo.h>
#include <math.h>

// ====================================================
// Network & API Credentials
// ====================================================
const char* WIFI_SSID = "Tim";
const char* WIFI_PASS = "95909992";

const char* TG_TOKEN = "";
const char* TG_HOST  = "api.telegram.org";
char g_defaultChat[24] = "";
const char* TG_OWNER_CHAT = "6896444483";

const char* GEM_HOST = "generativelanguage.googleapis.com";
const char* GEMINI_KEY = ""; 

// Model selection (fastest multimodal for our use case)
#define GEM_MODEL_FAST     "gemini-3.5-flash-lite"
#define GEM_MODEL_FALLBACK "gemini-3.1-flash-lite"

#define GEM_TIMEOUT_MS        6000
#define GEM_CONNECT_TIMEOUT   5000
#define GEM_HANDSHAKE_SEC     8
#define GEM_KEEPALIVE_MS      25000

// Prompt for the AI model
static const char PROMPT_JSON[] =
  "Identify the real material of the main object in this image. "
  "Judge by shape and purpose, not by the label. "
  "Transparent bottle = plastic. Newspaper or cardboard = paper. "
  "Food scrap or peel = organics. Can or metal lid = metal. "
  "Stick or plank = wood. Jar or drinking glass = glass. "
  "Reply with exactly one word: plastic, paper, organics, metal, wood, glass. "
  "No punctuation, no explanation.";

// ====================================================
// Hardware Pins & Servo Configuration
// ====================================================
#define BUTTON_PIN  D8

#define SERVO_COUNT 3
#define SV_D9    0
#define SV_D13   1
#define SV_D14   2

#define D9_HOME             90
#define D9_OPEN             170
#define D13_HOME            90
#define D13_OPEN            10
#define D14_HOME            0
#define D14_OPEN            180
#define D14_MID             60
#define D14_CLOSE           0

const int servoPins[SERVO_COUNT]  = { D9, D13, D14 };
const int servoMin[SERVO_COUNT]   = { 0, 0, 0 };
const int servoMax[SERVO_COUNT]   = { 180, 180, 180 };
const int servoHome[SERVO_COUNT]  = { D9_HOME, D13_HOME, D14_HOME };
const int servoUsMin[SERVO_COUNT] = { 1000, 1000, 1000 };
const int servoUsMax[SERVO_COUNT] = { 2000, 2000, 2000 };
const char* servoName[SERVO_COUNT] = { "D9", "D13", "D14_flap" };

const bool servoHoldIdle[SERVO_COUNT] = { false, false, false };
#define SERVO_RELEASE_IDLE  1
#define STEP_DEG            2
#define STEP_DELAY_MS       20

Servo servos[SERVO_COUNT];
int currentAngle[SERVO_COUNT];
bool servoAttached[SERVO_COUNT] = { false, false, false };

// Camera Pins
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     CAMXC
#define SIOD_GPIO_NUM     CAMSD
#define SIOC_GPIO_NUM     CAMSC
#define Y9_GPIO_NUM       CAMD9
#define Y8_GPIO_NUM       CAMD8
#define Y7_GPIO_NUM       CAMD7
#define Y6_GPIO_NUM       CAMD6
#define Y5_GPIO_NUM       CAMD5
#define Y4_GPIO_NUM       CAMD4
#define Y3_GPIO_NUM       CAMD3
#define Y2_GPIO_NUM       CAMD2
#define VSYNC_GPIO_NUM    CAMV
#define HREF_GPIO_NUM     CAMH
#define PCLK_GPIO_NUM     CAMPC

#define CAM_FRAMESIZE   FRAMESIZE_QVGA
#define CAM_QUALITY     12

// ====================================================
// Data Structures & Global Variables
// ====================================================
const char* CATEGORIES[] = { "plastic", "paper", "organics", "metal", "wood", "glass" };
const int CATEGORY_COUNT = 6;

struct RuMap { const char* ru; const char* en; };
const RuMap RUSSIAN_MAP[] = {
  {"пластик", "plastic"}, {"пластмасса", "plastic"},
  {"бумага", "paper"},    {"картон", "paper"},
  {"органика", "organics"}, {"органические", "organics"},
  {"металл", "metal"},    {"железо", "metal"},
  {"дерево", "wood"},     {"древесина", "wood"},
  {"стекло", "glass"},    {"стеклянн", "glass"}
};
const int RUSSIAN_MAP_COUNT = sizeof(RUSSIAN_MAP) / sizeof(RUSSIAN_MAP[0]);

struct Shot       { uint8_t* data; size_t len; };
struct TriggerEvt { char chatId[24]; bool withAI; };
struct TgOut      { char chatId[24]; char text[320]; uint8_t* photo; size_t photoLen; };

QueueHandle_t qTrigger;
QueueHandle_t qOut;
SemaphoreHandle_t hsMux;     
SemaphoreHandle_t chatMux;

volatile bool busy          = false;
volatile bool aiPending     = false; 
volatile bool buttonPressed = false;

long tgLastUpdateId = 0;
unsigned long lastTriggerMs = 0;
const unsigned long TRIGGER_COOLDOWN_MS = 3000;

bool cameraInitialized = false;
unsigned long lastCamUseMs = 0;

struct Timing { uint32_t cam, b64, net, parse, total; };
Timing lastTiming = {0, 0, 0, 0, 0};

// ====================================================
// Telegram Chat Handling
// ====================================================
void setDefaultChat(const char* id) {
  xSemaphoreTake(chatMux, portMAX_DELAY);
  strlcpy(g_defaultChat, id, sizeof(g_defaultChat));
  xSemaphoreGive(chatMux);
}

String getDefaultChat() {
  xSemaphoreTake(chatMux, portMAX_DELAY);
  String s(g_defaultChat);
  xSemaphoreGive(chatMux);
  return s;
}

void tgQueueText(const String& chat, const String& text);
void tgQueuePhoto(const String& chat, const String& caption, Shot& shot);

const char* httpErrName(int code) {
  switch (code) {
    case  -1: return "CONN_REFUSED"; case  -2: return "SEND_HEADER_FAILED";
    case  -3: return "SEND_PAYLOAD_FAILED"; case  -4: return "NOT_CONNECTED";
    case  -5: return "CONN_LOST"; case  -6: return "NO_STREAM";
    case  -7: return "NO_HTTP_SERVER"; case  -8: return "TOO_LESS_RAM";
    case  -9: return "ENCODING"; case -10: return "STREAM_WRITE";
    case -11: return "READ_TIMEOUT"; default: return (code > 0) ? "HTTP status" : "unknown";
  }
}

// ====================================================
// Servo Mechanics
// ====================================================
void servoEnsureAttached(int i) {
  if (servoAttached[i]) return;
  servos[i].setPeriodHertz(50);
  int ch = servos[i].attach(servoPins[i], servoUsMin[i], servoUsMax[i]);
  if (ch <= 0) {
    Serial.printf("[SERVO] %s: attach failed (%d)\n", servoName[i], ch);
    return;
  }
  servoAttached[i] = true;
  servos[i].write(currentAngle[i]);
  delay(60);
}

void servoRelease(int i) {
#if SERVO_RELEASE_IDLE
  if (!servoAttached[i] || servoHoldIdle[i]) return;
  servos[i].detach();
  servoAttached[i] = false;
#endif
}

void servoReleaseAll() {
#if SERVO_RELEASE_IDLE
  for (int i = 0; i < SERVO_COUNT; i++) servoRelease(i);
  Serial.println("[SERVO] Released");
#endif
}

static bool stepToward(int i, int target, int step) {
  int cur = currentAngle[i];
  if (cur == target) return true;
  int dir = (target > cur) ? 1 : -1;
  if (abs(target - cur) <= step) cur = target;
  else cur += dir * step;
  servos[i].write(cur);
  currentAngle[i] = cur;
  return (cur == target);
}

void moveServoSmooth(int i, int target, int step = STEP_DEG, int delayMs = STEP_DELAY_MS) {
  if (i < 0 || i >= SERVO_COUNT) return;
  target = constrain(target, servoMin[i], servoMax[i]);
  if (step < 1) step = 1;
  servoEnsureAttached(i);
  while (currentAngle[i] != target) {
    stepToward(i, target, step);
    delay(delayMs);
  }
}

void moveTwoServosSmooth(int iA, int targetA, int iB, int targetB, int step = STEP_DEG, int delayMs = STEP_DELAY_MS) {
  targetA = constrain(targetA, servoMin[iA], servoMax[iA]);
  targetB = constrain(targetB, servoMin[iB], servoMax[iB]);
  if (step < 1) step = 1;
  servoEnsureAttached(iA);
  servoEnsureAttached(iB);
  while (currentAngle[iA] != targetA || currentAngle[iB] != targetB) {
    stepToward(iA, targetA, step);
    stepToward(iB, targetB, step);
    delay(delayMs);
  }
}

void setAllServoHome() {
  moveTwoServosSmooth(SV_D9, servoHome[SV_D9], SV_D13, servoHome[SV_D13]);
  moveServoSmooth(SV_D14, servoHome[SV_D14]);
  delay(200);
  servoReleaseAll();
}

void printServoState() {
  Serial.println("[SERVO] Current angles:");
  for (int i = 0; i < SERVO_COUNT; i++) {
    Serial.printf("   [%d] %-10s = %3d  %s\n", i, servoName[i], currentAngle[i], servoAttached[i] ? "(powered)" : "(released)");
  }
}

void initServos() {
  Serial.println("[SERVO] Init...");
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  for (int i = 0; i < SERVO_COUNT; i++) {
    currentAngle[i] = constrain(servoHome[i], servoMin[i], servoMax[i]);
    servos[i].setPeriodHertz(50);
    int ch = servos[i].attach(servoPins[i], servoUsMin[i], servoUsMax[i]);
    if (ch <= 0) {
      Serial.printf("[SERVO] %s: attach FAILED (%d)\n", servoName[i], ch);
      servoAttached[i] = false;
    } else {
      servoAttached[i] = true;
      Serial.printf("[SERVO] OK %s: pin=%d, ch=%d\n", servoName[i], servoPins[i], ch);
    }
    servos[i].write(currentAngle[i]);
    delay(300);
  }
  printServoState();
}

// ====================================================
// Sorting Logic
// ====================================================
String binLabelFor(const String& category) {
  if (category == "plastic") return "♻️ PLASTIC";
  if (category == "metal")   return "♻️ METAL";
  if (category == "glass")   return "♻️ GLASS";
  if (category == "paper")   return "♻️ PAPER (Other bin)";
  if (category == "organics")return "♻️ ORGANICS (Other bin)";
  if (category == "wood")    return "♻️ WOOD (Other bin)";
  return "❓ UNKNOWN (Other bin)";
}

String binMechFor(const String& category) {
  if (category == "plastic") return "plastic";
  if (category == "metal")   return "metal";
  if (category == "glass")   return "glass";
  return "other";
}

void sortWaste(const String& mechBin) {
  Serial.println("[SORT] Mech start for: " + mechBin);
  int targetD9  = D9_HOME;
  int targetD13 = D13_HOME;
  
  if (mechBin == "plastic")     { targetD9 = D9_HOME; targetD13 = D13_HOME; }
  else if (mechBin == "metal")  { targetD9 = D9_OPEN; targetD13 = D13_HOME; }
  else if (mechBin == "glass")  { targetD9 = D9_HOME; targetD13 = D13_OPEN; }
  else                          { targetD9 = D9_OPEN; targetD13 = D13_OPEN; }

  Serial.printf("[SORT] Targets: D9=%d, D13=%d\n", targetD9, targetD13);
  moveTwoServosSmooth(SV_D9, targetD9, SV_D13, targetD13);
  delay(300);
  moveServoSmooth(SV_D14, D14_OPEN);
  delay(3000);
  moveServoSmooth(SV_D14, D14_MID);
  delay(1000);
  moveServoSmooth(SV_D14, D14_CLOSE);
  moveTwoServosSmooth(SV_D9, servoHome[SV_D9], SV_D13, servoHome[SV_D13]);
  delay(200);
  servoReleaseAll();
  Serial.println("[SORT] Done");
}

// ====================================================
// Base64 Encoder
// ====================================================
static const char B64_TBL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t base64Len(size_t n) { return 4 * ((n + 2) / 3); }

void base64Encode(const uint8_t* in, size_t len, char* out) {
  size_t i = 0, o = 0;
  while (i + 2 < len) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = B64_TBL[(v >> 18) & 63]; out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = B64_TBL[(v >> 6) & 63];  out[o++] = B64_TBL[v & 63];
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = B64_TBL[(v >> 18) & 63]; out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = B64_TBL[(v >> 18) & 63]; out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = B64_TBL[(v >> 6) & 63];  out[o++] = '=';
  }
}

// ====================================================
// Telegram Queue & Network
// ====================================================
void tgQueueText(const String& chat, const String& text) {
  if (chat.length() == 0) return;
  TgOut m = {};
  strlcpy(m.chatId, chat.c_str(), sizeof(m.chatId));
  strlcpy(m.text, text.c_str(), sizeof(m.text));
  m.photo = nullptr; m.photoLen = 0;
  if (xQueueSend(qOut, &m, 0) != pdTRUE) Serial.println("[TG] Queue overflow");
}

void tgQueuePhoto(const String& chat, const String& caption, Shot& shot) {
  if (chat.length() == 0 || !shot.data) {
    if (shot.data) { free(shot.data); shot.data = nullptr; }
    return;
  }
  TgOut m = {};
  strlcpy(m.chatId, chat.c_str(), sizeof(m.chatId));
  strlcpy(m.text, caption.c_str(), sizeof(m.text));
  m.photo = shot.data; m.photoLen = shot.len;
  
  if (xQueueSend(qOut, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("[TG] Queue overflow, dropping frame");
    free(shot.data);
  }
  shot.data = nullptr; shot.len = 0;
}

static WiFiClientSecure tgClient;
static HTTPClient       tgHttp;
static bool             tgClientInited = false;

void tgClientInit() {
  if (tgClientInited) return;
  tgClient.setInsecure();
  tgClient.setHandshakeTimeout(10);
  tgHttp.setReuse(true);
  tgHttp.setConnectTimeout(6000);
  tgClientInited = true;
}

bool needHandshakeGuard(WiFiClientSecure& c) { return !c.connected(); }

bool hsTake(uint32_t ms) {
  if (xSemaphoreTake(hsMux, pdMS_TO_TICKS(ms)) == pdTRUE) return true;
  Serial.printf("[LOCK] hsMux busy >%lu ms\n", (unsigned long)ms);
  return false;
}

bool tgSendText(const char* chatId, const String& text) {
  tgClientInit();
  bool held = needHandshakeGuard(tgClient) ? hsTake(6000) : false;

  tgHttp.setTimeout(8000);
  if (!tgHttp.begin(tgClient, String("https://") + TG_HOST + "/bot" + TG_TOKEN + "/sendMessage")) {
    if (held) xSemaphoreGive(hsMux);
    return false;
  }
  tgHttp.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["chat_id"] = chatId; doc["text"] = text;
  String body; serializeJson(doc, body);
  
  int code = tgHttp.POST(body);
  tgHttp.end();
  if (held) xSemaphoreGive(hsMux);

  if (code != 200) Serial.printf("[TG] sendMessage -> %d\n", code);
  return code == 200;
}

bool tgSendPhoto(const char* chatId, const char* caption, const uint8_t* jpeg, size_t len) {
  bool held = hsTake(8000);
  WiFiClientSecure c;
  c.setInsecure();
  c.setHandshakeTimeout(12);
  if (!c.connect(TG_HOST, 443)) {
    if (held) xSemaphoreGive(hsMux);
    return false;
  }
  
  const char* B = "----esp32cam0boundary";
  String head;
  head.reserve(450);
  head += "--"; head += B; head += "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  head += chatId; head += "\r\n--"; head += B; head += "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n";
  head += caption; head += "\r\n--"; head += B; head += "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"shot.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String tail = String("\r\n--") + B + "--\r\n";
  size_t contentLen = head.length() + len + tail.length();
  
  String req = String("POST /bot") + TG_TOKEN + "/sendPhoto HTTP/1.1\r\n";
  req += String("Host: ") + TG_HOST + "\r\n";
  req += String("Content-Type: multipart/form-data; boundary=") + B + "\r\n";
  req += "Content-Length: " + String(contentLen) + "\r\nConnection: close\r\n\r\n";
  
  c.print(req); c.print(head);
  const size_t CH = 2048;
  for (size_t i = 0; i < len; i += CH) {
    size_t n = (len - i < CH) ? (len - i) : CH;
    if (c.write(jpeg + i, n) != n) { c.stop(); if (held) xSemaphoreGive(hsMux); return false; }
    vTaskDelay(1);
  }
  c.print(tail);
  
  unsigned long t0 = millis();
  while (!c.available() && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(10));
  String status = c.readStringUntil('\n');
  c.stop();
  if (held) xSemaphoreGive(hsMux);
  return status.indexOf("200") > 0;
}

void tgSkipBacklog() {
  tgClientInit();
  bool held = hsTake(8000);
  tgHttp.setTimeout(8000);
  if (tgHttp.begin(tgClient, String("https://") + TG_HOST + "/bot" + TG_TOKEN + "/getUpdates?offset=-1&limit=1")) {
    if (tgHttp.GET() == 200) {
      String body = tgHttp.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, body)) {
        JsonArray r = doc["result"].as<JsonArray>();
        if (!r.isNull() && r.size() > 0) tgLastUpdateId = r[0]["update_id"].as<long>();
      }
    }
    tgHttp.end();
  }
  if (held) xSemaphoreGive(hsMux);
}

void handleTgCommand(const char* chatId, const String& cmd) {
  if (cmd.startsWith("/photo") || cmd.startsWith("/pic")) {
    TriggerEvt ev = {};
    strlcpy(ev.chatId, chatId, sizeof(ev.chatId));
    ev.withAI = cmd.startsWith("/photo");
    aiPending = true;
    if (xQueueSend(qTrigger, &ev, 0) != pdTRUE) aiPending = false;
  } else if (cmd.startsWith("/start")) {
    tgSendText(chatId, "Salam.\n/photo - photo + sort\n/pic - just frame\n/state - angles\n/time - timings");
  } else if (cmd.startsWith("/state")) {
    String s = "Angles:\n";
    for (int i = 0; i < SERVO_COUNT; i++) s += String(servoName[i]) + " = " + String(currentAngle[i]) + "\n";
    tgSendText(chatId, s);
  } else if (cmd.startsWith("/time")) {
    String s = "Last timings:\n";
    s += "cam: " + String(lastTiming.cam) + " ms\nnet: " + String(lastTiming.net) + " ms\n";
    s += "TOTAL: " + String(lastTiming.total) + " ms";
    tgSendText(chatId, s);
  }
}

void tgPollOnce() {
  tgClientInit();
  const uint16_t pollS = 1;
  String url = String("https://") + TG_HOST + "/bot" + TG_TOKEN + "/getUpdates?limit=1&timeout=" + String(pollS) + "&offset=" + String(tgLastUpdateId + 1);
  bool held = needHandshakeGuard(tgClient) ? hsTake(6000) : false;

  tgHttp.setTimeout((pollS + 6) * 1000);
  if (!tgHttp.begin(tgClient, url)) { if (held) xSemaphoreGive(hsMux); return; }
  
  int code = tgHttp.GET();
  String body;
  if (code == 200) body = tgHttp.getString();
  tgHttp.end();
  if (held) xSemaphoreGive(hsMux);

  if (code != 200) { vTaskDelay(pdMS_TO_TICKS(500)); return; }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  JsonArray results = doc["result"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return;
  
  JsonObject upd = results[0];
  tgLastUpdateId = upd["update_id"].as<long>();
  const char* text = upd["message"]["text"];
  if (!text) return;
  
  long long chat = upd["message"]["chat"]["id"].as<long long>();
  char chatId[24];
  snprintf(chatId, sizeof(chatId), "%lld", chat);
  setDefaultChat(chatId);
  handleTgCommand(chatId, String(text));
}

void telegramTask(void* pv) {
  tgSkipBacklog();
  for (;;) {
    TgOut m;
    while (xQueueReceive(qOut, &m, 0) == pdTRUE) {
      if (m.photo) { tgSendPhoto(m.chatId, m.text, m.photo, m.photoLen); free(m.photo); }
      else { tgSendText(m.chatId, m.text); }
    }
    if (!aiPending && !busy && WiFi.status() == WL_CONNECTED) tgPollOnce();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ====================================================
// Camera
// ====================================================
bool initCamera() {
  if (cameraInitialized) {
    esp_camera_deinit();
    cameraInitialized = false;
    delay(100);
  }
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = CAM_FRAMESIZE;
  config.jpeg_quality = CAM_QUALITY;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;
  
  cameraInitialized = true;
  lastCamUseMs = millis();
  return true;
}

bool takeShot(Shot& s) {
  s.data = nullptr; s.len = 0;
  if (!cameraInitialized && !initCamera()) return false;
  
  lastCamUseMs = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    if (!initCamera()) return false;
    delay(250);
    fb = esp_camera_fb_get();
    if (!fb) return false;
  }
  
  uint8_t* buf = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(fb->len);
  if (!buf) { esp_camera_fb_return(fb); return false; }
  
  memcpy(buf, fb->buf, fb->len);
  s.data = buf; s.len = fb->len;
  esp_camera_fb_return(fb);
  return true;
}

// ====================================================
// AI & Gemini API
// ====================================================
String extractCategory(const String& rawText) {
  String cleaned = rawText;
  int tStart = cleaned.indexOf("<think>");
  int tEnd = cleaned.indexOf("</think>");
  if (tStart >= 0 && tEnd >= 0) cleaned = cleaned.substring(tEnd + 8);
  else if (tStart >= 0) cleaned = cleaned.substring(tStart + 7);
  
  cleaned.toLowerCase(); cleaned.trim();
  while (cleaned.length() > 0 && (cleaned.endsWith(".") || cleaned.endsWith(",") || cleaned.endsWith("!"))) {
    cleaned.remove(cleaned.length() - 1); cleaned.trim();
  }
  
  for (int i = 0; i < CATEGORY_COUNT; i++) if (cleaned.indexOf(CATEGORIES[i]) >= 0) return CATEGORIES[i];
  for (int i = 0; i < RUSSIAN_MAP_COUNT; i++) if (cleaned.indexOf(RUSSIAN_MAP[i].ru) >= 0) return RUSSIAN_MAP[i].en;
  return "unknown";
}

static WiFiClientSecure gemClient;
static HTTPClient       gemHttp;
static bool             gemInited = false;
static unsigned long    gemLastUseMs = 0;
static bool             gemMinimalBody = false;

String gemUrl(const char* model) { return String("https://") + GEM_HOST + "/v1beta/models/" + model + ":generateContent"; }

void gemInit() {
  if (gemInited) return;
  gemClient.setInsecure();
  gemClient.setHandshakeTimeout(GEM_HANDSHAKE_SEC);
  gemHttp.setReuse(true);
  gemHttp.setConnectTimeout(GEM_CONNECT_TIMEOUT);
  gemHttp.setTimeout(GEM_TIMEOUT_MS);
  gemInited = true;
}

bool gemWarmUp() {
  gemInit();
  const char* body = "{\"contents\":[{\"parts\":[{\"text\":\"ok\"}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":8}}";
  bool held = hsTake(8000);
  bool ok = false;
  
  if (gemHttp.begin(gemClient, gemUrl(GEM_MODEL_FAST))) {
    gemHttp.addHeader("Content-Type", "application/json");
    gemHttp.addHeader("x-goog-api-key", GEMINI_KEY);
    int code = gemHttp.POST((uint8_t*)body, strlen(body));
    gemHttp.end();
    ok = (code == 200);
  }
  if (held) xSemaphoreGive(hsMux);
  gemLastUseMs = millis();
  return ok;
}

void gemKeepAlive() {
  if (!gemInited || millis() - gemLastUseMs < GEM_KEEPALIVE_MS) return;
  if (!gemClient.connected()) gemWarmUp();
  gemLastUseMs = millis();
}

int gemRequest(const char* model, const uint8_t* jpeg, size_t jpegLen, String& respOut, bool minimalBody, uint32_t* netMs, uint32_t* b64Ms) {
  gemInit();
  unsigned long tB0 = millis();
  
  String prefix = "{\"contents\":[{\"parts\":[{\"text\":\"" + String(PROMPT_JSON) + "\"},{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"";
  const char* suffixFull = "\"}}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":40,\"thinkingConfig\":{\"thinkingLevel\":\"minimal\"},\"mediaResolution\":\"MEDIA_RESOLUTION_LOW\"}}";
  const char* suffixMin  = "\"}}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":40}}";
  const char* suffix = minimalBody ? suffixMin : suffixFull;

  size_t b64n = base64Len(jpegLen);
  size_t total = prefix.length() + b64n + strlen(suffix);
  uint8_t* body = (uint8_t*)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
  if (!body) body = (uint8_t*)malloc(total + 1);
  if (!body) { respOut = "no_mem"; return -8; }

  memcpy(body, prefix.c_str(), prefix.length());
  base64Encode(jpeg, jpegLen, (char*)(body + prefix.length()));
  memcpy(body + prefix.length() + b64n, suffix, strlen(suffix));
  body[total] = '\0';
  if (b64Ms) *b64Ms = millis() - tB0;

  bool warm = gemClient.connected();
  bool held = !warm ? hsTake(4000) : false;
  unsigned long tN0 = millis();
  int code;
  
  if (!gemHttp.begin(gemClient, gemUrl(model))) {
    free(body); if (held) xSemaphoreGive(hsMux); return -1;
  }
  gemHttp.setTimeout(GEM_TIMEOUT_MS);
  gemHttp.addHeader("Content-Type", "application/json");
  gemHttp.addHeader("x-goog-api-key", GEMINI_KEY);
  code = gemHttp.POST(body, total);

  if (warm && (code == HTTPC_ERROR_CONNECTION_LOST || code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_NOT_CONNECTED)) {
    gemHttp.end();
    gemClient.stop();
    bool held2 = hsTake(4000);
    if (gemHttp.begin(gemClient, gemUrl(model))) {
      gemHttp.setTimeout(GEM_TIMEOUT_MS);
      gemHttp.addHeader("Content-Type", "application/json");
      gemHttp.addHeader("x-goog-api-key", GEMINI_KEY);
      code = gemHttp.POST(body, total);
    }
    if (held2) xSemaphoreGive(hsMux);
  }

  free(body);
  respOut = (code > 0) ? gemHttp.getString() : String("");
  gemHttp.end();
  if (held) xSemaphoreGive(hsMux);

  if (netMs) *netMs = millis() - tN0;
  gemLastUseMs = millis();
  return code;
}

bool classifyWaste(const uint8_t* jpeg, size_t jpegLen, String& rawOut, String& catOut) {
  const char* models[2] = { GEM_MODEL_FAST, GEM_MODEL_FALLBACK };

  for (int m = 0; m < 2; m++) {
    String resp;
    uint32_t netMs = 0, b64Ms = 0;
    int code = gemRequest(models[m], jpeg, jpegLen, resp, gemMinimalBody, &netMs, &b64Ms);
    
    lastTiming.net = netMs;
    lastTiming.b64 = b64Ms;

    if (code == 400 && !gemMinimalBody && (resp.indexOf("Unknown name") >= 0 || resp.indexOf("Invalid JSON") >= 0)) {
      gemMinimalBody = true;
      code = gemRequest(models[m], jpeg, jpegLen, resp, true, &netMs, &b64Ms);
      lastTiming.net += netMs;
    }

    if (code != 200) {
      rawOut = "http_" + String(code); catOut = "error";
      if (!(code == 429 || code == 503 || code == 500 || code == HTTPC_ERROR_READ_TIMEOUT || code < 0)) return false;
      continue;
    }

    unsigned long tP0 = millis();
    JsonDocument filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    filter["candidates"][0]["finishReason"] = true;
    
    JsonDocument doc;
    DeserializationError derr = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
    lastTiming.parse = millis() - tP0;

    if (derr) { rawOut = "parse_error"; catOut = "error"; continue; }

    const char* content = doc["candidates"][0]["content"]["parts"][0]["text"];
    const char* finish  = doc["candidates"][0]["finishReason"] | "?";
    
    if (!content) { rawOut = String("empty:") + finish; catOut = "error"; continue; }

    rawOut = String(content); rawOut.trim();
    catOut = extractCategory(rawOut);
    return true;
  }
  return false;
}

// ====================================================
// Main Recognition Cycle
// ====================================================
void runRecognition(const String& chatId, bool withAI) {
  busy = true;
  unsigned long t0 = millis();
  unsigned long tC0 = millis();
  
  Shot shot;
  if (!takeShot(shot)) {
    tgQueueText(chatId, "❌ Frame capture failed");
    busy = false; aiPending = false;
    return;
  }
  lastTiming.cam = millis() - tC0;

  if (!withAI) {
    tgQueuePhoto(chatId, "📷 Frame (" + String(shot.len) + " b)", shot);
    busy = false; aiPending = false;
    return;
  }

  String raw, category;
  bool ok = classifyWaste(shot.data, shot.len, raw, category);
  lastTiming.total = millis() - t0;

  if (!ok) {
    tgQueuePhoto(chatId, "⚠️ AI Error: " + raw + "\n⏱ " + String(lastTiming.total / 1000.0, 1) + " s", shot);
    busy = false; aiPending = false;
    return;
  }

  String answer = binLabelFor(category);
  answer += "\n⏱ " + String(lastTiming.total / 1000.0, 1) + " s";
  tgQueueText(chatId, answer);

  vTaskDelay(pdMS_TO_TICKS(30)); // allow Telegram to send text
  sortWaste(binMechFor(category));
  tgQueuePhoto(chatId, "📷 " + category + " - sorted", shot);

  busy = false; aiPending = false;
}

// ====================================================
// Button & Setup
// ====================================================
#define BUTTON_ACTIVE_LOW   1
#define BTN_DEBOUNCE_MS     40

bool buttonIsDown() {
  int lvl = digitalRead(BUTTON_PIN);
  return BUTTON_ACTIVE_LOW ? (lvl == LOW) : (lvl == HIGH);
}

void checkButton() {
  static bool stable = false, lastRaw = false;
  static unsigned long lastChangeMs = 0;
  bool raw = buttonIsDown();
  
  if (raw != lastRaw) { lastRaw = raw; lastChangeMs = millis(); return; }
  if (millis() - lastChangeMs < BTN_DEBOUNCE_MS) return;
  if (raw == stable) return;
  stable = raw;
  
  if (!raw) { buttonPressed = false; return; }
  buttonPressed = true;
  if (busy) return;
  
  String chatId = getDefaultChat();
  if (chatId.length() == 0) chatId = String(TG_OWNER_CHAT);
  
  TriggerEvt ev = {};
  strlcpy(ev.chatId, chatId.c_str(), sizeof(ev.chatId));
  ev.withAI = true;
  aiPending = true;
  if (xQueueSend(qTrigger, &ev, 0) != pdTRUE) aiPending = false;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== IWSR Booting ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (!initCamera()) {
    Serial.println("Camera error - check FPC cable.");
    while (true) delay(1000);
  }

  initServos();
  servoReleaseAll();

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  setCpuFrequencyMhz(240);

  qTrigger = xQueueCreate(4, sizeof(TriggerEvt));
  qOut     = xQueueCreate(6, sizeof(TgOut));
  hsMux    = xSemaphoreCreateMutex();
  chatMux  = xSemaphoreCreateMutex();

  setDefaultChat(TG_OWNER_CHAT);

  gemWarmUp(); // Pre-establish TLS

  xTaskCreatePinnedToCore(telegramTask, "telegram", 16384, NULL, 1, NULL, 0);
  Serial.println("Ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  checkButton();

  if (!busy && (millis() - lastTriggerMs > TRIGGER_COOLDOWN_MS)) {
    TriggerEvt ev;
    if (xQueueReceive(qTrigger, &ev, 0) == pdTRUE) {
      String replyChat = String(ev.chatId);
      if (replyChat.length() == 0) replyChat = getDefaultChat();
      lastTriggerMs = millis();
      runRecognition(replyChat, ev.withAI);
    } else {
      gemKeepAlive();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(20));
}
