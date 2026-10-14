#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <atomic>
#include <strings.h>
#include <string.h>
#include "secrets.h"
#include "scmp.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

extern unsigned char DisplayBuffer[8];
extern volatile unsigned char Key;
extern void ResetCPU(void);
extern void Execute(int);
extern void WriteMemory(int, int);
extern int ReadMemory(int);
extern void ClearProgramMemory(void);

// Nombre mDNS: http://mk14.local
static constexpr const char* MDNS_HOSTNAME = "mk14";

// Emulated CPU clock in MHz. Set to 1 for a slower, easier-to-follow display;
// use 2 or 4 to speed up programs without changing the display refresh rate.
static constexpr uint32_t EMULATED_CLOCK_MHZ = 1;
// Maximum "debt" of cycles the emulator may catch up after being starved.
// Prevents a burst of full-speed execution (e.g. after a Wi-Fi stall).
static constexpr int64_t MAX_LAG_CYCLES = 50000LL * EMULATED_CLOCK_MHZ;  // 50 ms

// How often the display is resent even if it has not changed. The web UI
// closes a socket that stays silent for 12 s, so this must stay well below it.
static constexpr uint32_t DISPLAY_HEARTBEAT_MS = 3000;

static uint32_t lastDisplayMs = 0;
static uint32_t lastSendMs = 0;
static uint32_t lastCleanupMs = 0;
static uint32_t lastWiFiRetryMs = 0;
static bool mdnsStarted = false;
static volatile bool mdnsPending = false;

// Set from the network task, consumed by the emulator task. The CPU state is
// only ever touched from the emulator task, so there is no data race.
static volatile bool resetRequested = false;

// Limit uploads to protect heap on the ESP32.
static constexpr size_t MAX_HEX_UPLOAD = 32768;
struct HexUploadContext {
  String content;
  bool tooLarge = false;
};

// Validated HEX waiting to be applied by the emulator task (nullptr = none).
static std::atomic<HexUploadContext*> pendingHex{nullptr};

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool readHexByte(const String& line, size_t pos, uint8_t& value) {
  if (pos + 1 >= line.length()) return false;
  int hi = hexNibble(line[pos]);
  int lo = hexNibble(line[pos + 1]);
  if (hi < 0 || lo < 0) return false;
  value = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

// Validate the complete Intel HEX file first, then apply it in a second pass
// so a bad checksum or malformed record never leaves a partially loaded file.
static bool processIntelHex(const String& content, bool apply, size_t& byteCount,
                            size_t& recordCount, String& error) {
  size_t pos = 0;
  uint32_t addressBase = 0;
  bool sawEOF = false;
  byteCount = 0;
  recordCount = 0;

  while (pos < content.length()) {
    size_t end = content.indexOf('\n', pos);
    if (end == static_cast<size_t>(-1)) end = content.length();
    String line = content.substring(pos, end);
    line.trim();
    pos = end < content.length() ? end + 1 : content.length();
    if (!line.length()) continue;
    if (sawEOF) { error = "Hay datos después del registro EOF"; return false; }
    if (line[0] != ':') { error = "Registro Intel HEX sin ':' inicial"; return false; }

    uint8_t count, addrHi, addrLo, type;
    if (!readHexByte(line, 1, count) || !readHexByte(line, 3, addrHi) ||
        !readHexByte(line, 5, addrLo) || !readHexByte(line, 7, type)) {
      error = "Cabecera Intel HEX no válida"; return false;
    }
    const size_t expectedLength = 11u + (size_t)count * 2u;
    if (line.length() != expectedLength) { error = "Longitud de registro incorrecta"; return false; }

    uint8_t sum = static_cast<uint8_t>(count + addrHi + addrLo + type);
    uint8_t dataByte = 0;
    for (uint8_t i = 0; i < count; ++i) {
      if (!readHexByte(line, 9u + (size_t)i * 2u, dataByte)) {
        error = "Dato hexadecimal no válido"; return false;
      }
      sum = static_cast<uint8_t>(sum + dataByte);
    }
    uint8_t checksum;
    if (!readHexByte(line, 9u + (size_t)count * 2u, checksum) ||
        static_cast<uint8_t>(sum + checksum) != 0) {
      error = "Checksum incorrecto en un registro"; return false;
    }

    const uint16_t offset = (uint16_t)(((uint16_t)addrHi << 8) | addrLo);
    if (type == 0x00) {
      const uint32_t absolute = addressBase + offset;
      if (absolute > 0x0FFFu || (count && absolute + count - 1u > 0x0FFFu)) {
        error = "Dirección fuera del espacio de 12 bits del MK14"; return false;
      }
      if (apply) {
        for (uint8_t i = 0; i < count; ++i) {
          uint8_t value;
          readHexByte(line, 9u + (size_t)i * 2u, value);
          WriteMemory((int)(absolute + i), value);
        }
      }
      byteCount += count;
    } else if (type == 0x01) {
      if (count != 0 || offset != 0) { error = "Registro EOF no válido"; return false; }
      sawEOF = true;
    } else if (type == 0x02) {
      uint8_t hi, lo;
      if (count != 2 || !readHexByte(line, 9, hi) || !readHexByte(line, 11, lo)) {
        error = "Registro de dirección segmentada no válido"; return false;
      }
      addressBase = (uint32_t)(((uint16_t)hi << 8) | lo) << 4;
      if (addressBase > 0x0FFFu) { error = "Segmento fuera del espacio del MK14"; return false; }
    } else if (type == 0x04) {
      uint8_t hi, lo;
      if (count != 2 || !readHexByte(line, 9, hi) || !readHexByte(line, 11, lo)) {
        error = "Registro de dirección lineal no válido"; return false;
      }
      uint16_t upper = (uint16_t)(((uint16_t)hi << 8) | lo);
      if (upper != 0) { error = "La dirección extendida supera los 12 bits del MK14"; return false; }
      addressBase = 0;
    } else if (type == 0x03 || type == 0x05) {
      // Start-address records are valid but intentionally not executed here:
      // the MK14's G key remains the explicit run/go command.
    } else {
      error = "Tipo de registro Intel HEX no compatible"; return false;
    }
    ++recordCount;
  }

  if (!sawEOF) { error = "Falta el registro EOF (:00000001FF)"; return false; }
  if (byteCount == 0) { error = "El archivo no contiene datos"; return false; }
  return true;
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[MK14] Wi-Fi connected; IP: ");
      Serial.println(WiFi.localIP());
      mdnsPending = true;  // started from loop(), not from the event task
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[MK14] Wi-Fi disconnected; retrying...");
      break;
    default:
      break;
  }
}

static int formatDisplay(char* buf, size_t size) {
  return snprintf(buf, size,
    "{\"d\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
    DisplayBuffer[0], DisplayBuffer[1], DisplayBuffer[2], DisplayBuffer[3],
    DisplayBuffer[4], DisplayBuffer[5], DisplayBuffer[6], DisplayBuffer[7]);
}

static void sendDisplay(AsyncWebSocketClient* client) {
  char json[80];
  int n = formatDisplay(json, sizeof(json));
  if (n > 0 && client && client->status() == WS_CONNECTED && client->canSend())
    client->text(json, n);
}

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  (void)server; (void)arg;
  if (type == WS_EVT_CONNECT) {
    sendDisplay(client);
    return;
  }
  if (type != WS_EVT_DATA || data == nullptr || len == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  const char* key = doc["key"] | "";
  const bool pressed = doc["pressed"] | false;
  if (!key[0] || !pressed) return;

  // Reset is a command in the virtual UI; all other keys follow the original
  // MK14 keypad's one-shot scan semantics. The reset itself is executed by the
  // emulator task so the CPU registers are never modified mid-instruction.
  if (!strcasecmp(key, "r") || !strcasecmp(key, "rst") ||
      !strcasecmp(key, "reset")) {
    resetRequested = true;
    return;
  }
  if (strlen(key) == 1) {
    char k = key[0];
    if (k >= 'A' && k <= 'Z') k = char(k - 'A' + 'a');
    Key = (unsigned char)k;
    Serial.printf("[MK14] key '%c'\n", k);
  }
}

// Applies a validated HEX handed over by the web handler. Runs in the emulator
// task, i.e. between two instruction batches, so no explicit CPU pause or
// blocking wait in the network task is needed.
static void applyPendingHex() {
  HexUploadContext* job = pendingHex.exchange(nullptr);
  if (!job) return;
  size_t bytes = 0, records = 0;
  String error;
  ClearProgramMemory();  // user RAM only (0xF12-0xFFF); monitor workspace intact
  const bool ok = processIntelHex(job->content, true, bytes, records, error);
  delete job;
  Key = 0;  // discard any key held during the upload; user enters G afresh
  if (ok) {
    Serial.printf("[MK14] Intel HEX loaded: %u data bytes, %u records\n",
                  (unsigned)bytes, (unsigned)records);
    Serial.print("[MK14] RAM F20-F27:");
    for (int address = 0xF20; address <= 0xF27; ++address)
      Serial.printf(" %02X", ReadMemory(address) & 0xFF);
    Serial.println();
  } else {
    Serial.printf("[MK14] HEX apply failed: %s\n", error.c_str());
  }
}

// The SC/MP emulation runs in its own task on core 1, paced by emulated
// cycles. Wi-Fi and AsyncTCP live on core 0 (see CONFIG_ASYNC_TCP_RUNNING_CORE
// in platformio.ini), so a busy CPU can no longer starve the network stack.
static void emuTask(void*) {
  uint32_t lastUs = micros();
  int64_t budget = 0;  // emulated cycles we are allowed to execute
  for (;;) {
    if (resetRequested) {
      resetRequested = false;
      ResetCPU();
    }
    applyPendingHex();

    const uint32_t nowUs = micros();
    budget += (int64_t)(uint32_t)(nowUs - lastUs) * EMULATED_CLOCK_MHZ;
    lastUs = nowUs;
    if (budget > MAX_LAG_CYCLES) budget = MAX_LAG_CYCLES;

    for (int i = 0; i < 2000 && budget > 0; ++i) {
      const long before = Cycles;
      Execute(1);
      long used = Cycles - before;
      if (used <= 0) used = 5;  // minimum for opcodes lacking an explicit CYC()
      budget -= used;
    }
    vTaskDelay(1);  // always yield: lets the idle task and loop() run
  }
}

void setup() {
  Serial.begin(115200);
  delay(150);
  if (!LittleFS.begin(true)) {
    Serial.println("[MK14] LittleFS mount failed");
  }

  ResetCPU();
  // Do not preload the example message at 0xF20. Programs are supplied by
  // the user's Intel HEX upload, and their entry point may be elsewhere.
  xTaskCreatePinnedToCore(emuTask, "mk14-emu", 6144, nullptr, 1, nullptr, 1);

  // Connect to the existing Wi-Fi network using credentials in include/secrets.h.
  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Avoid power-save latency affecting WebSocket stability.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[MK14] Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);
  const uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[MK14] Open http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[MK14] Wi-Fi not connected yet; background reconnection remains enabled.");
  }

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });
  server.on("/hex/upload", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      auto* upload = static_cast<HexUploadContext*>(request->_tempObject);
      request->_tempObject = nullptr;
      if (!upload) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"No se recibió el archivo\"}");
        return;
      }
      if (upload->tooLarge) {
        delete upload;
        request->send(413, "application/json", "{\"ok\":false,\"error\":\"Archivo demasiado grande (máximo 32 KiB)\"}");
        return;
      }
      Serial.printf("[MK14] HEX upload received: %u bytes\n", (unsigned)upload->content.length());

      // Validate here (cheap, no CPU access); apply later in the emulator task.
      size_t bytes = 0, records = 0;
      String error;
      if (!processIntelHex(upload->content, false, bytes, records, error)) {
        Serial.printf("[MK14] HEX rejected: %s\n", error.c_str());
        delete upload;
        String response = String("{\"ok\":false,\"error\":\"") + error + "\"}";
        request->send(400, "application/json", response);
        return;
      }

      // Hand over ownership without blocking the network task.
      HexUploadContext* expected = nullptr;
      if (!pendingHex.compare_exchange_strong(expected, upload)) {
        delete upload;
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"Hay otra carga HEX en curso; inténtalo de nuevo\"}");
        return;
      }
      String response = String("{\"ok\":true,\"bytes\":") + String(bytes) +
                        ",\"records\":" + String(records) +
                        ",\"message\":\"HEX cargado. Usa G para ejecutar desde la dirección deseada.\"}";
      request->send(200, "application/json", response);
    },
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
       size_t index, size_t total) {
      if (index == 0) {
        delete static_cast<HexUploadContext*>(request->_tempObject);
        request->_tempObject = new HexUploadContext();
      }
      auto* upload = static_cast<HexUploadContext*>(request->_tempObject);
      if (!upload) return;
      if (total > MAX_HEX_UPLOAD) {
        upload->tooLarge = true;
        return;
      }
      if (!upload->tooLarge && len) upload->content.concat((const char*)data, len);
    });

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });
  server.begin();
}

void loop() {
  // The CPU runs in emuTask(); loop() only does network housekeeping.
  const uint32_t now = millis();

  // mDNS must be (re)started after every Wi-Fi association.
  if (mdnsPending && WiFi.status() == WL_CONNECTED) {
    mdnsPending = false;
    if (mdnsStarted) MDNS.end();
    mdnsStarted = MDNS.begin(MDNS_HOSTNAME);
    if (mdnsStarted) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[MK14] mDNS: http://%s.local\n", MDNS_HOSTNAME);
    } else {
      Serial.println("[MK14] mDNS start failed");
    }
  }

  // Some access points do not recover reliably from a transient disconnect
  // with auto-reconnect alone, so retry at a bounded interval as a fallback.
  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetryMs >= 10000) {
    lastWiFiRetryMs = now;
    Serial.println("[MK14] Wi-Fi retry...");
    WiFi.reconnect();
  }

  // Broadcast the display when it changes (capped at 10 Hz) and, as a
  // heartbeat, every DISPLAY_HEARTBEAT_MS even if it has not: the web UI
  // drops sockets that stay silent for 12 s. Skip the send while any client's
  // queue is full, otherwise AsyncWebSocket disconnects it ("Too many
  // messages queued").
  static unsigned char lastBroadcast[8] = {0};
  static bool haveBroadcast = false;
  if (now - lastDisplayMs >= 100) {
    lastDisplayMs = now;
    bool changed = !haveBroadcast;
    for (int i = 0; i < 8 && !changed; ++i) {
      if (DisplayBuffer[i] != lastBroadcast[i]) changed = true;
    }
    const bool heartbeatDue = (now - lastSendMs) >= DISPLAY_HEARTBEAT_MS;
    if ((changed || heartbeatDue) && ws.count() && ws.availableForWriteAll()) {
      char json[80];
      int n = formatDisplay(json, sizeof(json));
      if (n > 0) {
        ws.textAll(json);
        memcpy(lastBroadcast, DisplayBuffer, sizeof(lastBroadcast));
        haveBroadcast = true;
        lastSendMs = now;
      }
    }
  }

  if (now - lastCleanupMs >= 1000) {
    lastCleanupMs = now;
    ws.cleanupClients();
  }

  delay(5);
}
