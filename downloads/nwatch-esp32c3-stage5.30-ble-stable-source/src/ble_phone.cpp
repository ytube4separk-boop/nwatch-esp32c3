#include "ble_phone.h"


#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>

#include <cstring>

namespace {

const NimBLEUUID ANCS_SERVICE("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
const NimBLEUUID ANCS_SOURCE("9FBF120D-6301-42D9-8C58-25E699A21DBD");
const NimBLEUUID ANCS_CONTROL("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
const NimBLEUUID ANCS_DATA("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
const NimBLEUUID CTS_SERVICE(uint16_t(0x1805));
const NimBLEUUID CTS_TIME(uint16_t(0x2a2b));

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

struct LinkState {
  NimBLEServer *server = nullptr;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *source = nullptr;
  NimBLERemoteCharacteristic *control = nullptr;
  NimBLERemoteCharacteristic *data = nullptr;
  NimBLERemoteCharacteristic *time = nullptr;
  bool initialized = false;
  bool connected = false;
  bool secured = false;
  bool securityRequested = false;
  bool discoveryPending = false;
  bool discovering = false;
  bool subscribed = false;
  uint32_t nextDiscoveryMs = 0;
  uint32_t lastTimeReadMs = 0;
  char status[32] = "BLE STARTING";
} bleLink;

BlePhoneTime pendingTime = {};
bool timeReady = false;
BlePhoneNotification pendingNotice = {};
bool noticeReady = false;

struct QueuedNotice {
  uint32_t uid;
  uint8_t flags;
  uint8_t category;
};

QueuedNotice queue[6] = {};
uint8_t queueCount = 0;

struct AttributeParser {
  bool active = false;
  uint8_t header[5] = {};
  uint8_t tuple[3] = {};
  uint16_t headerUsed = 0;
  uint16_t tupleUsed = 0;
  uint16_t fieldLength = 0;
  uint16_t fieldUsed = 0;
  uint8_t fields = 0;
  uint8_t field = 0;
  uint32_t uid = 0;
  uint32_t bytes = 0;
  BlePhoneNotification value = {};
} parser;

uint32_t little32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

void setStatus(const char *value) {
  portENTER_CRITICAL(&mux);
  snprintf(bleLink.status, sizeof(bleLink.status), "%s", value);
  portEXIT_CRITICAL(&mux);
  Serial.printf("[NIMBLE] %s heap=%u\n", value, ESP.getFreeHeap());
}

void enqueueNotice(uint32_t uid, uint8_t flags, uint8_t category) {
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0; i < queueCount; ++i) {
    if (queue[i].uid == uid) {
      queue[i].flags = flags;
      queue[i].category = category;
      portEXIT_CRITICAL(&mux);
      return;
    }
  }
  if (queueCount == 6) {
    memmove(queue, queue + 1, sizeof(queue[0]) * 5);
    queueCount = 5;
  }
  queue[queueCount++] = {uid, flags, category};
  portEXIT_CRITICAL(&mux);
}

void removeNotice(uint32_t uid) {
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0; i < queueCount; ++i) {
    if (queue[i].uid == uid) {
      memmove(queue + i, queue + i + 1, sizeof(queue[0]) * (queueCount - i - 1));
      --queueCount;
      break;
    }
  }
  if (parser.active && parser.uid == uid)
    parser.active = false;
  portEXIT_CRITICAL(&mux);
}

char *attributeField(uint8_t id, size_t &capacity) {
  switch (id) {
  case 0:
    capacity = sizeof(parser.value.app);
    return parser.value.app;
  case 1:
    capacity = sizeof(parser.value.title);
    return parser.value.title;
  case 3:
    capacity = sizeof(parser.value.message);
    return parser.value.message;
  default:
    capacity = 0;
    return nullptr;
  }
}

void finishFieldLocked() {
  size_t capacity = 0;
  char *out = attributeField(parser.field, capacity);
  if (out && capacity)
    out[parser.fieldUsed < capacity - 1 ? parser.fieldUsed : capacity - 1] = 0;
  parser.tupleUsed = 0;
  parser.fieldLength = 0;
  parser.fieldUsed = 0;
  ++parser.fields;
  if (parser.fields == 5) {
    if (!parser.value.title[0])
      snprintf(parser.value.title, sizeof(parser.value.title), "%s", parser.value.app);
    pendingNotice = parser.value;
    noticeReady = true;
    parser.active = false;
  }
}

void parseAncsData(const uint8_t *data, size_t length) {
  static const uint8_t expected[] = {0, 1, 2, 3, 5};
  bool delivered = false;

  portENTER_CRITICAL(&mux);
  if (!parser.active) {
    portEXIT_CRITICAL(&mux);
    return;
  }

  for (size_t pos = 0; pos < length && parser.active; ++pos) {
    uint8_t byte = data[pos];
    if (++parser.bytes > 2048) {
      parser.active = false;
      break;
    }

    if (parser.headerUsed < 5) {
      parser.header[parser.headerUsed++] = byte;
      if (parser.headerUsed == 5 &&
          (parser.header[0] != 0 || little32(parser.header + 1) != parser.uid))
        parser.active = false;
      continue;
    }

    if (parser.tupleUsed < 3) {
      parser.tuple[parser.tupleUsed++] = byte;
      if (parser.tupleUsed < 3)
        continue;
      if (parser.fields >= sizeof(expected) || parser.tuple[0] != expected[parser.fields]) {
        parser.active = false;
        break;
      }
      parser.field = parser.tuple[0];
      parser.fieldLength = parser.tuple[1] | (uint16_t(parser.tuple[2]) << 8);
      parser.fieldUsed = 0;
      if (parser.fieldLength > 1024) {
        parser.active = false;
        break;
      }
      if (parser.fieldLength == 0)
        finishFieldLocked();
      continue;
    }

    size_t capacity = 0;
    char *out = attributeField(parser.field, capacity);
    if (out && parser.fieldUsed + 1 < capacity)
      out[parser.fieldUsed] = byte ? char(byte) : ' ';
    ++parser.fieldUsed;
    if (parser.fieldUsed == parser.fieldLength)
      finishFieldLocked();
  }

  delivered = noticeReady;
  portEXIT_CRITICAL(&mux);
  if (delivered)
    setStatus("NOTIFICATION RX");
}

void notificationSourceCallback(NimBLERemoteCharacteristic *, uint8_t *data, size_t length,
                                bool) {
  if (length != 8)
    return;
  uint8_t action = data[0];
  uint32_t uid = little32(data + 4);
  if (action == 2)
    removeNotice(uid);
  else if (action <= 1)
    enqueueNotice(uid, data[1], data[2]);
}

void dataSourceCallback(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
  parseAncsData(data, length);
}

void resetRemoteState() {
  portENTER_CRITICAL(&mux);
  bleLink.source = nullptr;
  bleLink.control = nullptr;
  bleLink.data = nullptr;
  bleLink.time = nullptr;
  bleLink.secured = false;
  bleLink.securityRequested = false;
  bleLink.discoveryPending = false;
  bleLink.discovering = false;
  bleLink.subscribed = false;
  bleLink.lastTimeReadMs = 0;
  queueCount = 0;
  parser.active = false;
  portEXIT_CRITICAL(&mux);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    NimBLEClient *client = server->getClient(info);
    portENTER_CRITICAL(&mux);
    bleLink.client = client;
    bleLink.connected = client != nullptr;
    bleLink.secured = info.isEncrypted();
    bleLink.securityRequested = false;
    bleLink.discoveryPending = info.isEncrypted();
    bleLink.nextDiscoveryMs = millis() + 250;
    portEXIT_CRITICAL(&mux);

    // 30-60 ms interval, latency 4, 4 s supervision: much lower radio duty cycle.
    server->updateConnParams(info.getConnHandle(), 24, 48, 4, 400);
    setStatus(info.isEncrypted() ? "RECONNECTED - SYNC" : "CONNECTED - PAIR");
  }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    bool ok = info.isEncrypted();
    portENTER_CRITICAL(&mux);
    bleLink.secured = ok;
    bleLink.discoveryPending = ok;
    bleLink.discovering = false;
    bleLink.nextDiscoveryMs = millis() + 250;
    portEXIT_CRITICAL(&mux);
    setStatus(ok ? "PAIRED - SYNCING" : "PAIRING FAILED");
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    portENTER_CRITICAL(&mux);
    bleLink.connected = false;
    bleLink.client = nullptr;
    portEXIT_CRITICAL(&mux);
    resetRemoteState();
    char message[32];
    snprintf(message, sizeof(message), "DISCONNECTED %d", reason);
    setStatus(message);
  }
} serverCallbacks;

bool decodeCurrentTime(const uint8_t *value, size_t length) {
  if (length < 10)
    return false;
  BlePhoneTime decoded = {uint16_t(value[0] | (uint16_t(value[1]) << 8)), value[2],
                          value[3], value[4], value[5], value[6]};
  bool valid = decoded.year >= 2024 && decoded.year <= 2099 && decoded.month >= 1 &&
               decoded.month <= 12 && decoded.day >= 1 && decoded.day <= 31 &&
               decoded.hour < 24 && decoded.minute < 60 && decoded.second < 60;
  if (!valid)
    return false;
  portENTER_CRITICAL(&mux);
  pendingTime = decoded;
  timeReady = true;
  bleLink.lastTimeReadMs = millis();
  portEXIT_CRITICAL(&mux);
  setStatus("TIME SYNCED");
  return true;
}

void readCurrentTime() {
  NimBLERemoteCharacteristic *timeChar = nullptr;
  bool possible = false;
  portENTER_CRITICAL(&mux);
  possible = bleLink.connected && bleLink.secured && bleLink.time;
  timeChar = bleLink.time;
  portEXIT_CRITICAL(&mux);
  if (!possible)
    return;

  NimBLEAttValue value = timeChar->readValue();
  if (!decodeCurrentTime(value.data(), value.size())) {
    portENTER_CRITICAL(&mux);
    bleLink.lastTimeReadMs = millis();
    portEXIT_CRITICAL(&mux);
  }
}

bool discoverPhoneServices() {
  NimBLEClient *client = nullptr;
  portENTER_CRITICAL(&mux);
  if (!bleLink.connected || !bleLink.secured || !bleLink.client || bleLink.discovering) {
    portEXIT_CRITICAL(&mux);
    return false;
  }
  bleLink.discovering = true;
  bleLink.discoveryPending = false;
  client = bleLink.client;
  portEXIT_CRITICAL(&mux);

  setStatus("DISCOVERING PHONE");
  NimBLERemoteService *ancs = client->getService(ANCS_SERVICE);
  NimBLERemoteService *cts = client->getService(CTS_SERVICE);
  NimBLERemoteCharacteristic *source = ancs ? ancs->getCharacteristic(ANCS_SOURCE) : nullptr;
  NimBLERemoteCharacteristic *control = ancs ? ancs->getCharacteristic(ANCS_CONTROL) : nullptr;
  NimBLERemoteCharacteristic *data = ancs ? ancs->getCharacteristic(ANCS_DATA) : nullptr;
  NimBLERemoteCharacteristic *timeChar = cts ? cts->getCharacteristic(CTS_TIME) : nullptr;

  bool subscribed = false;
  if (source && control && data) {
    bool dataOk = data->subscribe(true, dataSourceCallback, true);
    bool sourceOk = dataOk && source->subscribe(true, notificationSourceCallback, true);
    subscribed = dataOk && sourceOk;
  }

  portENTER_CRITICAL(&mux);
  bool stillConnected = bleLink.connected && bleLink.client == client;
  if (stillConnected) {
    bleLink.source = source;
    bleLink.control = control;
    bleLink.data = data;
    bleLink.time = timeChar;
    bleLink.subscribed = subscribed;
    bleLink.discovering = false;
    bleLink.discoveryPending = !subscribed;
    bleLink.nextDiscoveryMs = millis() + 5000;
  }
  portEXIT_CRITICAL(&mux);

  if (!stillConnected)
    return false;
  if (timeChar)
    readCurrentTime();
  setStatus(subscribed ? "ANCS + CTS READY"
                       : (timeChar ? "TIME READY - ANCS MISS" : "PHONE SERVICES MISS"));
  return subscribed;
}

void requestNextNotice() {
  QueuedNotice next = {};
  NimBLERemoteCharacteristic *control = nullptr;
  bool fetch = false;

  portENTER_CRITICAL(&mux);
  fetch = bleLink.connected && bleLink.subscribed && bleLink.control && !parser.active && queueCount;
  if (fetch) {
    next = queue[0];
    memmove(queue, queue + 1, sizeof(queue[0]) * (queueCount - 1));
    --queueCount;
    memset(&parser, 0, sizeof(parser));
    parser.active = true;
    parser.uid = next.uid;
    parser.value.category = next.category;
    parser.value.flags = next.flags;
    control = bleLink.control;
  }
  portEXIT_CRITICAL(&mux);

  if (!fetch)
    return;

  uint8_t command[] = {0,
                       uint8_t(next.uid),
                       uint8_t(next.uid >> 8),
                       uint8_t(next.uid >> 16),
                       uint8_t(next.uid >> 24),
                       0,
                       1,
                       72,
                       0,
                       2,
                       72,
                       0,
                       3,
                       180,
                       0,
                       5};
  if (!control->writeValue(command, sizeof(command), true)) {
    portENTER_CRITICAL(&mux);
    parser.active = false;
    portEXIT_CRITICAL(&mux);
  }
}

} // namespace

bool blePhoneInit() {
  if (!NimBLEDevice::init("NWatch C3")) {
    setStatus("NIMBLE INIT FAILED");
    return false;
  }

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setPower(-12); // Short-range watch link; minimizes RF peak current.

  NimBLEServer *server = NimBLEDevice::createServer();
  if (!server) {
    setStatus("NIMBLE SERVER FAILED");
    return false;
  }
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  NimBLEAdvertising *advertising = server->getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);

  // ANCS and CTS service solicitation, matching Apple's accessory discovery format.
  const uint8_t ancsSolicitation[] = {17, 0x15, 0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b,
                                      0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4,
                                      0x05, 0x79};
  const uint8_t ctsSolicitation[] = {3, 0x14, 0x05, 0x18};
  advData.addData(ancsSolicitation, sizeof(ancsSolicitation));
  advData.addData(ctsSolicitation, sizeof(ctsSolicitation));

  NimBLEAdvertisementData scanData;
  scanData.setName("NWatch C3");
  bool advConfigured = advertising->setAdvertisementData(advData);
  bool scanConfigured = advertising->setScanResponseData(scanData);
  Serial.printf("[NIMBLE] payload adv=%u scan=%u advBytes=%u scanBytes=%u\n",
                advConfigured, scanConfigured,
                unsigned(advData.getPayload().size()),
                unsigned(scanData.getPayload().size()));
  advertising->setMinInterval(1600); // 1.0 s
  advertising->setMaxInterval(2400); // 1.5 s

  portENTER_CRITICAL(&mux);
  bleLink.server = server;
  bleLink.initialized = true;
  portEXIT_CRITICAL(&mux);

  if (!advertising->start()) {
    setStatus("ADVERTISING FAILED");
    return false;
  }
  setStatus("PAIR NWatch C3");
  return true;
}

bool blePhoneShutdown() {
  bool initialized = false;
  portENTER_CRITICAL(&mux);
  initialized = bleLink.initialized;
  portEXIT_CRITICAL(&mux);

  bool ok = true;
  if (initialized)
    ok = NimBLEDevice::deinit(true);

  portENTER_CRITICAL(&mux);
  bleLink.server = nullptr;
  bleLink.client = nullptr;
  bleLink.source = nullptr;
  bleLink.control = nullptr;
  bleLink.data = nullptr;
  bleLink.time = nullptr;
  bleLink.initialized = false;
  bleLink.connected = false;
  bleLink.secured = false;
  bleLink.securityRequested = false;
  bleLink.discoveryPending = false;
  bleLink.discovering = false;
  bleLink.subscribed = false;
  bleLink.nextDiscoveryMs = 0;
  bleLink.lastTimeReadMs = 0;
  queueCount = 0;
  parser.active = false;
  timeReady = false;
  noticeReady = false;
  portEXIT_CRITICAL(&mux);
  setStatus(ok ? "BLE OFF" : "BLE STOP FAILED");
  return ok;
}

void blePhonePoll() {
  NimBLEClient *client = nullptr;
  bool connected = false;
  bool secured = false;
  bool requestSecurity = false;
  bool discover = false;
  bool syncDue = false;
  uint32_t now = millis();

  portENTER_CRITICAL(&mux);
  connected = bleLink.connected;
  secured = bleLink.secured;
  client = bleLink.client;
  if (connected && !secured && client && !bleLink.securityRequested) {
    bleLink.securityRequested = true;
    requestSecurity = true;
  }
  discover = connected && secured && bleLink.discoveryPending && !bleLink.discovering &&
             int32_t(now - bleLink.nextDiscoveryMs) >= 0;
  syncDue = connected && secured && bleLink.time &&
            (!bleLink.lastTimeReadMs || uint32_t(now - bleLink.lastTimeReadMs) >= 21600000UL);
  portEXIT_CRITICAL(&mux);

  if (requestSecurity) {
    if (!client->secureConnection(true)) {
      portENTER_CRITICAL(&mux);
      bleLink.securityRequested = false;
      portEXIT_CRITICAL(&mux);
      setStatus("PAIR REQUEST FAILED");
    }
  }

  // A bonded reconnect can become encrypted without starting a fresh pairing callback.
  if (connected && !secured && client && client->isConnected()) {
    NimBLEConnInfo info = client->getConnInfo();
    if (info.isEncrypted()) {
      portENTER_CRITICAL(&mux);
      bleLink.secured = true;
      bleLink.discoveryPending = true;
      bleLink.nextDiscoveryMs = now + 100;
      portEXIT_CRITICAL(&mux);
    }
  }

  if (discover)
    discoverPhoneServices();
  if (syncDue)
    readCurrentTime();
  requestNextNotice();
}

bool blePhoneTakeTime(BlePhoneTime &value) {
  portENTER_CRITICAL(&mux);
  bool ready = timeReady;
  if (ready) {
    value = pendingTime;
    timeReady = false;
  }
  portEXIT_CRITICAL(&mux);
  return ready;
}

bool blePhoneTakeNotification(BlePhoneNotification &value) {
  portENTER_CRITICAL(&mux);
  bool ready = noticeReady;
  if (ready) {
    value = pendingNotice;
    noticeReady = false;
  }
  portEXIT_CRITICAL(&mux);
  return ready;
}

bool blePhoneConnected() {
  portENTER_CRITICAL(&mux);
  bool value = bleLink.connected;
  portEXIT_CRITICAL(&mux);
  return value;
}

bool blePhoneSecured() {
  portENTER_CRITICAL(&mux);
  bool value = bleLink.secured;
  portEXIT_CRITICAL(&mux);
  return value;
}

bool blePhoneReady() {
  portENTER_CRITICAL(&mux);
  bool value = bleLink.subscribed;
  portEXIT_CRITICAL(&mux);
  return value;
}

const char *blePhoneStateText() {
  static char copy[32];
  portENTER_CRITICAL(&mux);
  snprintf(copy, sizeof(copy), "%s", bleLink.status);
  portEXIT_CRITICAL(&mux);
  return copy;
}
