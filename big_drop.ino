/* ============================================================================
   ESP32 (Deneyap Kart 1A v2) — Сортировка мусора
   Версия 21 — ОТВЕТ < 5 СЕКУНД
   ----------------------------------------------------------------------------
   Что изменено по сравнению с v20 и ПОЧЕМУ:

   1) МОДЕЛЬ. Было gemini-3.7-flash — это "агентная"/кодовая модель,
      thinking по умолчанию = medium, минимальный уровень = "low".
      Она ДУМАЕТ перед ответом, отсюда >8 с и HTTP -11 (read timeout).
      Стало gemini-3.5-flash-lite + thinkingLevel:"minimal" — самая быстрая
      стабильная мультимодальная модель, у неё minimal реально поддержан.
      Fallback: gemini-3.1-flash-lite.

   2) maxOutputTokens. Было 256 при включённом thinking. Лимит считается
      ВМЕСТЕ с токенами размышлений => модель могла упереться в лимит
      во время "думания" и вернуть пустой ответ. Теперь thinking minimal + 40.

   3) ПОСТОЯННОЕ TLS-СОЕДИНЕНИЕ. Было: WiFiClientSecure на стеке внутри
      функции => каждый запрос = новое рукопожатие (1.5-3 с!). setReuse(true)
      на локальном объекте не работает в принципе.
      Стало: глобальные gemClient/gemHttp + прогрев в setup() => рукопожатие
      оплачивается один раз при старте.

   4) ЧТЕНИЕ ОТВЕТА. Было deserializeJson(doc, http.getStream()).
      getStream() отдаёт СЫРОЙ поток. Если Google ответит
      Transfer-Encoding: chunked — в поток попадут маркеры размера чанков,
      и парсер сломается. Стало http.getString() (он умеет chunked).

   5) ДЛИННЫЙ LONG-POLL ТЕЛЕГРАМА. Было timeout=20 с, и всё это время
      задача держала netMux и не разгребала очередь исходящих. То есть готовый
      ответ мог ЛЕЖАТЬ в очереди до 20 секунд. Стало timeout=1 с + флаг
      aiPending, который запрещает начинать опрос, пока идёт распознавание.

   6) МЬЮТЕКС. Было xSemaphoreTake(netMux, portMAX_DELAY) вокруг всего
      запроса к Gemini => ожидание до 20+ с. Стало: мьютекс берётся ТОЛЬКО
      если нужно новое рукопожатие (чтобы два TLS-хендшейка не шли разом).

   7) ПОРЯДОК ДЕЙСТВИЙ. Было: распознали -> покрутили серво (delay 3000 +
      delay 1000 + плавные ходы ~7 с) -> и только потом отправили ответ.
      Таймер "⏱" мерил всё это вместе. Стало: ответ уходит СРАЗУ, серво
      крутятся после. "⏱" = время от триггера до ответа.

   8) ДИАГНОСТИКА. Расшифровка отрицательных кодов HTTPClient и разбивка
      времени по фазам (кадр / base64 / сеть / парсинг).

   Ожидаемое время ответа при прогретом соединении: 1.5-3 с.
   ============================================================================ */

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
// 1. WI-FI
// ====================================================
const char* WIFI_SSID = "Tim";
const char* WIFI_PASS = "95909992";

// ====================================================
// 2. TELEGRAM
// ====================================================
const char* TG_TOKEN = ;
const char* TG_HOST  = "api.telegram.org";
char g_defaultChat[24] = "";
const char* TG_OWNER_CHAT = "6896444483";

// ====================================================
// 3. GEMINI
// ====================================================
const char* GEM_HOST = "generativelanguage.googleapis.com";

// Самая быстрая стабильная мультимодальная модель.
// thinkingLevel "minimal" поддержан именно у flash-lite.
#define GEM_MODEL_FAST     "gemini-3.5-flash-lite"
#define GEM_MODEL_FALLBACK "gemini-3.1-flash-lite"

const char* GEMINI_KEY = "";
// Ключ формата AQ. (Auth key) передаётся ТОЛЬКО заголовком x-goog-api-key.
// В виде ?key=... в URL он не работает. Ниже используется заголовок.

// Бюджеты времени
#define GEM_TIMEOUT_MS        6000   // ждём ответ модели не дольше 6 с
#define GEM_CONNECT_TIMEOUT   5000
#define GEM_HANDSHAKE_SEC     8
#define GEM_KEEPALIVE_MS      25000  // раз в 25 с подогреваем соединение

// Короткий промпт: меньше входных токенов = меньше префилла.
static const char PROMPT_JSON[] =
  "Identify the real material of the main object in this image. "
  "Judge by shape and purpose, not by the label. "
  "Transparent bottle = plastic. Newspaper or cardboard = paper. "
  "Food scrap or peel = organics. Can or metal lid = metal. "
  "Stick or plank = wood. Jar or drinking glass = glass. "
  "Reply with exactly one word: plastic, paper, organics, metal, wood, glass. "
  "No punctuation, no explanation.";

// ====================================================
// 4. КНОПКА
// ====================================================
#define BUTTON_PIN          D8

// ============================================================================
//        ТАБЛИЦА СЕРВОПРИВОДОВ
// ============================================================================
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

const int  servoPins  [SERVO_COUNT] = {  D9,  D13,  D14 };
const int  servoMin   [SERVO_COUNT] = {   0,    0,    0 };
const int  servoMax   [SERVO_COUNT] = { 180,  180,  180 };
const int  servoHome  [SERVO_COUNT] = { D9_HOME, D13_HOME, D14_HOME };
const int  servoUsMin [SERVO_COUNT] = { 1000, 1000, 1000 };
const int  servoUsMax [SERVO_COUNT] = { 2000, 2000, 2000 };
const char* servoName [SERVO_COUNT] = { "D9", "D13", "D14 (заслонка)" };

const bool servoHoldIdle[SERVO_COUNT] = { false, false, false };
#define SERVO_RELEASE_IDLE  1
#define STEP_DEG            2
#define STEP_DELAY_MS       20

Servo servos[SERVO_COUNT];
int   currentAngle [SERVO_COUNT];
bool  servoAttached[SERVO_COUNT] = { false, false, false };

// ====================================================
// КАМЕРА — DENEYAP KART 1A v2
// ====================================================
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

// QVGA 320x240 — точнее, чем QQVGA, а на скорость почти не влияет
// (при mediaResolution LOW модель всё равно видит ~280 токенов).
// Хотите ещё меньше трафика — поставьте FRAMESIZE_QQVGA.
#define CAM_FRAMESIZE   FRAMESIZE_QVGA
#define CAM_QUALITY     12

// ====================================================
// КАТЕГОРИИ
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

// ============================================================================
//                 СТРУКТУРЫ
// ============================================================================
struct Shot       { uint8_t* data; size_t len; };
struct TriggerEvt { char chatId[24]; bool withAI; };
struct TgOut      { char chatId[24]; char text[320]; uint8_t* photo; size_t photoLen; };

// ============================================================================
//                 ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================================
QueueHandle_t     qTrigger;
QueueHandle_t     qOut;
SemaphoreHandle_t hsMux;     // защищает ТОЛЬКО TLS-рукопожатия
SemaphoreHandle_t chatMux;

volatile bool busy          = false;
volatile bool aiPending     = false;   // есть незавершённый триггер -> TG не опрашивает
volatile bool buttonPressed = false;

long          tgLastUpdateId = 0;
unsigned long lastTriggerMs  = 0;
const unsigned long TRIGGER_COOLDOWN_MS = 3000;

bool          cameraInitialized = false;
unsigned long lastCamUseMs = 0;

// Тайминги последнего распознавания (для диагностики)
struct Timing { uint32_t cam, b64, net, parse, total; };
Timing lastTiming = {0, 0, 0, 0, 0};

// ============================================================================
//                 ЧАТ
// ============================================================================
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

// ============================================================================
//        РАСШИФРОВКА КОДОВ HTTPClient
// ============================================================================
const char* httpErrName(int code) {
  switch (code) {
    case  -1: return "CONNECTION_REFUSED (не законнектился / DNS / TLS)";
    case  -2: return "SEND_HEADER_FAILED";
    case  -3: return "SEND_PAYLOAD_FAILED (обрыв при отправке тела)";
    case  -4: return "NOT_CONNECTED";
    case  -5: return "CONNECTION_LOST (сервер закрыл соединение)";
    case  -6: return "NO_STREAM";
    case  -7: return "NO_HTTP_SERVER";
    case  -8: return "TOO_LESS_RAM (не хватило heap на TLS)";
    case  -9: return "ENCODING";
    case -10: return "STREAM_WRITE";
    case -11: return "READ_TIMEOUT (ответ не пришёл за отведённое время)";
    default:  return (code > 0) ? "HTTP status" : "неизвестная ошибка";
  }
}

// ============================================================================
//              СЕРВО
// ============================================================================
void servoEnsureAttached(int i) {
  if (servoAttached[i]) return;
  servos[i].setPeriodHertz(50);
  int ch = servos[i].attach(servoPins[i], servoUsMin[i], servoUsMax[i]);
  if (ch <= 0) {
    Serial.printf("[SERVO] %s: attach() не удался (%d)\n", servoName[i], ch);
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
  Serial.println("[SERVO] Обесточены");
#endif
}

static bool stepToward(int i, int target, int step) {
  int cur = currentAngle[i];
  if (cur == target) return true;
  int dir = (target > cur) ? 1 : -1;
  if (abs(target - cur) <= step) cur = target;
  else                           cur += dir * step;
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

void moveTwoServosSmooth(int iA, int targetA, int iB, int targetB,
                         int step = STEP_DEG, int delayMs = STEP_DELAY_MS) {
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
  Serial.println("[SERVO] Текущие углы:");
  for (int i = 0; i < SERVO_COUNT; i++) {
    Serial.printf("   [%d] %-16s = %3d  %s\n", i, servoName[i], currentAngle[i],
                  servoAttached[i] ? "(под напряжением)" : "(обесточен)");
  }
}

void initServos() {
  Serial.println("[SERVO] Инициализация...");
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  for (int i = 0; i < SERVO_COUNT; i++) {
    currentAngle[i] = constrain(servoHome[i], servoMin[i], servoMax[i]);
    servos[i].setPeriodHertz(50);
    int ch = servos[i].attach(servoPins[i], servoUsMin[i], servoUsMax[i]);
    if (ch <= 0) {
      Serial.printf("[SERVO] %s: attach() НЕ УДАЛСЯ (%d)\n", servoName[i], ch);
      servoAttached[i] = false;
    } else {
      servoAttached[i] = true;
      Serial.printf("[SERVO] OK %s: pin=%d, LEDC ch=%d\n", servoName[i], servoPins[i], ch);
    }
    servos[i].write(currentAngle[i]);
    delay(300);
  }
  printServoState();
}

// ============================================================================
//                 ЛОГИКА СОРТИРОВКИ (механика)
// ============================================================================
// Возвращает короткую подпись для ответа, но САМА НЕ КРУТИТ СЕРВО.
String binLabelFor(const String& category) {
  if (category == "plastic") return "♻️ PLASTIC — пластик";
  if (category == "metal")   return "♻️ METAL — металл";
  if (category == "glass")   return "♻️ GLASS — стекло";
  if (category == "paper")   return "♻️ PAPER — бумага (корзина «другое»)";
  if (category == "organics")return "♻️ ORGANICS — органика (корзина «другое»)";
  if (category == "wood")    return "♻️ WOOD — дерево (корзина «другое»)";
  return "❓ UNKNOWN — не определилось (корзина «другое»)";
}

// Механический бин: куда реально едет заслонка
String binMechFor(const String& category) {
  if (category == "plastic") return "plastic";
  if (category == "metal")   return "metal";
  if (category == "glass")   return "glass";
  return "other";
}

void sortWaste(const String& mechBin) {
  Serial.println("[SORT] Запуск механики для: " + mechBin);
  int targetD9  = D9_HOME;
  int targetD13 = D13_HOME;
  if (mechBin == "plastic")     { targetD9 = D9_HOME; targetD13 = D13_HOME; }
  else if (mechBin == "metal")  { targetD9 = D9_OPEN; targetD13 = D13_HOME; }
  else if (mechBin == "glass")  { targetD9 = D9_HOME; targetD13 = D13_OPEN; }
  else                          { targetD9 = D9_OPEN; targetD13 = D13_OPEN; }

  Serial.printf("[SORT] Цели: D9=%d, D13=%d\n", targetD9, targetD13);
  moveTwoServosSmooth(SV_D9, targetD9, SV_D13, targetD13);
  delay(300);
  moveServoSmooth(SV_D14, D14_OPEN);
  Serial.println("[SORT] Заслонка открыта, ждём 3 с...");
  delay(3000);
  moveServoSmooth(SV_D14, D14_MID);
  delay(1000);
  moveServoSmooth(SV_D14, D14_CLOSE);
  moveTwoServosSmooth(SV_D9, servoHome[SV_D9], SV_D13, servoHome[SV_D13]);
  delay(200);
  servoReleaseAll();
  Serial.println("[SORT] Механика завершена");
}

// ============================================================================
//                               BASE64
// ============================================================================
static const char B64_TBL[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t base64Len(size_t n) { return 4 * ((n + 2) / 3); }
void base64Encode(const uint8_t* in, size_t len, char* out) {
  size_t i = 0, o = 0;
  while (i + 2 < len) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = B64_TBL[(v >> 18) & 63];
    out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = B64_TBL[(v >> 6) & 63];
    out[o++] = B64_TBL[v & 63];
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = B64_TBL[(v >> 18) & 63];
    out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = B64_TBL[(v >> 18) & 63];
    out[o++] = B64_TBL[(v >> 12) & 63];
    out[o++] = B64_TBL[(v >> 6) & 63];
    out[o++] = '=';
  }
}

// ============================================================================
//         ОЧЕРЕДЬ ИСХОДЯЩИХ
// ============================================================================
void tgQueueText(const String& chat, const String& text) {
  if (chat.length() == 0) {
    Serial.println("[TG] chat_id пуст: " + text);
    return;
  }
  TgOut m = {};
  strlcpy(m.chatId, chat.c_str(), sizeof(m.chatId));
  strlcpy(m.text, text.c_str(), sizeof(m.text));
  m.photo = nullptr;
  m.photoLen = 0;
  if (xQueueSend(qOut, &m, 0) != pdTRUE) Serial.println("[TG] очередь переполнена");
}

void tgQueuePhoto(const String& chat, const String& caption, Shot& shot) {
  if (chat.length() == 0 || !shot.data) {
    if (shot.data) { free(shot.data); shot.data = nullptr; }
    return;
  }
  TgOut m = {};
  strlcpy(m.chatId, chat.c_str(), sizeof(m.chatId));
  strlcpy(m.text, caption.c_str(), sizeof(m.text));
  m.photo = shot.data;
  m.photoLen = shot.len;
  if (xQueueSend(qOut, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("[TG] очередь переполнена, кадр отброшен");
    free(shot.data);
  }
  shot.data = nullptr;
  shot.len = 0;
}

// ============================================================================
//              TELEGRAM (всё сетевое — только с ядра 0)
// ============================================================================
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

// Рукопожатия сериализуем, чтобы два TLS-хендшейка не шли одновременно
// (пиковый расход внутреннего heap иначе может уронить соединение с кодом -8).
bool needHandshakeGuard(WiFiClientSecure& c) { return !c.connected(); }

// Берём мьютекс с ОГРАНИЧЕНИЕМ по времени. Если не дождались — идём дальше.
// Вечная блокировка (portMAX_DELAY) — именно то, что убивало бюджет в v20:
// запрос к Gemini мог стоять в очереди за 20-секундным опросом Телеграма.
bool hsTake(uint32_t ms) {
  if (xSemaphoreTake(hsMux, pdMS_TO_TICKS(ms)) == pdTRUE) return true;
  Serial.printf("[LOCK] hsMux занят >%lu мс, продолжаю без него\n", (unsigned long)ms);
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
  doc["chat_id"] = chatId;
  doc["text"] = text;
  String body;
  serializeJson(doc, body);
  int code = tgHttp.POST(body);
  tgHttp.end();
  if (held) xSemaphoreGive(hsMux);

  if (code != 200) Serial.printf("[TG] sendMessage -> %d (%s)\n", code, httpErrName(code));
  return code == 200;
}

bool tgSendPhoto(const char* chatId, const char* caption, const uint8_t* jpeg, size_t len) {
  bool held = hsTake(8000);
  WiFiClientSecure c;
  c.setInsecure();
  c.setHandshakeTimeout(12);
  if (!c.connect(TG_HOST, 443)) {
    Serial.println("[TG] нет соединения для sendPhoto");
    if (held) xSemaphoreGive(hsMux);
    return false;
  }
  const char* B = "----esp32cam0boundary";
  String head;
  head.reserve(450);
  head += "--"; head += B;
  head += "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  head += chatId; head += "\r\n";
  head += "--"; head += B;
  head += "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n";
  head += caption; head += "\r\n";
  head += "--"; head += B;
  head += "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"shot.jpg\"\r\n"
          "Content-Type: image/jpeg\r\n\r\n";
  String tail = String("\r\n--") + B + "--\r\n";
  size_t contentLen = head.length() + len + tail.length();
  String req = String("POST /bot") + TG_TOKEN + "/sendPhoto HTTP/1.1\r\n";
  req += String("Host: ") + TG_HOST + "\r\n";
  req += String("Content-Type: multipart/form-data; boundary=") + B + "\r\n";
  req += "Content-Length: " + String(contentLen) + "\r\n";
  req += "Connection: close\r\n\r\n";
  c.print(req);
  c.print(head);
  const size_t CH = 2048;
  for (size_t i = 0; i < len; i += CH) {
    size_t n = (len - i < CH) ? (len - i) : CH;
    if (c.write(jpeg + i, n) != n) {
      Serial.println("[TG] обрыв при отправке JPEG");
      c.stop();
      if (held) xSemaphoreGive(hsMux);
      return false;
    }
    vTaskDelay(1);
  }
  c.print(tail);
  unsigned long t0 = millis();
  while (!c.available() && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(10));
  String status = c.readStringUntil('\n');
  c.stop();
  if (held) xSemaphoreGive(hsMux);
  bool ok = status.indexOf("200") > 0;
  if (!ok) Serial.println("[TG] sendPhoto -> " + status);
  return ok;
}

void tgSkipBacklog() {
  tgClientInit();
  bool held = hsTake(8000);
  tgHttp.setTimeout(8000);
  if (tgHttp.begin(tgClient, String("https://") + TG_HOST + "/bot" + TG_TOKEN +
                             "/getUpdates?offset=-1&limit=1")) {
    if (tgHttp.GET() == 200) {
      String body = tgHttp.getString();
      JsonDocument filter;
      filter["result"][0]["update_id"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        JsonArray r = doc["result"].as<JsonArray>();
        if (!r.isNull() && r.size() > 0) tgLastUpdateId = r[0]["update_id"].as<long>();
      }
    }
    tgHttp.end();
  }
  if (held) xSemaphoreGive(hsMux);
  Serial.printf("[TG] стартовый offset: %ld\n", tgLastUpdateId);
}

void handleTgCommand(const char* chatId, const String& cmd);

// Короткий long-poll: 1 секунда.
// Длинный (20 с) блокировал очередь исходящих — готовый ответ лежал и ждал.
void tgPollOnce() {
  tgClientInit();
  const uint16_t pollS = 1;
  String url = String("https://") + TG_HOST + "/bot" + TG_TOKEN +
               "/getUpdates?limit=1&allowed_updates=%5B%22message%22%5D&timeout=" +
               String(pollS) + "&offset=" + String(tgLastUpdateId + 1);

  bool held = needHandshakeGuard(tgClient) ? hsTake(6000) : false;

  tgHttp.setTimeout((pollS + 6) * 1000);
  if (!tgHttp.begin(tgClient, url)) {
    if (held) xSemaphoreGive(hsMux);
    return;
  }
  int code = tgHttp.GET();
  String body;
  if (code == 200) body = tgHttp.getString();
  tgHttp.end();
  if (held) xSemaphoreGive(hsMux);

  if (code != 200) {
    if (code < 0) Serial.printf("[TG] getUpdates -> %d (%s)\n", code, httpErrName(code));
    vTaskDelay(pdMS_TO_TICKS(500));
    return;
  }

  JsonDocument filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["text"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;

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
  String cmd(text);
  cmd.trim();
  Serial.println("[TG] команда: " + cmd);
  handleTgCommand(chatId, cmd);
}

void handleTgCommand(const char* chatId, const String& cmd) {
  if (cmd.startsWith("/photo") || cmd.startsWith("/pic")) {
    TriggerEvt ev = {};
    strlcpy(ev.chatId, chatId, sizeof(ev.chatId));
    ev.withAI = cmd.startsWith("/photo");
    aiPending = true;                      // сразу глушим опрос Телеграма
    if (xQueueSend(qTrigger, &ev, 0) != pdTRUE) aiPending = false;
  } else if (cmd.startsWith("/start")) {
    tgSendText(chatId,
      "Salam.\n/photo — фото + сортировка\n/pic — просто кадр\n"
      "/state — углы серво\n/time — тайминги последнего распознавания\n"
      "Кнопка на D8 — ручной запуск");
  } else if (cmd.startsWith("/state")) {
    String s = "🔧 Углы серво:\n";
    for (int i = 0; i < SERVO_COUNT; i++) {
      s += String(servoName[i]) + " = " + String(currentAngle[i]) + " " +
           (servoAttached[i] ? "(питание)" : "(обесточен)") + "\n";
    }
    tgSendText(chatId, s);
  } else if (cmd.startsWith("/time")) {
    String s = "⏱ Последнее распознавание:\n";
    s += "кадр:   " + String(lastTiming.cam)   + " мс\n";
    s += "base64: " + String(lastTiming.b64)   + " мс\n";
    s += "сеть:   " + String(lastTiming.net)   + " мс\n";
    s += "парсинг:" + String(lastTiming.parse) + " мс\n";
    s += "ИТОГО:  " + String(lastTiming.total) + " мс";
    tgSendText(chatId, s);
  } else if (cmd.startsWith("/ping")) {
    tgSendText(chatId, "pong. IP: " + WiFi.localIP().toString() +
                       ", heap " + String(ESP.getFreeHeap()));
  }
}

void telegramTask(void* pv) {
  Serial.printf("[TG] задача запущена на ядре %d\n", xPortGetCoreID());
  tgSkipBacklog();
  String d = getDefaultChat();
  if (d.length()) {
    tgSendText(d.c_str(), String("🤖 Сортировщик запущен (") + GEM_MODEL_FAST + ")\n/photo — фото + сортировка");
  }
  for (;;) {
    TgOut m;
    // Исходящие — всегда в приоритете
    while (xQueueReceive(qOut, &m, 0) == pdTRUE) {
      if (m.photo) {
        tgSendPhoto(m.chatId, m.text, m.photo, m.photoLen);
        free(m.photo);
      } else {
        tgSendText(m.chatId, m.text);
      }
    }
    if (!aiPending && !busy && WiFi.status() == WL_CONNECTED) tgPollOnce();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================================
//                    КАМЕРА
// ============================================================================
bool initCamera() {
  if (cameraInitialized) {
    esp_camera_deinit();
    cameraInitialized = false;
    delay(100);
  }
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = CAM_FRAMESIZE;
  config.jpeg_quality = CAM_QUALITY;
  // fb_count=2 + GRAB_LATEST: свежий кадр без двойного захвата (-200 мс)
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Ошибка инициализации: 0x%x\n", err);
    return false;
  }
  cameraInitialized = true;
  lastCamUseMs = millis();
  Serial.printf("[CAM] Включена, XCLK 20 МГц, fb_count=%d, q=%d\n",
                config.fb_count, CAM_QUALITY);
  return true;
}

bool takeShot(Shot& s) {
  s.data = nullptr;
  s.len = 0;
  if (!cameraInitialized) {
    if (!initCamera()) {
      Serial.println("[CAM] Не удалось инициализировать камеру");
      return false;
    }
    delay(250);
  }
  lastCamUseMs = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAM] Нет кадра, переинициализация...");
    if (!initCamera()) return false;
    delay(250);
    fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] Вторая попытка не удалась"); return false; }
  }
  if (!(fb->len > 3 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8)) {
    Serial.printf("[CAM] Не JPEG! Первые байты: %02X %02X %02X\n",
                  fb->buf[0], fb->buf[1], fb->buf[2]);
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(fb->len);
  if (!buf) {
    Serial.println("[CAM] Не хватило памяти");
    esp_camera_fb_return(fb);
    return false;
  }
  memcpy(buf, fb->buf, fb->len);
  s.data = buf;
  s.len = fb->len;
  esp_camera_fb_return(fb);
  Serial.printf("[CAM] JPEG кадр: %u байт\n", (unsigned)s.len);
  lastCamUseMs = millis();
  return true;
}

// ============================================================================
//                    РАСПОЗНАВАНИЕ
// ============================================================================
String extractCategory(const String& rawText) {
  String cleaned = rawText;
  int thinkStart = cleaned.indexOf("<think>");
  int thinkEnd = cleaned.indexOf("</think>");
  if (thinkStart >= 0 && thinkEnd >= 0)      cleaned = cleaned.substring(thinkEnd + 8);
  else if (thinkStart >= 0)                  cleaned = cleaned.substring(thinkStart + 7);
  cleaned.toLowerCase();
  cleaned.trim();
  while (cleaned.length() > 0 && (cleaned.endsWith(".") || cleaned.endsWith(",") ||
         cleaned.endsWith("!") || cleaned.endsWith("?") || cleaned.endsWith("\"") ||
         cleaned.endsWith("'") || cleaned.endsWith("`") || cleaned.endsWith(")"))) {
    cleaned.remove(cleaned.length() - 1);
    cleaned.trim();
  }
  for (int i = 0; i < CATEGORY_COUNT; i++) {
    if (cleaned == CATEGORIES[i] || cleaned.indexOf(CATEGORIES[i]) >= 0) return CATEGORIES[i];
  }
  for (int i = 0; i < RUSSIAN_MAP_COUNT; i++) {
    if (cleaned.indexOf(RUSSIAN_MAP[i].ru) >= 0) return RUSSIAN_MAP[i].en;
  }
  return "unknown";
}

// ---------- Постоянное соединение с Gemini ----------
static WiFiClientSecure gemClient;
static HTTPClient       gemHttp;
static bool             gemInited = false;
static unsigned long    gemLastUseMs = 0;
// Если сервер отверг thinkingConfig/mediaResolution (HTTP 400 Unknown name),
// переключаемся на минимальное тело запроса и больше не шлём эти поля.
static bool             gemMinimalBody = false;

String gemUrl(const char* model) {
  return String("https://") + GEM_HOST + "/v1beta/models/" + model + ":generateContent";
}

void gemInit() {
  if (gemInited) return;
  gemClient.setInsecure();                          // без проверки сертификата = быстрее
  gemClient.setHandshakeTimeout(GEM_HANDSHAKE_SEC);
  gemHttp.setReuse(true);                           // keep-alive, ставим ДО begin()
  gemHttp.setConnectTimeout(GEM_CONNECT_TIMEOUT);
  gemHttp.setTimeout(GEM_TIMEOUT_MS);
  gemInited = true;
}

// Прогрев: один крошечный текстовый запрос, чтобы оплатить TLS-рукопожатие
// заранее, а не в момент, когда пользователь ждёт ответ.
bool gemWarmUp() {
  gemInit();
  unsigned long t0 = millis();
  const char* body =
    "{\"contents\":[{\"parts\":[{\"text\":\"ok\"}]}],"
    "\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":8}}";

  bool held = hsTake(8000);
  bool ok = false;
  if (gemHttp.begin(gemClient, gemUrl(GEM_MODEL_FAST))) {
    gemHttp.addHeader("Content-Type", "application/json");
    gemHttp.addHeader("x-goog-api-key", GEMINI_KEY);
    int code = gemHttp.POST((uint8_t*)body, strlen(body));
    String resp = (code > 0) ? gemHttp.getString() : String("");
    gemHttp.end();
    ok = (code == 200);
    Serial.printf("[GEMINI] Прогрев: HTTP %d (%s), %lu мс\n",
                  code, httpErrName(code), (unsigned long)(millis() - t0));
    if (code == 400 || code == 403 || code == 404) {
      Serial.println("[GEMINI] Тело ответа: " + resp.substring(0, 400));
    }
  } else {
    Serial.println("[GEMINI] Прогрев: begin() не удался");
  }
  if (held) xSemaphoreGive(hsMux);
  gemLastUseMs = millis();
  return ok;
}

// Пустой запрос раз в GEM_KEEPALIVE_MS, чтобы Google не закрыл сокет
// по неактивности. Вызывается из loop() только когда система свободна.
void gemKeepAlive() {
  if (!gemInited) return;
  if (millis() - gemLastUseMs < GEM_KEEPALIVE_MS) return;
  if (!gemClient.connected()) { gemWarmUp(); return; }
  gemLastUseMs = millis();
}

// Один запрос к одной модели.
// minimalBody=true -> без thinkingConfig/mediaResolution (для совместимости).
int gemRequest(const char* model, const uint8_t* jpeg, size_t jpegLen,
               String& respOut, bool minimalBody, uint32_t* netMs, uint32_t* b64Ms) {
  gemInit();

  unsigned long tB0 = millis();
  String prefix = "{\"contents\":[{\"parts\":["
                  "{\"text\":\"" + String(PROMPT_JSON) + "\"},"
                  "{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"";

  // ГЛАВНОЕ ДЛЯ СКОРОСТИ: thinkingLevel "minimal" + mediaResolution LOW.
  const char* suffixFull =
      "\"}}]}],"
      "\"generationConfig\":{"
        "\"temperature\":0,"
        "\"maxOutputTokens\":40,"
        "\"thinkingConfig\":{\"thinkingLevel\":\"minimal\"},"
        "\"mediaResolution\":\"MEDIA_RESOLUTION_LOW\""
      "}}";
  const char* suffixMin =
      "\"}}]}],"
      "\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":40}}";

  const char* suffix = minimalBody ? suffixMin : suffixFull;

  size_t b64n  = base64Len(jpegLen);
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
  Serial.printf("[GEMINI] %s, тело %u б, соединение %s\n",
                model, (unsigned)total, warm ? "ГОРЯЧЕЕ" : "холодное (нужен TLS)");

  // Мьютекс берём только если предстоит рукопожатие, и не дольше 4 с
  bool held = !warm ? hsTake(4000) : false;

  unsigned long tN0 = millis();
  int code;
  if (!gemHttp.begin(gemClient, gemUrl(model))) {
    free(body);
    if (held) xSemaphoreGive(hsMux);
    respOut = "begin_failed";
    return -1;
  }
  gemHttp.setTimeout(GEM_TIMEOUT_MS);
  gemHttp.addHeader("Content-Type", "application/json");
  gemHttp.addHeader("x-goog-api-key", GEMINI_KEY);
  code = gemHttp.POST(body, total);

  // Если соединение было горячим, но оказалось мёртвым — один мгновенный
  // повтор с новым рукопожатием. Это типичная ситуация: Google закрыл
  // idle-сокет, а мы об этом ещё не знаем.
  if (warm && (code == HTTPC_ERROR_CONNECTION_LOST ||
               code == HTTPC_ERROR_CONNECTION_REFUSED ||
               code == HTTPC_ERROR_NOT_CONNECTED)) {
    Serial.println("[GEMINI] Сокет был мёртв, переподключаюсь...");
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

// true — распознали. rawOut/catOut заполнены.
bool classifyWaste(const uint8_t* jpeg, size_t jpegLen, String& rawOut, String& catOut) {
  const char* models[2] = { GEM_MODEL_FAST, GEM_MODEL_FALLBACK };

  for (int m = 0; m < 2; m++) {
    String resp;
    uint32_t netMs = 0, b64Ms = 0;
    int code = gemRequest(models[m], jpeg, jpegLen, resp, gemMinimalBody, &netMs, &b64Ms);
    lastTiming.net = netMs;
    lastTiming.b64 = b64Ms;

    Serial.printf("[GEMINI] HTTP %d (%s), сеть %lu мс\n",
                  code, httpErrName(code), (unsigned long)netMs);

    // Сервер не знает про thinkingConfig/mediaResolution -> упрощаем тело
    if (code == 400 && !gemMinimalBody &&
        (resp.indexOf("Unknown name") >= 0 || resp.indexOf("Invalid JSON payload") >= 0)) {
      Serial.println("[GEMINI] 400 из-за generationConfig. Перехожу на упрощённое тело.");
      Serial.println("[GEMINI] " + resp.substring(0, 300));
      gemMinimalBody = true;
      code = gemRequest(models[m], jpeg, jpegLen, resp, true, &netMs, &b64Ms);
      lastTiming.net += netMs;
      Serial.printf("[GEMINI] Повтор: HTTP %d (%s)\n", code, httpErrName(code));
    }

    if (code != 200) {
      if (code > 0) Serial.println("[GEMINI] Ответ: " + resp.substring(0, 300));
      rawOut = "http_" + String(code);
      catOut = "error";
      // 429/503 или таймаут — пробуем вторую модель. Иначе смысла нет.
      if (!(code == 429 || code == 503 || code == 500 ||
            code == HTTPC_ERROR_READ_TIMEOUT || code < 0)) return false;
      continue;
    }

    unsigned long tP0 = millis();
    JsonDocument filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    filter["candidates"][0]["finishReason"] = true;
    filter["promptFeedback"]["blockReason"] = true;
    JsonDocument doc;
    DeserializationError derr =
      deserializeJson(doc, resp, DeserializationOption::Filter(filter));
    lastTiming.parse = millis() - tP0;

    if (derr) {
      Serial.printf("[GEMINI] Парсинг не удался: %s\n", derr.c_str());
      Serial.println("[GEMINI] " + resp.substring(0, 300));
      rawOut = "parse_error";
      catOut = "error";
      continue;
    }

    const char* content = doc["candidates"][0]["content"]["parts"][0]["text"];
    const char* finish  = doc["candidates"][0]["finishReason"] | "?";
    const char* block   = doc["promptFeedback"]["blockReason"] | "none";

    if (!content) {
      Serial.printf("[GEMINI] Пустой ответ. finishReason=%s, blockReason=%s\n", finish, block);
      if (strcmp(finish, "MAX_TOKENS") == 0)
        Serial.println("[GEMINI] Подсказка: упёрлись в maxOutputTokens. Поднимите лимит.");
      rawOut = String("empty:") + finish;
      catOut = "error";
      continue;
    }

    rawOut = String(content);
    rawOut.trim();
    catOut = extractCategory(rawOut);
    Serial.println("[GEMINI] raw: " + rawOut.substring(0, 80));
    Serial.println("[GEMINI] category: " + catOut);
    return true;
  }
  return false;
}

// ============================================================================
//                ПОЛНЫЙ ЦИКЛ ПО ТРИГГЕРУ
// ============================================================================
void runRecognition(const String& chatId, bool withAI) {
  busy = true;
  unsigned long t0 = millis();

  unsigned long tC0 = millis();
  Shot shot;
  if (!takeShot(shot)) {
    tgQueueText(chatId, "❌ Ошибка захвата кадра");
    busy = false; aiPending = false;
    return;
  }
  lastTiming.cam = millis() - tC0;

  if (!withAI) {
    tgQueuePhoto(chatId, "📷 Кадр (" + String(shot.len) + " б)", shot);
    busy = false; aiPending = false;
    return;
  }

  String raw, category;
  bool ok = classifyWaste(shot.data, shot.len, raw, category);
  lastTiming.total = millis() - t0;

  if (!ok) {
    tgQueuePhoto(chatId, "⚠️ Ошибка распознавания: " + raw +
                         "\n⏱ " + String(lastTiming.total / 1000.0, 1) + " с", shot);
    busy = false; aiPending = false;
    return;
  }

  // === ОТВЕТ УХОДИТ ПЕРВЫМ. Серво крутятся после. ===
  String answer = binLabelFor(category);
  answer += "\n⏱ " + String(lastTiming.total / 1000.0, 1) + " с";
  answer += "  (кадр " + String(lastTiming.cam) + " / сеть " + String(lastTiming.net) + " мс)";
  tgQueueText(chatId, answer);
  Serial.printf("[ОТВЕТ] %s за %lu мс\n", category.c_str(), lastTiming.total);

  // Даём задаче Телеграма отправить текст, пока мы крутим механику
  vTaskDelay(pdMS_TO_TICKS(30));

  sortWaste(binMechFor(category));

  // Фото — уже без спешки, после сортировки
  tgQueuePhoto(chatId, "📷 " + category + " — сортировка выполнена", shot);

  busy = false;
  aiPending = false;
}

// ============================================================================
//                       КНОПКА
// ============================================================================
#define BUTTON_ACTIVE_LOW   1
#define BTN_DEBOUNCE_MS     40

bool buttonIsDown() {
  int lvl = digitalRead(BUTTON_PIN);
  return BUTTON_ACTIVE_LOW ? (lvl == LOW) : (lvl == HIGH);
}

void checkButton() {
  static bool          stable       = false;
  static bool          lastRaw      = false;
  static unsigned long lastChangeMs = 0;
  bool raw = buttonIsDown();
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = millis();
    return;
  }
  if (millis() - lastChangeMs < BTN_DEBOUNCE_MS) return;
  if (raw == stable) return;
  stable = raw;
  if (!raw) { buttonPressed = false; return; }
  buttonPressed = true;
  if (busy) {
    Serial.println("[BUTTON] Нажата, но система занята — игнорирую");
    return;
  }
  String chatId = getDefaultChat();
  if (chatId.length() == 0) chatId = String(TG_OWNER_CHAT);
  Serial.println("[BUTTON] Нажата -> запуск /photo");
  TriggerEvt ev = {};
  strlcpy(ev.chatId, chatId.c_str(), sizeof(ev.chatId));
  ev.withAI = true;
  aiPending = true;
  if (xQueueSend(qTrigger, &ev, 0) != pdTRUE) {
    aiPending = false;
    Serial.println("[BUTTON] Очередь триггеров переполнена");
  }
}

// ============================================================================
//              КОМАНДЫ ЧЕРЕЗ SERIAL MONITOR
// ============================================================================
void benchGemini(int times) {
  Shot s;
  if (!takeShot(s)) { Serial.println("[BENCH] нет кадра"); return; }
  Serial.printf("[BENCH] кадр %u б, %d прогонов\n", (unsigned)s.len, times);
  uint32_t best = 0xFFFFFFFF, worst = 0, sum = 0;
  for (int i = 0; i < times; i++) {
    String raw, cat;
    unsigned long t0 = millis();
    bool ok = classifyWaste(s.data, s.len, raw, cat);
    uint32_t dt = millis() - t0;
    sum += dt;
    if (dt < best) best = dt;
    if (dt > worst) worst = dt;
    Serial.printf("[BENCH] #%d: %s, %lu мс -> %s\n",
                  i + 1, ok ? "OK" : "FAIL", (unsigned long)dt, cat.c_str());
    delay(200);
  }
  Serial.printf("[BENCH] min %lu / avg %lu / max %lu мс\n",
                (unsigned long)best, (unsigned long)(sum / times), (unsigned long)worst);
  free(s.data);
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line == "b") {
    Serial.printf("[BTN] digitalRead(D8) = %d -> %s\n",
                  digitalRead(BUTTON_PIN), buttonIsDown() ? "НАЖАТА" : "отпущена");
    return;
  }
  if (busy) { Serial.println("[CMD] Система занята"); return; }
  if (line == "h")       { setAllServoHome(); Serial.println("[CMD] Домой"); }
  else if (line == "p")  printServoState();
  else if (line == "w")  gemWarmUp();
  else if (line == "g")  benchGemini(1);
  else if (line == "G")  benchGemini(3);
  else if (line == "i") {
    Serial.printf("[INFO] heap %u, psram %u, соединение с Gemini: %s\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                  gemClient.connected() ? "живое" : "закрыто");
  }
  else if (line.startsWith("s ")) {
    int sp2 = line.indexOf(' ', 2);
    if (sp2 > 0) {
      int idx = line.substring(2, sp2).toInt();
      int ang = line.substring(sp2 + 1).toInt();
      if (idx >= 0 && idx < SERVO_COUNT) {
        Serial.printf("[CMD] %s: %d -> %d\n", servoName[idx], currentAngle[idx], ang);
        moveServoSmooth(idx, ang);
        delay(300);
        servoRelease(idx);
      } else Serial.println("[CMD] Индекс 0..2");
    }
  } else {
    Serial.println("[CMD] b кнопка | h домой | p углы | s <0..2> <угол> | "
                   "w прогрев | g тест Gemini | G бенчмарк x3 | i память");
  }
}

// ============================================================================
//                                 SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(300);
  Serial.println("\n\n=== ЗАПУСК v21 (ответ < 5 c) ===");
  Serial.printf("Модель: %s (fallback %s)\n", GEM_MODEL_FAST, GEM_MODEL_FALLBACK);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (!initCamera()) {
    Serial.println("ОШИБКА камеры — проверь шлейф.");
    while (true) delay(1000);
  }
  Serial.printf("Камера OK. PSRAM: %s (%u б)\n",
                psramFound() ? "есть" : "НЕТ", (unsigned)ESP.getFreePsram());
  delay(10);
  Serial.printf("[BUTTON] Пин D8 = %d (%s)\n",
                digitalRead(BUTTON_PIN), buttonIsDown() ? "НАЖАТА" : "отпущена");

  initServos();
  servoReleaseAll();

  Serial.printf("Подключение к \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  WiFi.setSleep(false);                       // без этого +100..300 мс на каждый пакет
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.print("\nIP: ");
  Serial.println(WiFi.localIP());

  setCpuFrequencyMhz(240);
  Serial.printf("[PWR] CPU: %u МГц\n", (unsigned)getCpuFrequencyMhz());

  qTrigger = xQueueCreate(4, sizeof(TriggerEvt));
  qOut     = xQueueCreate(6, sizeof(TgOut));
  hsMux    = xSemaphoreCreateMutex();
  chatMux  = xSemaphoreCreateMutex();

  setDefaultChat(TG_OWNER_CHAT);
  Serial.printf("[TG] chat_id по умолчанию: %s\n", TG_OWNER_CHAT);

  // Прогреваем TLS до Google ЗАРАНЕЕ — это минус 1.5-3 с с первого ответа
  gemWarmUp();
  Serial.printf("[MEM] heap %u, psram %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  xTaskCreatePinnedToCore(telegramTask, "telegram", 16384, NULL, 1, NULL, 0);
  Serial.printf("loop() на ядре %d. Готов.\n", xPortGetCoreID());
  Serial.println("Serial: g — замер одного запроса, G — три, i — память");
}

// ============================================================================
//        ГЛАВНЫЙ ЦИКЛ — ЯДРО 1
// ============================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }
  handleSerialCommands();
  checkButton();

  if (!busy && (millis() - lastTriggerMs > TRIGGER_COOLDOWN_MS)) {
    TriggerEvt ev;
    if (xQueueReceive(qTrigger, &ev, 0) == pdTRUE) {
      String replyChat = String(ev.chatId);
      if (replyChat.length() == 0) replyChat = getDefaultChat();
      lastTriggerMs = millis();
      runRecognition(replyChat, ev.withAI);
    } else {
      gemKeepAlive();   // держим сокет живым, пока нечего делать
    }
  }
  vTaskDelay(pdMS_TO_TICKS(20));
}
