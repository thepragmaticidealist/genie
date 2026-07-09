

#define VERSION_MAJOR 2
#define VERSION_MINOR 0
#define VERSION_PATCH 0
#define VERSION "2.0.0"

// ── Includes ──────────────────────────────────────────────────
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "time.h"
#include "config.h"

// ── Display ───────────────────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   32
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── Telegram ──────────────────────────────────────────────────
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ── State flags ───────────────────────────────────────────────
bool telegramReady        = false;
bool relayRunning         = false;
bool wifiWasLost          = false;
bool testMode             = TEST_MODE_BOOT;
bool timesynced           = false;
bool wasSleeping          = false;

// ── Timers ────────────────────────────────────────────────────
unsigned long lastReconnectAttempt = 0;
unsigned long lastHeartbeat        = 0;
unsigned long lastPollTime         = 0;

// ── WiFi fault counter ────────────────────────────────────────
int reconnectAttempts = 0;

// ── Pending alert ─────────────────────────────────────────────
String pendingAlert = "";

// ── LED flash state ───────────────────────────────────────────
unsigned long lastRedFlash  = 0;
bool          redFlashing   = false;
bool          redFlashState = false;

// ── Error source identifiers ──────────────────────────────────
#define ERROR_DISPLAY  "DISPLAY"
#define ERROR_WIFI     "WIFI"
#define ERROR_SETUP    "SETUP"
#define ERROR_TELEGRAM "TELEGRAM"

// ═══════════════════════════════════════════════════════════════
//  WATCHDOG
// ═══════════════════════════════════════════════════════════════

void initWatchdog() {
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog armed (" + String(WDT_TIMEOUT_S) + "s).");
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

// ═══════════════════════════════════════════════════════════════
//  BOOT REASON
// ═══════════════════════════════════════════════════════════════

String getResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software restart (ESP.restart())";
    case ESP_RST_PANIC:     return "Kernel panic / exception";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog timeout";
    case ESP_RST_TASK_WDT:  return "Task watchdog timeout";
    case ESP_RST_WDT:       return "Other watchdog reset";
    case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout (low power)";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "Unknown";
  }
}

bool isAbnormalReset() {
  esp_reset_reason_t r = esp_reset_reason();
  return (r == ESP_RST_PANIC    ||
          r == ESP_RST_INT_WDT  ||
          r == ESP_RST_TASK_WDT ||
          r == ESP_RST_WDT      ||
          r == ESP_RST_BROWNOUT ||
          r == ESP_RST_SW);
}

// ═══════════════════════════════════════════════════════════════
//  ALERT SYSTEM
// ═══════════════════════════════════════════════════════════════

void sendAlert(String message) {
  Serial.println("[ALERT] " + message);
  if (WiFi.status() == WL_CONNECTED && telegramReady) {
    bot.sendMessage(ADMIN_CHAT_ID, "🔴 *Genie Alert*\n" + message, "Markdown");
    if (pendingAlert.length() > 0) {
      bot.sendMessage(ADMIN_CHAT_ID,
        "📋 *Queued alert (sent when WiFi restored)*\n" + pendingAlert, "Markdown");
      pendingAlert = "";
    }
  } else {
    pendingAlert += (pendingAlert.length() == 0) ? message : "\n" + message;
    Serial.println("[ALERT] Queued (no connectivity).");
  }
}

void sendBootReport() {
  String reason = getResetReason();
  String msg = isAbnormalReset()
    ? "⚠️ *Genie restarted* — v" + String(VERSION) + "\nReset reason: " + reason + "\nThis was an unplanned restart."
    : "🟢 *Genie online* — v"    + String(VERSION) + "\nReset reason: " + reason;
  bot.sendMessage(ADMIN_CHAT_ID, msg, "Markdown");
  Serial.println("Boot report sent: " + reason);
}

// ═══════════════════════════════════════════════════════════════
//  TIME
// ═══════════════════════════════════════════════════════════════

void syncTime() {
  configTime(TIMEZONE_OFFSET_HOURS * 3600, 0, NTP_SERVER);
  Serial.print("Syncing time...");
  unsigned long start = millis();
  struct tm ti;
  while (!getLocalTime(&ti) && millis() - start < 10000) {
    feedWatchdog(); delay(500); Serial.print(".");
  }
  if (getLocalTime(&ti)) {
    timesynced = true;
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    Serial.println("\nTime synced: " + String(buf));
  } else {
    Serial.println("\nTime sync failed — active hours disabled.");
    sendAlert("⚠️ NTP sync failed. Active hours gating disabled until next reboot.");
  }
}

// ────────────────────────────────────────────────────────────
// isWithinActiveHours()
//   wrapsMidnight = true  → active when h >= START or h < END
//                           e.g. 18:00 → 08:00 (Phase 1 default)
//   wrapsMidnight = false → active when h >= START and h < END 
//                           same-day window, reserved for future use
//   Always returns true if testMode, NTP unsynced, or getLocalTime fails.
// ────────────────────────────────────────────────────────────
bool isWithinActiveHours(bool wrapsMidnight = true) {
  if (testMode)    return true;
  if (!timesynced) return true;
  struct tm ti;
  if (!getLocalTime(&ti)) return true;
  int h = ti.tm_hour;
  bool isWithin = wrapsMidnight
    ? (h >= ACTIVE_HOUR_START || h < ACTIVE_HOUR_END)
    : (h >= ACTIVE_HOUR_START && h < ACTIVE_HOUR_END);

    // Serial.print("Time is within active hrs");
    // Serial.print(isWithin);

    return isWithin;
}

String getCurrentTimeString() {
  if (!timesynced) return "??:??";
  struct tm ti;
  if (!getLocalTime(&ti)) return "??:??";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &ti);
  return String(buf);
}

// ═══════════════════════════════════════════════════════════════
//  LED HELPERS
// ═══════════════════════════════════════════════════════════════

void initLEDs() {
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   LOW);
}

void setLED(int pin, bool state) {
  if (pin == LED_RED) redFlashing = false;
  digitalWrite(pin, state ? HIGH : LOW);
}

void flashLED(int pin) {
  if (pin == LED_RED) redFlashing = true;
}

void updateLEDs() {
  unsigned long now = millis();
  if (redFlashing && now - lastRedFlash >= FLASH_MS) {
    lastRedFlash  = now;
    redFlashState = !redFlashState;
    digitalWrite(LED_RED, redFlashState ? HIGH : LOW);
  }
}

void setErrorLED(bool state) {
  if (state) flashLED(LED_RED);
  else  setLED(LED_RED, false);
}

// ── Blocking pause — feeds watchdog and updates LEDs ──────────
void blockingPause(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) { feedWatchdog(); updateLEDs(); delay(10); }
}

// ═══════════════════════════════════════════════════════════════
//  RELAY HELPERS
// ═══════════════════════════════════════════════════════════════

void initRelay() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay initialised.");
}

void activateRelay() {
  Serial.println("Relay closing...");
  digitalWrite(RELAY_PIN, HIGH);
  blockingPause(RELAY_ACTIVE_MS);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay opened.");
}

// ═══════════════════════════════════════════════════════════════
//  DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────
// showScreen()
//   Single entry point for all OLED writes.
//   Draws the inverted status bar on row 0, then up to two
//   body lines at rows 16 and 24.
//   Pass "" for line2 if only one body line is needed.
// ────────────────────────────────────────────────────────────
void showScreen(String statusState, String line1, String line2 = "") {
  display.clearDisplay();

  // Status bar — row 0, inverted
  String bar = statusState;
  if (WiFi.status() == WL_CONNECTED) bar += " W+"; else bar += " W-";
  bar += telegramReady ? " TG+" : " TG-";
  if (testMode) bar += " TST";
  while (bar.length() < 21) bar += " ";
  if (bar.length() > 21)    bar  = bar.substring(0, 21);

  // TODO: Add countdown to telegram polling

  display.setCursor(0, 0);
  display.setTextColor(BLACK, WHITE);
  display.print(bar);
  display.setTextColor(WHITE, BLACK);  // restore: white text on black background

  // Body lines
  if (line1.length() > 0) {
    display.setCursor(0, 16);
    display.print(line1);
  }
  if (line2.length() > 0) {
    display.setCursor(0, 24);
    display.print(line2);
  }

  display.display();
}

void flashInvert() {
  display.invertDisplay(true);
  delay(INVERT_FLASH_MS);
  display.invertDisplay(false);
}

// ── Named screen functions — thin wrappers around showScreen() ─

void showIdle() {
  showScreen("RLY:--", "Genie is ready", getCurrentTimeString() + " EAT");
}

void showNoMessages() {
  showScreen("RLY:--", "No messages", "yet");
}

void showSleeping() {
  showScreen("ZZZ", "Sleeping",
    "Now " + getCurrentTimeString()+ "  back " + String(ACTIVE_HOUR_START) + ":00");
}

void showPolling() {
  showScreen("RLY:--", "Checking messages", "Please wait...");
}

void showCMDReceived(String sender) {
  String from = "From: " + sender;
  if (from.length() > 21) from = from.substring(0, 21);
  showScreen("RLY:--", "Command received", from);
}

void showBadCommand() {
  showScreen("RLY:--", "! Bad command", "Send: open <code>");
}

void showActivating() {
  showScreen("RLY:ON", "Activating...", "Door opening");
}

void showDone() {
  showScreen("RLY:OK", "Done", "Door opened");
}

void showWiFiLost() {
  showScreen("RLY:--", "! No WiFi", "Reconnecting...");
}

void showWiFiRestored() {
  showScreen("RLY:--", "WiFi restored", "Genie is ready");
}

void showTelegramOffline() {
  showScreen("RLY:--", "! Telegram offline", "Messages paused");
}

void showTestMode(String testLine) {
  showScreen("TEST", "TEST MODE ON", testLine);
}

void showRestarting() {
  showScreen("RESTART", "Restarting...", "");
}

// ═══════════════════════════════════════════════════════════════
//  DISPLAY INIT
// ═══════════════════════════════════════════════════════════════

void initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[ERROR][DISPLAY] E01: SSD1306 not found");
    while (true) { updateLEDs(); delay(10); }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.display();
  Serial.println("Display initialised.");
}

// ═══════════════════════════════════════════════════════════════
//  ERROR LOGGING
// ═══════════════════════════════════════════════════════════════

void logError(const char* source, const char* code, const char* message) {
  String msg = String("[") + source + "] " + code + ": " + message;
  Serial.println("[ERROR] " + msg);
  setErrorLED(true);
  sendAlert(msg);
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════

void printWiFiStatus() {
  Serial.print("  IP      : "); Serial.println(WiFi.localIP());
  Serial.print("  RSSI    : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  Serial.print("  Channel : "); Serial.println(WiFi.channel());
  Serial.print("  MAC     : "); Serial.println(WiFi.macAddress());
}

bool connectWiFi() {
  Serial.print("Connecting to "); Serial.print(WIFI_SSID); Serial.print("...");
  telegramReady = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startAttempt >= WIFI_TIMEOUT_MS) {
      Serial.println("\nWiFi timeout.");
      if (!wasSleeping) showWiFiLost();
      return false;
    }
    feedWatchdog(); updateLEDs(); delay(500); Serial.print(".");
  }
  Serial.println("\nConnected.");
  printWiFiStatus();
  setErrorLED(false);
  telegramReady = true;
  reconnectAttempts = 0;
  return true;
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiWasLost) {
      wifiWasLost   = true;
      telegramReady = false;
      setErrorLED(true);
      Serial.println("WiFi lost.");
      sendAlert("📡 WiFi lost. Attempting reconnect.");
      if (!wasSleeping) showWiFiLost();
    }
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_MS) {
      lastReconnectAttempt = now;
      reconnectAttempts++;
      Serial.print("Reconnect attempt ");
      Serial.print(reconnectAttempts);
      Serial.print(" / ");
      Serial.println(MAX_RECONNECT_ATTEMPTS);
      if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        Serial.println("Max reconnect attempts reached. Restarting...");
        pendingAlert += "\n⚠️ WiFi reconnect failed after " +
                        String(MAX_RECONNECT_ATTEMPTS) + " attempts. Restarting.";
        showRestarting();
        blockingPause(1000);
        ESP.restart();

        // TODO: Sleep if max restart attempts reached
      }
      connectWiFi();
    }
  } else if (wifiWasLost) {
    wifiWasLost       = false;
    telegramReady     = true;
    reconnectAttempts = 0;
    setErrorLED(false);
    Serial.println("WiFi restored.");
    sendAlert("✅ WiFi restored.");
    if (!wasSleeping) {
      showWiFiRestored();
      blockingPause(RESTORED_PAUSE_MS);
      showIdle();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  TEST MODE
// ═══════════════════════════════════════════════════════════════

void runSelfTest(String chatId) {
  bool savedTestMode = testMode;
  testMode = true;
  Serial.println("=== SELF TEST START ===");

  String report = "🧪 *Genie Self-Test Report* — v" + String(VERSION) + "\n";
  report += "Time: " + getCurrentTimeString() + " EAT\n\n";

  // ── LEDs ──
  int    ledPins[] = {LED_GREEN, LED_RED};
  String ledNames[]= {"Green", "Red"};
  report += "*LEDs*\n";
  for (int i = 0; i < 2; i++) {
    showTestMode(String("LED: ") + ledNames[i]);
    digitalWrite(ledPins[i], HIGH); blockingPause(500);
    digitalWrite(ledPins[i], LOW);  blockingPause(200);
    report += "  " + ledNames[i] + ": ✅ OK\n";
    Serial.println("LED " + ledNames[i] + ": OK");
  }
  // Restore green
  digitalWrite(LED_GREEN, HIGH);

  // ── Relay ──
  showTestMode("Relay...");
  report += "\n*Relay*\n";
  digitalWrite(RELAY_PIN, HIGH);
  blockingPause(RELAY_ACTIVE_MS);
  digitalWrite(RELAY_PIN, LOW);
  report += "  Relay: ✅ Fired OK\n";
  Serial.println("Relay: OK");

  // ── WiFi ──
  showTestMode("WiFi...");
  report += "\n*WiFi*\n";
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  // report += wifiOk
  //   ? "  Status: ✅ Connected\n  SSID: " + String(WIFI_SSID) +
  //     "\n  RSSI: " + String(WiFi.RSSI()) + " dBm\n  IP: " +
  //     WiFi.localIP().toString() + "\n"
  //   : "  Status: ❌ Not connected\n";

  report += wifiOk
    ? "  Status: ✅ Connected\n  SSID: " + String(WIFI_SSID) +
      "\n  RSSI: " + String(WiFi.RSSI()) + " dBm\n  IP: " + "\n"
    : "  Status: ❌ Not connected\n";

  Serial.println("WiFi: " + String(wifiOk ? "OK" : "FAIL"));
  feedWatchdog();

  // ── Telegram ──
  showTestMode("Telegram...");
  report += "\n*Telegram*\n";
  bool tgOk = false;
  if (wifiOk) {
    tgOk = bot.sendMessage(ADMIN_CHAT_ID, "🧪 Telegram test ping.", "");
    report += tgOk ? "  Status: ✅ Online\n" : "  Status: ❌ Unreachable\n";
  } else {
    report += "  Status: ⏭ Skipped (no WiFi)\n";
  }
  Serial.println("Telegram: " + String(tgOk ? "OK" : "FAIL"));
  feedWatchdog();

  // ── Active hours ──
  report += "\n*Active Hours*\n";
  report += "  Window: " + String(ACTIVE_HOUR_START) + ":00 – " +
            String(ACTIVE_HOUR_END) + ":00 EAT\n";
  report += "  Now: " + getCurrentTimeString() + "\n";
  report += isWithinActiveHours(true)
    ? "  Status: ✅ Within window\n"
    : "  Status: 💤 Outside window\n";

  // ── Send report ──
  bot.sendMessage(chatId, report, "Markdown");
  if (chatId != String(ADMIN_CHAT_ID)) {
    bot.sendMessage(ADMIN_CHAT_ID, report, "Markdown");
  }
  Serial.println("=== SELF TEST COMPLETE ===");

  testMode = savedTestMode;

  if (isWithinActiveHours(true)) showIdle();
  else                           showSleeping();
}

// ═══════════════════════════════════════════════════════════════
//  COMMAND PARSING
// ═══════════════════════════════════════════════════════════════

bool parseCommand(String text, String chatId) {
  int spaceIndex = text.indexOf(' ');
  if (spaceIndex == -1) {
    bot.sendMessage(chatId, "Bad command. Send: open <code>", "");
    showBadCommand(); setErrorLED(true); blockingPause(ERROR_PAUSE_MS); setErrorLED(false);
    return false;
  }
  String keyword = text.substring(0, spaceIndex);
  keyword.toLowerCase(); keyword.trim();
  String code = text.substring(spaceIndex + 1); code.trim();

  Serial.print("Keyword: "); Serial.println(keyword);
  Serial.print("Code: ");    Serial.println(code);

  if (keyword != "open") {
    bot.sendMessage(chatId, "Bad command. Send: open <code>", "");
    showBadCommand(); setErrorLED(true); blockingPause(ERROR_PAUSE_MS); setErrorLED(false);
    return false;
  }
  if (code != String(AUTHORISED_CODE)) {
    bot.sendMessage(chatId, "Wrong code. Please try again.", "");
    showBadCommand(); setErrorLED(true); blockingPause(ERROR_PAUSE_MS); setErrorLED(false);
    return false;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════
//  TELEGRAM
// ═══════════════════════════════════════════════════════════════

bool isAuthorised(String chatId) {
  for (int i = 0; i < AUTHORISED_CHAT_ID_COUNT; i++) {
    if (AUTHORISED_CHAT_IDS[i] == chatId) return true;
  }
  return false;
}

void runActivation(String chatId) {
  relayRunning = true;
  for (int i = 1; i <= TOTAL_ACTIVATIONS; i++) {
    showActivating();
    activateRelay();
    if (i < TOTAL_ACTIVATIONS) { feedWatchdog(); delay(500); }
  }
  relayRunning = false;
  showDone();
  if (chatId.length() > 0) {
    bot.sendMessage(chatId, "Done. Please push the door to open.", "");
  }
  blockingPause(DONE_SCREEN_MS);
  showIdle();
}

void pollTelegram() {
  showPolling();
  feedWatchdog();

  int messageCount = bot.getUpdates(bot.last_message_received + 1);

  if (messageCount == 0) {
    showNoMessages();
    return;
  }

  for (int i = 0; i < messageCount; i++) {
    feedWatchdog();
    String chatId = String(bot.messages[i].chat_id);
    String text   = bot.messages[i].text;
    String sender = bot.messages[i].from_name;
    text.trim();

    Serial.print("From: ");    Serial.println(sender);
    Serial.print("Chat ID: "); Serial.println(chatId);
    Serial.print("Message: "); Serial.println(text);

    if (!isAuthorised(chatId)) {
      bot.sendMessage(chatId, "Unauthorised. This attempt has been logged.", "");
      sendAlert("🚫 Unauthorised access attempt from chat ID: " + chatId);
      setErrorLED(true); blockingPause(ERROR_PAUSE_MS); setErrorLED(false);
      showIdle();
      continue;
    }

    // ── Test mode trigger ──
    String textLower = text;
    textLower.toLowerCase(); textLower.trim();
    if (textLower == "start test") {
      bot.sendMessage(chatId, "🧪 Starting self-test...", "");
      runSelfTest(chatId);
      continue;
    }

    // ── Relay busy guard ──
    if (relayRunning) {
      bot.sendMessage(chatId, "Busy. Please wait.", "");
      showIdle();
      continue;
    }

    // ── Parse and action ──
    bool valid = parseCommand(text, chatId);
    if (valid) {
      flashInvert();
      showCMDReceived(sender);
      bot.sendMessage(chatId,
        "Your wish is my command " + sender + ". I shall open the door. 🧞", "");
      runActivation(chatId);
    } else {
      showIdle();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(9600);
  delay(500);

  initWatchdog();
  initLEDs();
  setLED(LED_GREEN, true);

  Serial.println("========================================");
  Serial.println("  Genie v" + String(VERSION));
  Serial.println("  Reset reason: " + getResetReason());
  Serial.println("========================================\n");

  initRelay();
  initDisplay();

  showScreen("BOOT", "Genie v" + String(VERSION), "Starting up...");

  if (!connectWiFi()) {
    logError(ERROR_SETUP, "E04", "WiFi init failed at boot");
    showWiFiLost();
    blockingPause(10000);
    ESP.restart();
  }

  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  telegramReady = true;

  syncTime();
  sendBootReport();

  if (pendingAlert.length() > 0) {
    bot.sendMessage(ADMIN_CHAT_ID,
      "📋 *Queued alert (pre-WiFi)*\n" + pendingAlert, "Markdown");
    pendingAlert = "";
  }

  Serial.println("\nAll systems ready. v" + String(VERSION));
  // Don't call showIdle() here — loop() will immediately
  // evaluate active hours and call the right screen.
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  feedWatchdog();
  updateLEDs();


  // ════════════════════════════════════════════════
  // SLEEP MODE — outside active hours
  // checkWiFi() does NOT run here. The only output
  // to the display is showSleeping().
  // ════════════════════════════════════════════════
  if (!isWithinActiveHours(true)) {
    if (!wasSleeping) {
      wasSleeping = true;
      Serial.println("Outside active hours — entering sleep.");
      setErrorLED(false);
    }
    showSleeping();
    
    unsigned long sleepStart = millis();
    while (millis() - sleepStart < SLEEP_POLL_MS) {
      feedWatchdog();
      delay(500);
    }
    return;
  }

  // ════════════════════════════════════════════════
  // ACTIVE MODE — only reached inside active hours
  // ════════════════════════════════════════════════

  // First tick after waking from sleep
  if (wasSleeping) {
    wasSleeping  = false;
    lastPollTime = 0;
    Serial.println("Active hours resumed — waking up.");
    sendAlert("🟢 Genie is awake. Active hours resumed at " +
              getCurrentTimeString() + " EAT.");
    showIdle();
  }

  // WiFi watchdog — runs only during active hours
  checkWiFi();

  unsigned long now = millis();

  if (now - lastPollTime >= POLL_INTERVAL_MS) {
    lastPollTime = now;
    if (WiFi.status() == WL_CONNECTED) pollTelegram();
  }

  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.println("Heartbeat: " + getCurrentTimeString() +
                   " | WiFi:" + String(WiFi.status() == WL_CONNECTED ? "+" : "-") +
                   " | TG:"   + String(telegramReady ? "+" : "-"));
  }
}
