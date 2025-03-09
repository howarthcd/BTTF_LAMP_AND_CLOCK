// v2 - use TimeLib.h to correctly extract the date, seems to be a bug in NTPClient.h
//    - turn on the AM and PM LEDs at startup to indicate activity
// v3 - implement numeric display brightness adjustment by button
// v4 - refactored
//    - included brightness setting of AM/PM indicators
// v5 - added saving of display brightness to FLASH
// v6 - add flashing colon between hour and min
//    - restrict display brightness range to 0-4

#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <Preferences.h>

// Pin Definitions
#define PIN 5
#define red_CLK 16
#define red1_DIO 17
#define red2_DIO 18
#define red3_DIO 19
#define AM 32
#define PM 33
#define analogPin 34

// Constants
#define NUMPIXELS 48
#define UTC_OFFSET 1
//#define clockBrightness 3

const long utcOffsetInSeconds = 0;  // Non-DST Offset in seconds
int clockBrightness;
int ledBrightness;
int currentMinutes = 0, currentHours = 0, currentYear = 0, currentMonth = 0, monthDay = 0;

unsigned long lastColonToggleTime = 0;
bool colonVisible = true;                         // Start with the colon visible
const unsigned long COLON_FLASH_INTERVAL = 1000;  // Interval for flashing (1000ms)

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
TM1637Display red1(red_CLK, red1_DIO);
TM1637Display red2(red_CLK, red2_DIO);
TM1637Display red3(red_CLK, red3_DIO);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds* UTC_OFFSET);

Preferences preferences;  // Preferences object for storing settings

void setup() {
  // Pin initialization
  pinMode(PIN, OUTPUT);
  pinMode(red_CLK, OUTPUT);
  pinMode(red1_DIO, OUTPUT);
  pinMode(red2_DIO, OUTPUT);
  pinMode(red3_DIO, OUTPUT);
  pinMode(AM, OUTPUT);
  pinMode(PM, OUTPUT);
  pinMode(analogPin, INPUT);

  Serial.begin(9600);  // Start Serial for Debugging

  // Open the preferences
  preferences.begin("settings", false);

  // Try to retrieve the saved clockBrightness, default to 3 if not found
  clockBrightness = preferences.getInt("clockBrightness", 3);
  if (clockBrightness > 4) clockBrightness = 3;
  if (clockBrightness < 0) clockBrightness = 3;
  ledBrightness = (255 / 8) * (clockBrightness + 1);

  analogWrite(AM, ledBrightness);
  analogWrite(PM, ledBrightness);

  WiFiManager manager;
  manager.setTimeout(180);
  if (!manager.autoConnect("BTTF_LAMP_CLOCK", "password")) {
    Serial.println("Connection failed, restarting...");
    ESP.restart();  // Reset and try again
  }

  delay(3000);

  timeClient.begin();
  red1.setBrightness(clockBrightness);
  red2.setBrightness(clockBrightness);
  red3.setBrightness(clockBrightness);
  pixels.setBrightness(250);
}

void loop() {
  static unsigned long lastTimeUpdate = 0;
  unsigned long currentMillis = millis();

  // Update time every 5 seconds (approx.)
  if (currentMillis - lastTimeUpdate >= 5000) {
    lastTimeUpdate = currentMillis;
    timeClient.update();
    setTime(timeClient.getEpochTime());
    currentYear = year();
    currentMonth = month();
    monthDay = day();
    currentMinutes = timeClient.getMinutes();
    currentHours = timeClient.getHours();
    updateTimeDisplay();
    checkDSTAndSetOffset();
    updateAMPM();
  } else if (currentMillis - lastColonToggleTime >= COLON_FLASH_INTERVAL) {
    lastColonToggleTime = currentMillis;
    colonVisible = !colonVisible;  // Toggle colon visibility
    if (currentYear > 0) updateTimeDisplay();
  }


  handleButtonPress();
  updateNeoPixels();
}

void updateTimeDisplay() {
  // Display the date and time
  red1.showNumberDecEx(monthDay, 0b01000000, true, 2, 0);
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
  if (isDST(currentMonth, monthDay, currentYear)) {
    timeClient.setTimeOffset(utcOffsetInSeconds + (UTC_OFFSET * 3600));  // Apply DST
    Serial.println("DST active, UTC+1");
  } else {
    timeClient.setTimeOffset(utcOffsetInSeconds);  // Standard time
    Serial.println("DST inactive, UTC");
  }
}

void updateAMPM() {
  // Adjust the AM/PM LEDs based on clockBrightness
  ledBrightness = (255 / 8) * (clockBrightness + 1);  // Recalculate LED brightness based on current clock brightness

  if (currentHours >= 13) {
    analogWrite(AM, 0);
    analogWrite(PM, ledBrightness);
  } else if (currentHours == 12) {
    analogWrite(AM, 0);
    analogWrite(PM, ledBrightness);
  } else {
    analogWrite(AM, ledBrightness);
    analogWrite(PM, 0);
  }
}

void handleButtonPress() {
  static unsigned long lastButtonPress = 0;
  unsigned long currentMillis = millis();

  if (analogRead(analogPin) > 100 && (currentMillis - lastButtonPress > 500)) {
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

    delay(750);  // Button debounce
    red1.setBrightness(clockBrightness, true);
    red2.setBrightness(clockBrightness, true);
    red3.setBrightness(clockBrightness, true);
    updateTimeDisplay();
  }
}

void updateNeoPixels() {
  static int var = 0;
  var = (var + 1) % 4;  // Cycle through 4 patterns

  pixels.clear();  // Reset all pixels
  switch (var) {
    case 0:
      setPixelColors(255, 0, 0, 160, 160, 0);  // Red, Yellow
      break;
    case 1:
      setPixelColors(0, 0, 255, 200, 250, 255);  // Blue, Light Blue
      break;
    case 2:
      setPixelColors(255, 0, 10, 0, 10, 255);  // Red, Blue
      break;
    case 3:
      pixels.clear();  // Turn off all pixels
      break;
  }
  pixels.show();
}

void setPixelColors(byte r1, byte g1, byte b1, byte r2, byte g2, byte b2) {
  for (int i = 0; i < 6; i++) {
    pixels.setPixelColor(i, pixels.Color(r1, g1, b1));
  }
  for (int i = 6; i < 12; i++) {
    pixels.setPixelColor(i, pixels.Color(r2, g2, b2));
  }
  for (int i = 12; i < 18; i++) {
    pixels.setPixelColor(i, pixels.Color(r1, g1, b1));
  }
}

bool isDST(int currentMonth, int monthDay, int currentYear) {
  int lastSundayInMarch = getLastSundayOfMonth(3, currentYear);
  int lastSundayInOctober = getLastSundayOfMonth(10, currentYear);
  int dayOfYear = getDayOfYear(currentMonth, monthDay, currentYear);
  return (dayOfYear >= lastSundayInMarch && dayOfYear < lastSundayInOctober);
}

int getDayOfYear(int currentMonth, int monthDay, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // Leap year adjustment
  }

  int dayOfYear = 0;
  for (int i = 0; i < currentMonth - 1; i++) {
    dayOfYear += daysInMonth[i];
  }
  return dayOfYear + monthDay;
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
