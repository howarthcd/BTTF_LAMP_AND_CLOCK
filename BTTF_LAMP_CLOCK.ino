//  v2 - use TimeLib.h to correctly extract the date, seems to be a bug in NTPClient.h
//     - turn on the AM and PM LEDs at startup to indicate activity
//  v3 - implement numeric display brightness adjustment by button
//  v4 - refactored
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

#include "ESP32_WS2812_Lib.h"  //https://github.com/Zhentao-Lin/ESP32_WS2812_Lib
#include <TM1637Display.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <TimeLib.h>  //https://playground.arduino.cc/Code/Time/
#include <Preferences.h>

// Pin Definitions
#define PIN 5  //LED Strip Pin

#define red_CLK 16
#define red1_DIO 17
#define red2_DIO 18
#define red3_DIO 19
#define AM 32
#define PM 33
#define analogPin 34

// Constants
#define NUMPIXELS 18
#define UTC_OFFSET 1
#define LOGO_BRIGHTNESS_MAX 255

int refreshTimeFromNTPIntervalMinutes = 1;  // The minimum time inbetween time syncs from the NTP server.

const long utcOffsetInSeconds = 0;  // Non-DST Offset in seconds
int logoBrightness;
int clockBrightness;
int ledBrightness;
int var = 3;

int currentMinutes = 0, currentHours = 0, currentYear = 0, currentMonth = 0, currentDay = 0, previousMinutes = 0;
long epochTimeCurrent = 0;
long epochTimeNTP = 0;
long epochTimeLocalLastRefreshFromNTP = 0;  //Tracks the local calculated time that the last NTP refresh was attempted.
unsigned long lastColonToggleTime = 0;
bool colonVisible = true;                         // Start with the colon visible
const unsigned long COLON_FLASH_INTERVAL = 1000;  // Interval for flashing (1000ms)

Preferences preferences;  // Preferences object for storing settings

ESP32_WS2812 pixels = ESP32_WS2812(NUMPIXELS, PIN, 0);

TM1637Display red1(red_CLK, red1_DIO);
TM1637Display red2(red_CLK, red2_DIO);
TM1637Display red3(red_CLK, red3_DIO);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds* UTC_OFFSET);

int timerCount = 5;  // Set the timerCount artificially high so that the first display update happens immediately.
bool skipTimerLogic = false;

hw_timer_t* myTimer = NULL;
// timer interrupt ISR
void IRAM_ATTR onTimer() {
  // Toggle the colon
  colonVisible = !colonVisible;

  // Only update if we have previously retrieved the time
  // and we are not in the middle of processing a button press or
  // other time-sensitive action.
  if (currentYear > 0 && !skipTimerLogic) updateTimeDisplay();

  epochTimeCurrent += 1;

  // Keep track of how many times we have got here.
  timerCount += 1;
}

void setup() {

  Serial.begin(9600);  // Start Serial for Debugging
  Serial.println("Initialising.");

  // Pin initialization
  pinMode(PIN, OUTPUT);
  pinMode(red_CLK, OUTPUT);
  pinMode(red1_DIO, OUTPUT);
  pinMode(red2_DIO, OUTPUT);
  pinMode(red3_DIO, OUTPUT);
  pinMode(AM, OUTPUT);
  pinMode(PM, OUTPUT);
  pinMode(analogPin, INPUT);

  pixels.begin();
  pixels.setBrightness(LOGO_BRIGHTNESS_MAX);
  updateNeoPixels();

  // Switch off the displays.
  Serial.println("Switching off the displays.");
  red1.setBrightness(0, false);
  red2.setBrightness(0, false);
  red3.setBrightness(0, false);
  red1.showNumberDecEx(0, 0b00000000, true);
  red2.showNumberDecEx(0, 0b00000000, true);
  red3.showNumberDecEx(0, 0b00000000, true);

  // Open the preferences
  preferences.begin("settings", false);

  // Try to retrieve the saved clockBrightness, default to 3 if not found or if out of range.
  clockBrightness = preferences.getInt("clockBrightness", 3);
  if (clockBrightness > 4) clockBrightness = 3;
  if (clockBrightness < 0) clockBrightness = 3;

  // Calculate the brightness of the AM/PM indicators.
  ledBrightness = (255 / 8) * (clockBrightness + 1);

  // Turn on the AM/PM indicators to show that we are starting up.
  analogWrite(AM, ledBrightness);
  analogWrite(PM, ledBrightness);

  // Connect to WiFi. Attempt to connect to saved details first.
  Serial.println("Connecting to WiFi.");
  WiFiManager manager;
  manager.setConnectTimeout(5);
  manager.setConnectRetries(5);
  if (manager.getWiFiIsSaved()) {
    manager.setEnableConfigPortal(false);
    if (!manager.autoConnect("BTTF_LAMP_CLOCK", "password")) {
      manager.setEnableConfigPortal(true);
      manager.setTimeout(180);
      if (!manager.autoConnect("BTTF_LAMP_CLOCK", "password")) {
        Serial.println("Connection failed, restarting...");
        ESP.restart();  // Reset and try again
      }
    }
  }
  Serial.println("Successfully connected to WiFi.");
  delay(3000);

  timeClient.begin();

  Serial.println("Getting the time from the NTP server.");
  epochTimeNTP = getEpochTimeFromNTPServer();
  if (epochTimeNTP == 0) {
    Serial.println("Could not retrieve time from NTP server, restarting...");
    analogWrite(AM, 0);
    analogWrite(PM, 0);
    ESP.restart();  // Reset and try again
  } else {
    Serial.print("Epoch time from NTP server: ");
    Serial.println(epochTimeNTP);
  }
  epochTimeLocalLastRefreshFromNTP = epochTimeNTP;
  epochTimeCurrent = epochTimeNTP;

  // Turn off the AM/PM indicators.
  analogWrite(AM, 0);
  analogWrite(PM, 0);

  var = 0;

  delay(1000);

  for (int j = 0; j <= LOGO_BRIGHTNESS_MAX; j++) {
    pixels.setBrightness(j);
    updateNeoPixels();
  }

  delay(1000);

  // Define a timer. The timer will be used to toggle the time
  // colon on/off every 1s.
  Serial.println("Defining the 1000ms timer.");
  uint64_t alarmLimit = 1000000;
  myTimer = timerBegin(1000000);  // timer frequency
  timerAttachInterrupt(myTimer, &onTimer);
  timerAlarm(myTimer, alarmLimit, true, 0);
}

void loop() {

  // if (WiFi.status() != WL_CONNECTED) {
  //   Serial.println("WiFi disconnected...");

  //   for (int j = LOGO_BRIGHTNESS_MAX; j >= 0; j--) {
  //     pixels.setBrightness(j);
  //     updateNeoPixels();
  //   }

  //   WiFiManager manager;
  //   manager.setConnectTimeout(5);
  //   manager.setConnectRetries(1);
  //   if (manager.getWiFiIsSaved()) manager.setEnableConfigPortal(false);
  //   if (!manager.autoConnect("BTTF_LAMP_CLOCK", "password")) {
  //     Serial.println("Connection failed, restarting...");
  //     ESP.restart();  // Reset and try again
  //   }

  //   for (int j = 0; j <= LOGO_BRIGHTNESS_MAX; j++) {
  //     pixels.setBrightness(j);
  //     updateNeoPixels();
  //   }
  // }


  // Maybe update the time from the NTP server.
  // TODO: Check seconds so as not to set the time backwards if the local timekeeper has advanced too much.
  if (epochTimeCurrent - epochTimeLocalLastRefreshFromNTP > refreshTimeFromNTPIntervalMinutes * 60) {
    Serial.println("Getting the time from the NTP server.");
    epochTimeNTP = 0;
    epochTimeNTP = getEpochTimeFromNTPServer();
    if (epochTimeNTP > 0) {
      epochTimeCurrent = epochTimeNTP;
    }

    if (epochTimeNTP == 0) {
      Serial.println("Could not retrieve time from NTP server, try again next time.");
    } else {
      Serial.print("Epoch time from NTP server: ");
      Serial.println(epochTimeNTP);
    }

    // Store the current time so that we don't get stuck in a loop
    // if the NTP server was unavailable.
    epochTimeLocalLastRefreshFromNTP = epochTimeCurrent;
  }

  // Check to see if the time displays need to be updated.
  maybeUpdateClock();

  handleButtonPress();
}


void maybeUpdateClock() {
  if (timerCount > 0 && colonVisible) {

    //Serial.println("Entered time display update...");
    previousMinutes = currentMinutes;
    setTime(epochTimeCurrent);
    currentYear = year();
    currentMonth = month();
    currentDay = day();
    currentHours = hour();
    currentMinutes = minute();
    if (currentYear >= 2025 && previousMinutes != currentMinutes) {
      Serial.println("Updating time display.");
      updateTimeDisplay();
      red1.setBrightness(clockBrightness);
      red2.setBrightness(clockBrightness);
      red3.setBrightness(clockBrightness);
      checkDSTAndSetOffset();
      timerCount = 0;
    }
  }
}

void updateTimeDisplay() {
  // Display the date and time
  red1.showNumberDecEx(currentDay, 0b01000000, true, 2, 0);
  red1.showNumberDecEx(currentMonth, 0b01000000, true, 2, 2);
  red2.showNumberDecEx(currentYear, 0b00000000, true);

  // For the time, add the flashing colon logic
  uint8_t hour = currentHours;
  uint8_t minute = currentMinutes;

  if (colonVisible) {
    red3.showNumberDecEx(hour, 0b01000000, true, 2, 0);    // Display hours with colon
    red3.showNumberDecEx(minute, 0b01000000, true, 2, 2);  // Display minutes with colon
  } else {
    red3.showNumberDecEx(hour, 0b00000000, true, 2, 0);    // Display hours without colon
    red3.showNumberDecEx(minute, 0b00000000, true, 2, 2);  // Display minutes without colon
  }
}

void checkDSTAndSetOffset() {
  if (isDST(currentMonth, currentDay, currentYear)) {
    adjustTime(utcOffsetInSeconds + (UTC_OFFSET * 3600));  // Apply DST
    Serial.println("DST active, UTC+1");
  } else {
    adjustTime(utcOffsetInSeconds);  // Standard time
    Serial.println("DST inactive, UTC");
  }
}

void updateAMPM() {
  // Adjust the AM/PM LEDs based on clockBrightness
  ledBrightness = (255 / 8) * (clockBrightness + 1);  // Recalculate LED brightness based on current clock brightness

  if (isAM()) {
    analogWrite(AM, ledBrightness);
    analogWrite(PM, 0);
  } else {
    analogWrite(PM, ledBrightness);
    analogWrite(AM, 0);
  }
}

void handleButtonPress() {
  static unsigned long lastButtonPress = 0;
  unsigned long currentMillis = millis();

  if (analogRead(analogPin) > 100 && (currentMillis - lastButtonPress > 500)) {

    Serial.println("Changing display brightness.");
    // Disable the colon update for the duration of the button handler
    skipTimerLogic = true;
    lastButtonPress = currentMillis;
    clockBrightness = (clockBrightness + 1) % 4;  // Cycle through 0-4

    // Save the new brightness value to flash memory
    preferences.putInt("clockBrightness", clockBrightness);

    red1.setBrightness(clockBrightness);
    red2.setBrightness(clockBrightness, false);
    red3.setBrightness(clockBrightness, false);

    red1.showNumberDecEx(0, 0b00000000, true, 2, 0);
    red1.showNumberDecEx(clockBrightness + 1, 0b00000000, true, 2, 2);
    red2.showNumberDecEx(currentYear, 0b00000000, true);
    red3.showNumberDecEx(currentHours, 0b01000000, true, 2, 0);
    red3.showNumberDecEx(currentMinutes, 0b01000000, true, 2, 2);
    updateAMPM();

    delay(750);  // Delay to allow display showing the new brightness level to be seen
    red1.setBrightness(clockBrightness, true);
    red2.setBrightness(clockBrightness, true);
    red3.setBrightness(clockBrightness, true);
    updateTimeDisplay();

    // Re-enable the colon update
    skipTimerLogic = false;
  }
}

void updateNeoPixels() {

  //pixels.clear();  // Reset all pixels
  switch (var) {
    case 0:
      //setPixelColors(255, 0, 0, 160, 160, 0);  // Red, Yellow
      setPixelColors(255, 0, 0, 160, 160, 0);  // Red, Yellow
      break;
    case 1:
      setPixelColors(0, 0, 255, 200, 250, 255);  // Blue, Light Blue
      break;
    case 2:
      setPixelColors(255, 0, 10, 0, 10, 255);  // Red, Blue
      break;
    case 3:
      setPixelColors(0, 0, 0, 0, 0, 0);  // All off
      break;
  }
  pixels.show();
}

void setPixelColors(byte r1, byte g1, byte b1, byte r2, byte g2, byte b2) {
  for (int i = 0; i < 6; i++) {
    pixels.setLedColorData(i, r1, g1, b1);
  }
  for (int i = 6; i < 12; i++) {
    pixels.setLedColorData(i, r2, g2, b2);
  }
  for (int i = 12; i < 18; i++) {
    pixels.setLedColorData(i, r1, g1, b1);
  }
}

bool isDST(int month, int day, int year) {
  int lastSundayInMarch = getLastSundayOfMonth(3, year);
  int lastSundayInOctober = getLastSundayOfMonth(10, year);
  int dayOfYear = getDayOfYear(month, day, year);
  return (dayOfYear >= lastSundayInMarch && dayOfYear < lastSundayInOctober);
}

int getDayOfYear(int month, int day, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // Leap year adjustment
  }

  int dayOfYear = 0;
  for (int i = 0; i < month - 1; i++) {
    dayOfYear += daysInMonth[i];
  }
  return dayOfYear + day;
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

int getLastSundayOfMonth(int month, int year) {
  // Array with number of days per month
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }
  int lastDay = daysInMonth[month - 1];  // Last day of the month

  // Time structure to hold information about the last day of the month
  tm timeinfo;
  timeinfo.tm_year = year - 1900;  // Year since 1900
  timeinfo.tm_mon = month - 1;     // Month (0-11)
  timeinfo.tm_mday = lastDay;
  timeinfo.tm_hour = 12;  // Set to noon to avoid DST issues
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  mktime(&timeinfo);  // Normalize tm struct

  // Get the weekday (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
  int weekday = timeinfo.tm_wday;

  // Calculate the last Sunday of the month
  int lastSunday = lastDay - weekday;
  return getDayOfYear(month, lastSunday, year);  // Convert to day of the year
}

long getEpochTimeFromNTPServer() {
  long epochTime = 0;
  // Get the current time, retry if unsuccessful
  for (int i = 0; i <= 20; i++) {
    timeClient.update();
    epochTime = timeClient.getEpochTime();
    if (epochTime > 0) {
      break;
    }
    delay(50);
  }
  return epochTime;
}
