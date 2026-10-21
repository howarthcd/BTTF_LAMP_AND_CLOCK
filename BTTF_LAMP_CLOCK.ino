// BUILD AND UPLOAD NOTES
//  Board manager URL (File > Preferences > Additional boards manager URLs):
//    https://espressif.github.io/arduino-esp32/package_esp32_index.json
//  Board package: "esp32" by Espressif Systems, version 3.x (Tools > Board > Boards Manager).
//  Board type:    Tools > Board > esp32 > ESP32 Dev Module (ESP32-WROOM-32).
//  Uploading:     Click Upload, and when the IDE shows "Connecting........" briefly touch the RST/EN pin
//                 to GND (blip reset). Once the upload percentage appears the board flashes by itself.
//                 If that fails, bridge GPIO0 to GND, blip RST, then remove the bridge after the upload
//                 starts and blip RST again to run the sketch. Use a data USB cable, and note that
//                 GPIO34 (logo brightness button) needs an external pull-down.
//
// BUTTONS
//  Two buttons: BRIGHTNESS (GPIO34) and COLOUR (GPIO25). Presses act when the button is released.
//    Tap BRIGHTNESS ........ Cycle the display brightness through 5 levels. The new level (1-5) is shown
//                            on the left display for 0.75s and saved to EEPROM.
//    Tap COLOUR ............ Cycle the logo colour scheme: 0 = off, 1 = red/yellow, 2 = blue/light blue,
//                            3 = red/blue. The scheme number is shown for 0.75s and saved to EEPROM.
//    Tap BOTH together ..... Show the firmware version on the left display for 1.5s (release in under 2s).
//    Hold BOTH for 2s ...... Restart the clock (displays and logo switch off, then it reboots).
//  Holding a single button does not repeat. Release it and press again to step further.
//
//  v2 - use TimeLib.h to correctly extract the date, seems to be a bug in NTPClient.h
//     - turn on the AM and PM LEDs at startup to indicate activity
//  v3 - implement numeric display brightness adjustment by button
//  v4 - optimised by ChatGPT
//     - included brightness setting of AM/PM indicators
//  v5 - added saving of display brightness to FLASH
//  v6 - add flashing colon between hour and min
//     - restrict display brightness range to 0-4 as 5-7 offers little perceivable additional brightness
//  v7 - use timer to update colon
//  v8 - only update the display if the colon is to be toggled on
//     - only update the display if the year is valid
//  v9 - switch LED driver library
//     - switch off the logo at startup and then on when running
//     - remove logo LED refresh from main loop as timer interrupt caused glitches during partial refreshes
// v10 - fade in the logo at switch-on
//     - switch off the numeric displays at switch-on
//     - added WiFi disconnect retry
// v11 - test to only get the time from the NTP server once at startup, then rely on the timer to increment epoch time
//       make more use of the TimeLib library functions
//     - added resilience around network connection at startup
//     - added delays to make the startup sequence more pleasing
// v12 - improve the first update to the time displays so that they don't visibly switch from 0 to the current time
// v13 - Implement regular NTP server syncs to ensure that the clock doesn't drift too far.
// v14 - Use EEPROM library instead of Preferences.
//     - Add additional WiFi settings to try to solve occasional connection failures.
// v15 - Made the date display format configurable - supports MMDD or DDMM.
//     - Optimised variable types.
//     - Allowed hard-coded WiFi credentials. If not specified then the config portal will launch.
//     - Support 12H and 24H time display.
// v16 - Use onboard RTC rather than the timer to keep track of time and apply DST in accordance with specified timezone.
//     - Remove time libraries as no longer needed.
//     - Connect to NTP server periodically to obtain current time.
//     - Prevent brief flash of time colon if off when display brightness button is pressed.
// v17 - Fix - logo off until ready to update time disaply for the first time.
//     - Added second button (on IO25) to change logo colour.
//     - Logo colour saved to EEPROM and retrieved at startup.
// v18 - Renumbered the logo colours. 0 = off, 1 = red/yellow etc.
//     - Implemented two button + hold reset.
// v20 - Removed the hardware timer/ISR. The displays are now only touched from loop(), driven by the RTC,
//       so the colon is locked to real seconds and the minute changes on time.
//     - Fix - config portal now launches when no WiFi credentials are saved.
//     - Fix - displays no longer stay dark if the clock boots at minute :00.
//     - Fix - midnight shows 12 rather than 00 in 12H mode.
//     - Fix - display brightness cycles through the full 0-4 range.
//     - NTP resync is non-blocking and waits for the sync to complete before disconnecting; retries after 60s on failure.
//     - At startup, waits for NTP to actually sync (the RTC survives a soft reset) and gives up waiting after a timeout.
//     - Buttons act on release, reset needs a 2s hold of both, and the colour button / brightness button no longer auto-repeat.
//     - Date and time written to the TM1637 displays in one transfer each.
//     - Settings display is non-blocking.
//     - A short tap of both buttons together shows the firmware version (FIRMWARE_VERSION) for 1.5s.
//

#include <WiFiManager.h>       // https://github.com/tzapu/WiFiManager
#include <TM1637Display.h>     // https://github.com/avishorp/TM1637
#include <ESP32_WS2812_Lib.h>  // https://github.com/Zhentao-Lin/ESP32_WS2812_Lib
#include <EEPROM.h>
#include <esp_sntp.h>

#define DISPLAY_CLK 16
#define DISPLAY1_DIO 17
#define DISPLAY2_DIO 18
#define DISPLAY3_DIO 19
#define AM_LED 32
#define PM_LED 33
#define BUTTON_LOGO_BRIGHTNESS 34  // Input-only pin with no internal pull resistor, needs an external pull-down.
#define BUTTON_LOGO_COLOURS 25
#define EEPROM_SIZE 2
#define CHANNEL 0
#define LEDS_PIN 5
#define LEDS_COUNT 18
#define LOGO_BRIGHTNESS_MAX 255
#define CLOCK_BRIGHTNESS_MAX 4  // 5-7 offers little perceivable additional brightness.
#define LOGO_SCHEME_COUNT 4
#define FIRMWARE_VERSION 20  // Shown by a short tap of both buttons together.

//========================USEFUL VARIABLES=============================
const char* ntpServer = "pool.ntp.org";
const char* tzString = "GMT0BST,M3.5.0/1,M10.5.0";  // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
int refreshTimeFromNTPIntervalSeconds = 1800;        // The minimum time between time syncs with the NTP server.
byte dateDisplayFormat = 2;                          // Date display format, 1 = MMDD, 2 = DDMM
bool clock24h = true;                                // If false then 12H, if true then 24H.
const char* ssid = "";                               // YOUR SSID HERE - leave empty to use WiFi management portal
const char* password = "";                           // WIFI PASSWORD HERE
//======================================================================

const unsigned long SETTING_DISPLAY_MS = 750;         // How long a changed setting is shown.
const unsigned long VERSION_DISPLAY_MS = 1500;        // How long the firmware version is shown.
const unsigned long BUTTON_DEBOUNCE_MS = 300;        // Minimum time between button actions.
const unsigned long RESET_HOLD_MS = 2000;             // How long both buttons must be held to restart.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;  // Resync: time allowed to join the WiFi network.
const unsigned long NTP_SYNC_TIMEOUT_MS = 15000;      // Time allowed for the NTP sync to complete.
const unsigned long NTP_RETRY_MS = 60000;             // Time before retrying a failed resync.
const unsigned long STARTUP_WIFI_TIMEOUT_MS = 30000;  // Startup: time allowed for WiFi to connect.

// Logo colour schemes: { r1, g1, b1, r2, g2, b2 }. 0 = off.
const byte LOGO_SCHEMES[LOGO_SCHEME_COUNT][6] = {
  { 0, 0, 0, 0, 0, 0 },          // Off
  { 255, 0, 0, 160, 160, 0 },    // Red, Yellow
  { 0, 0, 255, 200, 250, 255 },  // Blue, Light Blue
  { 255, 0, 10, 0, 10, 255 },    // Red, Blue
};

byte clockBrightness;
byte logoColours = 0;  // Logo off to start with.

// Cached display values, refreshed whenever the minute changes.
byte currentMonth = 0, currentDay = 0, currentHours = 0, currentMinutes = 0;
int currentYear = 0;
bool isAM = true;

bool colonVisible = true;
int lastSecond = -1;
int lastMinute = -1;
bool fullRefresh = true;  // Forces every display to be redrawn (first update, or after a setting is shown).

bool settingShown = false;  // True while a changed setting is being shown on the left display.
unsigned long settingShownUntil = 0;

enum SyncState { SYNC_IDLE, SYNC_CONNECTING, SYNC_WAITING_FOR_NTP };
SyncState syncState = SYNC_IDLE;
unsigned long lastSyncAttempt = 0;
unsigned long syncStateStart = 0;
unsigned long syncDueAfterMs = 0;
volatile bool ntpSynced = false;

ESP32_WS2812 pixels = ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

TM1637Display red1(DISPLAY_CLK, DISPLAY1_DIO);
TM1637Display red2(DISPLAY_CLK, DISPLAY2_DIO);
TM1637Display red3(DISPLAY_CLK, DISPLAY3_DIO);

void onNtpSync(struct timeval* tv) {
  ntpSynced = true;
}

byte amPmBrightness() {
  return (255 / 8) * (clockBrightness + 1);
}

void setup() {

  Serial.begin(9600);  // Start Serial for Debugging
  Serial.println("Initialising.");

  // Pin initialization
  pinMode(LEDS_PIN, OUTPUT);
  pinMode(DISPLAY_CLK, OUTPUT);
  pinMode(DISPLAY1_DIO, OUTPUT);
  pinMode(DISPLAY2_DIO, OUTPUT);
  pinMode(DISPLAY3_DIO, OUTPUT);
  pinMode(AM_LED, OUTPUT);
  pinMode(PM_LED, OUTPUT);
  pinMode(BUTTON_LOGO_BRIGHTNESS, INPUT);
  pinMode(BUTTON_LOGO_COLOURS, INPUT_PULLDOWN);

  pixels.begin();
  pixels.setBrightness(LOGO_BRIGHTNESS_MAX);
  updateNeoPixels(0);

  // Switch off the displays.
  Serial.println("Switching off the displays.");
  blankDisplays();

  //Init EEPROM
  Serial.println("Retrieving the saved settings from EEPROM.");
  EEPROM.begin(EEPROM_SIZE);
  clockBrightness = EEPROM.read(0);
  logoColours = EEPROM.read(1);

  // Default the brightness to 3 and the logo colour to 1 if the saved values are out of range.
  if (clockBrightness > CLOCK_BRIGHTNESS_MAX) clockBrightness = 3;
  if (logoColours >= LOGO_SCHEME_COUNT) logoColours = 1;

  // Turn on the AM/PM indicators to show that we are starting up.
  analogWrite(AM_LED, amPmBrightness());
  analogWrite(PM_LED, amPmBrightness());

  // Connect to WiFi. Attempt to connect to saved details first.
  Serial.println("Connecting to WiFi.");
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

  if (strlen(ssid) == 0) {
    Serial.println("Using credentials saved via portal.");
    WiFiManager manager;
    manager.setConnectTimeout(10);
    manager.setConnectRetries(5);
    bool connected = false;
    if (manager.getWiFiIsSaved()) {
      // Try the saved network without raising the portal first.
      manager.setEnableConfigPortal(false);
      connected = manager.autoConnect("BTTF_LAMP_CLOCK", "password");
      if (!connected) manager.setEnableConfigPortal(true);
    }
    if (!connected) {
      // Nothing saved, or the saved network failed: launch the config portal.
      manager.setTimeout(180);
      connected = manager.autoConnect("BTTF_LAMP_CLOCK", "password");
    }
    if (!connected) {
      Serial.println("Connection failed, restarting...");
      ESP.restart();  // Reset and try again
    }
  } else {
    Serial.println("Using hard-coded credentials.");
    WiFi.begin(ssid, password);
  }

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > STARTUP_WIFI_TIMEOUT_MS) {
      Serial.println("");
      Serial.println("Timed out waiting for WiFi, restarting...");
      ESP.restart();
    }
    delay(100);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("Successfully connected to WiFi.");

  // Start SNTP with the timezone applied, and wait for it to actually sync. The RTC survives a
  // soft reset, so a valid time on its own doesn't mean that NTP has been reached.
  ntpSynced = false;
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(tzString, ntpServer);

  unsigned long ntpStart = millis();
  while (!ntpSynced && millis() - ntpStart < 2UL * NTP_SYNC_TIMEOUT_MS) {
    delay(100);
  }

  struct tm t;
  if (!getLocalTime(&t, 0)) {
    Serial.println("Could not retrieve time from NTP server, restarting...");
    analogWrite(AM_LED, 0);
    analogWrite(PM_LED, 0);
    ESP.restart();  // Reset and try again
  }
  if (ntpSynced) {
    Serial.println("Time synced from NTP server.");
  } else {
    Serial.println("NTP sync timed out, carrying on with the RTC time. Will retry shortly.");
  }

  // Turn off the AM/PM indicators.
  analogWrite(AM_LED, 0);
  analogWrite(PM_LED, 0);

  delay(1000);

  // Fade in the logo.
  for (int j = 0; j <= LOGO_BRIGHTNESS_MAX; j++) {
    pixels.setBrightness(j);
    updateNeoPixels(logoColours);
    delay(3);
  }

  delay(1000);

  stopWiFi();

  // Schedule the first resync.
  lastSyncAttempt = millis();
  syncDueAfterMs = refreshTimeFromNTPIntervalSeconds * 1000UL;
  if (!ntpSynced) syncDueAfterMs = NTP_RETRY_MS;
}

void loop() {
  handleButtons();

  struct tm t;
  bool haveTime = getLocalTime(&t, 0);  // 0 = don't block.

  updateSync(haveTime ? t.tm_sec : 0);

  if (settingShown) {
    if ((long)(millis() - settingShownUntil) >= 0) {
      settingShown = false;
      fullRefresh = true;  // Put the normal display back.
    }
  } else if (haveTime) {
    maybeUpdateClock(t);
  }

  delay(10);
}

//=========================== WiFi / NTP ==============================

void stopWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Non-blocking periodic NTP resync. Brings WiFi up, waits for SNTP to set the RTC, then shuts WiFi down again.
void updateSync(byte seconds) {
  unsigned long now = millis();

  switch (syncState) {
    case SYNC_IDLE:
      if (seconds >= 30 && now - lastSyncAttempt >= syncDueAfterMs) {
        Serial.println("Time to resync with NTP server.");
        lastSyncAttempt = now;
        syncStateStart = now;
        WiFi.mode(WIFI_STA);
        if (strlen(ssid) > 0) {
          WiFi.begin(ssid, password);
        } else {
          WiFi.begin();  // Use the credentials saved via the portal.
        }
        syncState = SYNC_CONNECTING;
      }
      break;

    case SYNC_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected to WiFi, requesting time.");
        ntpSynced = false;
        configTzTime(tzString, ntpServer);  // Restarts SNTP so that it asks straight away.
        syncStateStart = now;
        syncState = SYNC_WAITING_FOR_NTP;
      } else if (now - syncStateStart > WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("Timeout when connecting to WiFi.");
        finishSync(false);
      }
      break;

    case SYNC_WAITING_FOR_NTP:
      if (ntpSynced) {
        Serial.println("Time synced from NTP server.");
        finishSync(true);
      } else if (now - syncStateStart > NTP_SYNC_TIMEOUT_MS) {
        Serial.println("Timeout waiting for NTP server.");
        finishSync(false);
      }
      break;
  }
}

void finishSync(bool success) {
  stopWiFi();
  syncDueAfterMs = success ? refreshTimeFromNTPIntervalSeconds * 1000UL : NTP_RETRY_MS;
  syncState = SYNC_IDLE;
}

//=============================== Displays ============================

void blankDisplays() {
  red1.setBrightness(0, false);
  red2.setBrightness(0, false);
  red3.setBrightness(0, false);
  red1.clear();
  red2.clear();
  red3.clear();
}

// Writes a pair of two-digit numbers to a 4-digit display in a single transfer, optionally with the colon on.
void showPair(TM1637Display& display, byte left, byte right, bool colon) {
  uint8_t segments[4] = {
    display.encodeDigit(left / 10),
    (uint8_t)(display.encodeDigit(left % 10) | (colon ? 0x80 : 0x00)),
    display.encodeDigit(right / 10),
    display.encodeDigit(right % 10),
  };
  display.setSegments(segments);
}

// Called from loop() when the RTC second changes (or when a full redraw is needed).
void maybeUpdateClock(const struct tm& t) {
  if (t.tm_sec == lastSecond && !fullRefresh) return;

  lastSecond = t.tm_sec;
  colonVisible = (t.tm_sec % 2 == 0);

  bool minuteChanged = fullRefresh || t.tm_min != lastMinute;

  if (minuteChanged) {
    lastMinute = t.tm_min;
    currentYear = t.tm_year + 1900;
    currentMonth = t.tm_mon + 1;
    currentDay = t.tm_mday;
    currentHours = t.tm_hour;
    currentMinutes = t.tm_min;
    isAM = currentHours < 12;

    // Convert to 12H format if required.
    if (!clock24h) {
      currentHours = currentHours % 12;
      if (currentHours == 0) currentHours = 12;  // Midnight and noon.
    }
  }

  if (fullRefresh) {
    red1.setBrightness(clockBrightness, true);
    red2.setBrightness(clockBrightness, true);
    red3.setBrightness(clockBrightness, true);
    fullRefresh = false;
  }

  if (minuteChanged) {
    if (dateDisplayFormat == 1) {
      showPair(red1, currentMonth, currentDay, true);
    } else {
      showPair(red1, currentDay, currentMonth, true);
    }
    red2.showNumberDecEx(currentYear, 0b00000000, true);
    updateAMPM();
  }

  showPair(red3, currentHours, currentMinutes, colonVisible);
}

void updateAMPM() {
  // Adjust the AM/PM LEDs based on clockBrightness
  byte brightness = amPmBrightness();

  if (isAM) {
    analogWrite(AM_LED, brightness);
    analogWrite(PM_LED, 0);
  } else {
    analogWrite(PM_LED, brightness);
    analogWrite(AM_LED, 0);
  }
}

// Shows a changed setting on the left display (the others and the AM/PM LEDs go off) for a short while.
// loop() restores the normal display afterwards.
void showSetting(int value) {
  blankForSetting();
  red1.showNumberDec(value, true);
  holdSettingDisplay(SETTING_DISPLAY_MS);
}

// Shows the firmware version number on the left display, e.g. " 20" for version 20.
void showVersion() {
  Serial.print("Firmware version: ");
  Serial.println(FIRMWARE_VERSION);
  blankForSetting();
  red1.showNumberDec(FIRMWARE_VERSION, false);
  holdSettingDisplay(VERSION_DISPLAY_MS);
}

// Leaves only the left display lit, and the AM/PM LEDs off.
void blankForSetting() {
  red1.setBrightness(clockBrightness, true);
  red2.setBrightness(clockBrightness, false);
  red3.setBrightness(clockBrightness, false);
  red2.clear();
  red3.clear();
  analogWrite(PM_LED, 0);
  analogWrite(AM_LED, 0);
}

void holdSettingDisplay(unsigned long ms) {
  settingShown = true;
  settingShownUntil = millis() + ms;
}

//=============================== Buttons =============================

void restartClock() {
  blankDisplays();
  analogWrite(PM_LED, 0);
  analogWrite(AM_LED, 0);
  updateNeoPixels(0);
  ESP.restart();
}

void cycleBrightness() {
  Serial.println("Changing display brightness.");
  clockBrightness = (clockBrightness + 1) % (CLOCK_BRIGHTNESS_MAX + 1);

  // Save the new brightness value to flash memory
  Serial.println("Writing new display brightness level to EEPROM.");
  EEPROM.write(0, clockBrightness);
  EEPROM.commit();

  showSetting(clockBrightness + 1);
}

void cycleLogoColour() {
  Serial.println("Changing logo colour.");
  logoColours = (logoColours + 1) % LOGO_SCHEME_COUNT;

  // Save the new logo colour to flash memory
  Serial.println("Writing new logo colour setting to EEPROM.");
  EEPROM.write(1, logoColours);
  EEPROM.commit();

  updateNeoPixels(logoColours);
  showSetting(logoColours);
}

// Single presses act on release. Tapping both buttons together shows the firmware version.
// Holding both buttons for RESET_HOLD_MS restarts the clock.
void handleButtons() {
  static bool prevBrightness = false, prevColours = false, combo = false;
  static unsigned long bothSince = 0, bothHeldMs = 0, lastAction = 0;

  unsigned long now = millis();
  bool brightness = digitalRead(BUTTON_LOGO_BRIGHTNESS);
  bool colours = digitalRead(BUTTON_LOGO_COLOURS);

  if (brightness && colours) {
    combo = true;
    if (bothSince == 0) {
      bothSince = now;
    } else if (now - bothSince >= RESET_HOLD_MS) {
      restartClock();
    }
  } else if (bothSince != 0) {
    bothHeldMs = now - bothSince;  // The combo has just been broken, remember how long it was held.
    bothSince = 0;
  }

  if (!brightness && !colours) {
    if (now - lastAction > BUTTON_DEBOUNCE_MS) {
      if (combo) {
        if (bothHeldMs >= 50) {  // A real tap of both buttons, not a bounce.
          lastAction = now;
          showVersion();
        }
      } else if (prevBrightness) {
        lastAction = now;
        cycleBrightness();
      } else if (prevColours) {
        lastAction = now;
        cycleLogoColour();
      }
    }
    combo = false;
    bothHeldMs = 0;
  }

  prevBrightness = brightness;
  prevColours = colours;
}

//=============================== Logo ================================

void updateNeoPixels(byte scheme) {
  const byte* c = LOGO_SCHEMES[scheme];

  for (int i = 0; i < LEDS_COUNT; i++) {
    // The middle six LEDs use the second colour, the outer twelve the first.
    bool middle = (i >= 6 && i < 12);
    pixels.setLedColorData(i, c[middle ? 3 : 0], c[middle ? 4 : 1], c[middle ? 5 : 2]);
  }
  pixels.show();
}
