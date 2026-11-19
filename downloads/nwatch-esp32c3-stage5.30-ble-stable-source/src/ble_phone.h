#pragma once

#include <Arduino.h>

struct BlePhoneTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

struct BlePhoneNotification {
  uint8_t category;
  uint8_t flags;
  char app[49];
  char title[73];
  char message[181];
};

bool blePhoneInit();
bool blePhoneShutdown();
void blePhonePoll();
bool blePhoneTakeTime(BlePhoneTime &value);
bool blePhoneTakeNotification(BlePhoneNotification &value);
bool blePhoneConnected();
bool blePhoneSecured();
bool blePhoneReady();
const char *blePhoneStateText();

