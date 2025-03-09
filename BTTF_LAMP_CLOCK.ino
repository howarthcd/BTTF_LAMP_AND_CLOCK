// v2 - use TimeLib.h to correctly extract the date, seems to be a bug in NTPClient.h
//    - turn on the AM and PM LEDs at startup to indicate activity

#include "Adafruit_NeoPixel.h"
#include "TM1637Display.h"
#include "WiFiManager.h"
#include "NTPClient.h"
#include "TimeLib.h"


// Which pin on the Arduino is connected to the NeoPixels?
#define PIN 5  // Strip led DIN

#define red_CLK 16
#define red1_DIO 17
#define red2_DIO 18
#define red3_DIO 19

#define AM 32
#define PM 33

// How many NeoPixels are attached to the Arduino?
#define NUMPIXELS 48  // Popular NeoPixel ring size
bool debug = true;
bool res;
int var = 0;
int analogPin = 34;
const long utcOffsetInSeconds = 0;  // Offset in second
int loopCount = 5;

//========================USEFUL VARIABLES=============================
int UTC = 1;                // UTC + value in hour - Summer time
int Display_backlight = 3;  // Set displays brightness 0 to 7;
//======================================================================

int ledBrightness = (255 / 8) * (Display_backlight + 1);

// When setting up the NeoPixel library, we tell it how many pixels,
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
// Setup the red displays
TM1637Display red1(red_CLK, red1_DIO);
TM1637Display red2(red_CLK, red2_DIO);
TM1637Display red3(red_CLK, red3_DIO);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds *UTC);


// Function to check if the year is a leap year
bool isLeapYear(int year) {
  // Leap year if divisible by 4, but not divisible by 100 unless also divisible by 400
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

// Function to calculate the day of the year (1 = Jan 1st, 365 = Dec 31st)
int getDayOfYear(int currentMonth, int monthDay, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };  // Normal year

  // Adjust February for leap year
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }

  int dayOfYear = 0;

  // Add the days of the previous months
  for (int i = 0; i < currentMonth - 1; i++) {
    dayOfYear += daysInMonth[i];
  }

  // Add the current month's days
  dayOfYear += monthDay;

  return dayOfYear;
}

// Function to calculate if a given year has daylight saving time (DST)
bool isDST(int currentMonth, int monthDay, int year) {
  // Get the last Sunday in March (DST starts in the UK)
  int lastSundayInMarch = getLastSundayOfMonth(3, year);

  // Get the last Sunday in October (DST ends in the UK)
  int lastSundayInOctober = getLastSundayOfMonth(10, year);

  // Check if the current date is between the last Sunday in March and the last Sunday in October
  int dayOfYear = getDayOfYear(currentMonth, monthDay, year);

  // If we're between March and October, DST is in effect
  return dayOfYear >= lastSundayInMarch && dayOfYear < lastSundayInOctober;
}

// Function to calculate the last Sunday of a given month in a given year
int getLastSundayOfMonth(int month, int year) {
  // Get the last day of the month (e.g., 31 for March, 30 for April, etc.)
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }
  int lastDay = daysInMonth[month - 1];

  // Find the day of the week for the last day of the month
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


void setup() {

  pinMode(PIN, OUTPUT);
  pinMode(red_CLK, OUTPUT);
  pinMode(red1_DIO, OUTPUT);
  pinMode(red2_DIO, OUTPUT);
  pinMode(red3_DIO, OUTPUT);
  pinMode(AM, OUTPUT);
  pinMode(PM, OUTPUT);

  pinMode(analogPin, INPUT);
  //manager.resetSettings();

  if (debug) Serial.begin(9600);

  analogWrite(AM, ledBrightness);
  analogWrite(PM, ledBrightness);

  WiFiManager manager;

  manager.setTimeout(180);
  //fetches ssid and password and tries to connect, if connections succeeds it starts an access point with the name called "BTTF_CLOCK" and waits in a blocking loop for configuration
  res = manager.autoConnect("BTTF_LAMP_CLOCK", "password");

  if (!res) {
    if (debug) Serial.println("failed to connect and timeout occurred");
    ESP.restart();  //reset and try again
  }

  delay(3000);

  timeClient.begin();
  red1.setBrightness(Display_backlight);
  red2.setBrightness(Display_backlight);
  red3.setBrightness(Display_backlight);
  pixels.setBrightness(250);
}

void loop() {

  // Update the time every 5 loop iterations, approx 5s due to the 1s delay at the end of the loop.
  if (loopCount == 5) {

    if (debug) Serial.println("Getting the current time");
    timeClient.update();
    if (debug) {
      Serial.print("Time: ");
      Serial.println(timeClient.getEpochTime());
    }
    unsigned long epochTime = timeClient.getEpochTime();
    setTime(epochTime);
    int currentYear = year();
    int currentMonth = month();
    int monthDay = day();

    //struct tm *ptm = gmtime((time_t *)&epochTime);
    //int currentYear = ptm->tm_year + 1900;
    //int monthDay = ptm->tm_mday;
    //int currentMonth = ptm->tm_mon + 1;

    if (debug) {
      Serial.print("Year: ");
      Serial.println(currentYear);
      Serial.print("Month day: ");
      Serial.println(monthDay);
      Serial.print("Month: ");
      Serial.println(currentMonth);
    }

    red1.showNumberDecEx(monthDay, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(currentMonth, 0b01000000, true, 2, 2);
    red2.showNumberDecEx(currentYear, 0b00000000, true);
    red3.showNumberDecEx(timeClient.getHours(), 0b01000000, true, 2, 0);
    red3.showNumberDecEx(timeClient.getMinutes(), 0b01000000, true, 2, 2);

    // Check if DST is active for the current date
    if (isDST(currentMonth, monthDay, currentYear)) {
      // Summer - DST is in effect (last Sunday of March to last Sunday of October)
      timeClient.setTimeOffset(utcOffsetInSeconds + (UTC * 3600));  // Add 1 hour for DST
      if (debug) Serial.println("DST is active, UTC+1");
    } else {
      // Winter - DST is not in effect
      timeClient.setTimeOffset(utcOffsetInSeconds);  // Standard time
      if (debug) Serial.println("DST is not active, UTC");
    }

    if (timeClient.getHours() >= 13) {
      analogWrite(AM, 0);
      analogWrite(PM, ledBrightness);
    }

    else if (timeClient.getHours() == 12) {
      analogWrite(AM, 0);
      analogWrite(PM, ledBrightness);
    }

    else {
      analogWrite(AM, ledBrightness);
      analogWrite(PM, 0);
    }

    // Reset the loop counter
    loopCount = 1;
  } else {
    // Increment the loop counter
    loopCount += 1;
  }

  pixels.clear();  // Set all pixel colors to 'off'

  if (var > 3) { var = 0; }  // Reset counter

  // read the switch
  if (analogRead(analogPin) > 100) {
    if (debug) Serial.print("Button pressed");
    var = var + 1;
    delay(45);
  }

  if (debug) {
    Serial.print("Var=");
    Serial.println(var);
    Serial.print("Digital=");
    Serial.println(analogRead(analogPin));
  }

  delay(100);
  switch (var) {
    case 0:
      for (int i = 0; i < 6; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
      for (int i = 6; i < 12; i++) {
        pixels.setPixelColor(i, pixels.Color(160, 160, 0));
      }
      for (int i = 12; i < 18; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
      pixels.show();
      break;

    case 1:
      for (int i = 0; i < 6; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 255));
      }
      for (int i = 6; i < 12; i++) {
        pixels.setPixelColor(i, pixels.Color(200, 250, 255));
      }
      for (int i = 12; i < 18; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 255));
      }
      pixels.show();
      break;

    case 2:
      pixels.clear();
      for (int i = 0; i < 6; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 10));
      }
      for (int i = 6; i < 12; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 10, 255));
      }
      for (int i = 12; i < 18; i++) {
        pixels.setPixelColor(i, pixels.Color(255, 0, 10));
      }
      pixels.show();
      break;

    case 3:
      pixels.clear();
      for (int i = 0; i < 16; i++) {
        pixels.setPixelColor(i, pixels.Color(0, 0, 0));
      }
      pixels.show();
      break;
  }

  delay(1000);
}
