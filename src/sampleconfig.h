#define WIFI_SSID           "1133"
#define WIFI_PASSWORD       "1123"

// ── Telegram ─────────────────────────────────────────────────
#define BOT_TOKEN           "1123"


// Authorised chat IDs — add as many as needed
const String AUTHORISED_CHAT_IDS[] = {
  "60",   // Your name — chat ID from getUpdates
  "72",
  "87"
  // "XXXXXXXXX",   // Other user — add more as needed
};

const int AUTHORISED_CHAT_ID_COUNT = sizeof(AUTHORISED_CHAT_IDS) / sizeof(AUTHORISED_CHAT_IDS[0]);

// Admin chat ID — receives error alerts and boot reports
// Can be the same as your personal chat ID above
#define ADMIN_CHAT_ID       "1"

// Authorised open code
#define AUTHORISED_CODE     "1"

// ── Pin assignments ───────────────────────────────────────────
#define LED_GREEN           1    // Device power on
#define LED_RED             1    // Error / fault
#define RELAY_PIN           2    // Active HIGH — closes relay

// IO15 reserved — unconnected (Phase 1.8 test button)

// ── Timing (ms) ───────────────────────────────────────────────
#define WIFI_TIMEOUT_MS       20000   // Max wait for WiFi connect
#define RECONNECT_MS           5000   // Delay between reconnect attempts
#define HEARTBEAT_MS           5000   // Status bar refresh interval
#define FLASH_MS                500   // red LED flash period
#define POLL_INTERVAL_MS      15000   // Telegram polling interval
#define INVERT_FLASH_MS        3000   // Display invert duration
#define ERROR_PAUSE_MS         2000   // Pause after error display
#define DONE_SCREEN_MS         3000   // Pause on done screen
#define RESTORED_PAUSE_MS      2000   // Pause on WiFi restored screen
#define RELAY_ACTIVE_MS         500   // Relay hold duration

// ── Fault / failover ──────────────────────────────────────────
// How many consecutive failed WiFi reconnects before ESP.restart()
#define MAX_RECONNECT_ATTEMPTS    1

// Max restarts before Genie stops trying and enters fault sleep
#define MAX_RESTARTS          1

// How long to sleep between restart attempts once cap is reached (ms)
#define FAULT_SLEEP_MS    300000  // 5 minutes

// Watchdog timeout in seconds — resets device if loop() stalls
#define WDT_TIMEOUT_S            30

// ── Active hours (local time) ─────────────────────────────────
// Genie is active between ACTIVE_HOUR_START and ACTIVE_HOUR_END.
// If start > end the window wraps midnight (e.g. 18→8 = 6pm–8am).
#define ACTIVE_HOUR_START        19  // 7 pm
#define ACTIVE_HOUR_END           9   // 9 am
#define TIMEZONE_OFFSET_HOURS     4   // = UTC+4
#define NTP_SERVER               "pool.ntp.org"

// Sleep check interval when outside active hours (ms)
#define SLEEP_POLL_MS         300000  // 5 minutes

// ── Test mode ────────────────────────────────────────────────
// Test mode is triggered by Telegram command "start test"
// and bypasses active-hours gating.
// Set true to force test mode on at boot (dev use only).
#define TEST_MODE_BOOT        false

// ── Relay ─────────────────────────────────────────────────────
#define TOTAL_ACTIVATIONS         1   // Relay presses per command