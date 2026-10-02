#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include "climate_sensor.h"
#include "project_config.h"
#include "web_page.h"

namespace {

HardwareSerial carSerial(1);
WebServer httpServer(80);
WebSocketsServer webSocket(81);

char currentCommand = 'S';
unsigned long lastBrowserCommandMs = 0;
unsigned long lastUartTxMs = 0;
unsigned long lastStm32RxMs = 0;
unsigned long lastStatusBroadcastMs = 0;
unsigned long lastVersionRequestMs = 0;
bool stm32EverSeen = false;
String uartLine;
String stm32Version;
String lastControlSource = "boot";
int servoAngle = -1;
int servoTargetAngle = 90;
unsigned long lastServoQueryMs = 0;
bool motorBoardReady = false;
constexpr int SERVO_MIN_ANGLE = 30;
constexpr int SERVO_MAX_ANGLE = 150;

bool isStm32Online() {
  return stm32EverSeen && (millis() - lastStm32RxMs <= 2500UL);
}

bool hasServoFirmware() {
  return stm32Version == "V80SG90";
}

int parseServoAngle(const String &value) {
  if (value.isEmpty() || value.length() > 3) return -1;
  for (size_t index = 0; index < value.length(); ++index) {
    if (value[index] < '0' || value[index] > '9') return -1;
  }
  const int angle = value.toInt();
  return angle >= SERVO_MIN_ANGLE && angle <= SERVO_MAX_ANGLE ? angle : -1;
}

bool isValidCommand(char command) {
  return command == 'F' || command == 'B' || command == 'L' ||
         command == 'R' || command == 'S';
}

const char *commandName(char command) {
  switch (command) {
    case 'F': return "FORWARD";
    case 'B': return "BACKWARD";
    case 'L': return "LEFT";
    case 'R': return "RIGHT";
    default: return "STOP";
  }
}

char parseCommand(String text) {
  text.trim();
  text.toUpperCase();
  if (text == "F" || text == "FORWARD") return 'F';
  if (text == "B" || text == "BACKWARD") return 'B';
  if (text == "L" || text == "LEFT") return 'L';
  if (text == "R" || text == "RIGHT") return 'R';
  if (text == "S" || text == "STOP") return 'S';
  return '\0';
}

String statusJson() {
  const bool stm32Online = isStm32Online();
  const bool stationOnline = WiFi.status() == WL_CONNECTED;
  const String stationIp = stationOnline ? WiFi.localIP().toString() : String();
  const String apIp = WiFi.softAPIP().toString();

  String json;
  json.reserve(760);
  json += F("{\"type\":\"status\",\"command\":\"");
  json += commandName(currentCommand);
  json += F("\",\"stm32_online\":");
  json += stm32Online ? F("true") : F("false");
  json += F(",\"stm32_version\":\"");
  json += stm32Version;
  json += '"';
  json += F(",\"servo_supported\":");
  json += hasServoFirmware() ? F("true") : F("false");
  json += F(",\"servo_angle\":");
  json += servoAngle >= 0 ? String(servoAngle) : String("null");
  json += F(",\"servo_target_angle\":");
  json += servoTargetAngle;
  json += F(",\"motor_ready\":");
  json += motorBoardReady && stm32Online ? F("true") : F("false");
  json += F(",\"station_connected\":");
  json += stationOnline ? F("true") : F("false");
  json += F(",\"station_ip\":\"");
  json += stationIp;
  json += F("\",\"ap_ip\":\"");
  json += apIp;
  json += F("\",\"source\":\"");
  json += lastControlSource;
  json += F("\",\"uptime_ms\":");
  json += millis();
  /* AHT10 温湿度读数，直接拼进来，网页读 state.car.climate 即可 */
  json += F(",\"climate\":");
  json += climateSensor.statusJson();
  json += '}';
  return json;
}

void broadcastStatus() {
  String status = statusJson();
  webSocket.broadcastTXT(status);
  lastStatusBroadcastMs = millis();
}

void writeUartCommand(char command) {
  carSerial.write(static_cast<uint8_t>(command));
  carSerial.write('\n');
  lastUartTxMs = millis();
}

void requestStm32Version() {
  carSerial.write(static_cast<uint8_t>('V'));
  carSerial.write('\n');
  lastVersionRequestMs = millis();
}

void applyServoAngle(int angle) {
  /* This frame must not refresh either motor-command watchdog. */
  carSerial.printf("P%03d\n", angle);
  servoTargetAngle = angle;
  broadcastStatus();
  Serial.printf("[SERVO] target %d degrees\n", angle);
}

void applyCommand(char command, const char *source) {
  if (!isValidCommand(command)) return;
  if (command != 'S' && (!isStm32Online() || !motorBoardReady)) return;
  writeUartCommand(command);
  currentCommand = command;
  lastBrowserCommandMs = millis();
  lastControlSource = source;
  broadcastStatus();
  Serial.printf("[CONTROL] %s from %s\n", commandName(command), source);
}

void forceStop(const char *reason) {
  if (currentCommand != 'S') {
    writeUartCommand('S');
    currentCommand = 'S';
    lastControlSource = reason;
    broadcastStatus();
    Serial.printf("[SAFETY] STOP: %s\n", reason);
  }
}

void processStm32Line(String line) {
  line.trim();
  if (line.isEmpty()) return;

  const bool versionLine = line.length() >= 2 && line.length() <= 16 &&
      line[0] == 'V';
  const int reportedServoAngle = line.length() == 4 && line[0] == 'P'
      ? parseServoAngle(line.substring(1)) : -1;
  if (line == "R" || line == "M0" || line == "M1" || versionLine || reportedServoAngle >= 0 || line[0] == 'A' ||
      line[0] == 'E' || line[0] == 'T') {
    stm32EverSeen = true;
    lastStm32RxMs = millis();
  }
  if (line == "R") {
    stm32Version = "";
    servoAngle = -1;
    servoTargetAngle = 90;
    motorBoardReady = false;
    lastVersionRequestMs = 0;
  } else if (versionLine) {
    stm32Version = line;
    /* Legacy firmware only announced its version after motor initialization. */
    if (!hasServoFirmware()) motorBoardReady = true;
    broadcastStatus();
  } else if (reportedServoAngle >= 0 && hasServoFirmware()) {
    servoAngle = reportedServoAngle;
    broadcastStatus();
  } else if (line == "M0" || line == "M1") {
    motorBoardReady = line == "M1";
    if (!motorBoardReady) forceStop("motor board not ready");
    broadcastStatus();
  } else if (line[0] == 'E') {
    forceStop("STM32 motor error");
  } else if (line[0] == 'T') {
    currentCommand = 'S';
    lastControlSource = "STM32 timeout";
  }
  Serial.printf("[STM32] %s\n", line.c_str());
}

void pollCarUart() {
  while (carSerial.available() > 0) {
    const char value = static_cast<char>(carSerial.read());
    if (value == '\n' || value == '\r') {
      if (!uartLine.isEmpty()) {
        processStm32Line(uartLine);
        uartLine = "";
      }
    } else if (uartLine.length() < 24) {
      uartLine += value;
    } else {
      uartLine = "";
    }
  }
}

void handleWebSocketEvent(uint8_t clientId, WStype_t type,
                          uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      {
        String status = statusJson();
        webSocket.sendTXT(clientId, status);
      }
      Serial.printf("[WEB] client %u connected\n", clientId);
      break;
    case WStype_DISCONNECTED:
      forceStop("browser disconnected");
      Serial.printf("[WEB] client %u disconnected\n", clientId);
      break;
    case WStype_TEXT: {
      String message;
      message.reserve(length);
      for (size_t index = 0; index < length; ++index) {
        message += static_cast<char>(payload[index]);
      }
      if (message.equalsIgnoreCase("PING")) {
        String status = statusJson();
        webSocket.sendTXT(clientId, status);
        break;
      }
      const char command = parseCommand(message);
      if (command != '\0') applyCommand(command, "websocket");
      break;
    }
    default:
      break;
  }
}

void configureHttpServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send_P(200, "text/html; charset=utf-8", CONTROL_PAGE);
  });

  httpServer.on("/api/status", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", statusJson());
  });

  httpServer.on("/api/command", HTTP_POST, []() {
    const String value = httpServer.hasArg("cmd")
        ? httpServer.arg("cmd") : httpServer.arg("plain");
    const char command = parseCommand(value);
    if (command == '\0') {
      httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid command\"}");
      return;
    }
    if (command != 'S' && (!isStm32Online() || !motorBoardReady)) {
      httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"motor board is not ready\"}");
      return;
    }
    applyCommand(command, "http");
    httpServer.send(200, "application/json", statusJson());
  });

  httpServer.on("/api/servo", HTTP_POST, []() {
    const int angle = httpServer.hasArg("angle")
        ? parseServoAngle(httpServer.arg("angle")) : -1;
    if (angle < 0) {
      httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"angle must be 30..150\"}");
      return;
    }
    if (!isStm32Online() || !hasServoFirmware()) {
      httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"STM32 servo firmware is not ready\"}");
      return;
    }
    applyServoAngle(angle);
    httpServer.send(200, "application/json", statusJson());
  });

  httpServer.on("/favicon.ico", HTTP_GET, []() {
    httpServer.send(204, "text/plain", "");
  });

  httpServer.onNotFound([]() {
    httpServer.send(404, "application/json", "{\"error\":\"not found\"}");
  });

  httpServer.begin();
}

void configureWifi() {
  const bool useStation = strlen(WIFI_SSID) > 0;
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(useStation ? WIFI_AP_STA : WIFI_AP);

  if (!WiFi.softAP(CAR_AP_SSID, CAR_AP_PASSWORD, 6, false, 2)) {
    Serial.println("[WIFI] failed to start access point");
  } else {
    Serial.printf("[WIFI] AP %s, open http://%s/\n",
                  CAR_AP_SSID, WiFi.softAPIP().toString().c_str());
  }

  if (useStation) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WIFI] connecting to %s", WIFI_SSID);
    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 8000UL) {
      delay(200);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] station IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WIFI] station timeout; AP control is still available");
    }
  }

  if (MDNS.begin("car-control")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] http://car-control.local/");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nBLE geofence car - web control MVP");

  carSerial.begin(CAR_UART_BAUD, SERIAL_8N1,
                  CAR_UART_RX_PIN, CAR_UART_TX_PIN);
  writeUartCommand('S');
  requestStm32Version();

  /* AHT10 温湿度传感器走 I2C，接不上也不影响小车和舵机 */
  climateSensor.begin();

  configureWifi();
  configureHttpServer();
  webSocket.begin();
  webSocket.onEvent(handleWebSocketEvent);
  Serial.println("[READY] web server on port 80, WebSocket on port 81");
}

void loop() {
  httpServer.handleClient();
  webSocket.loop();
  pollCarUart();
  climateSensor.service();

  const unsigned long now = millis();
  if (currentCommand != 'S' &&
      now - lastBrowserCommandMs > ESP32_COMMAND_TIMEOUT_MS) {
    forceStop("ESP32 command timeout");
  }

  /* Keep the STM32 link alive and periodically reassert STOP when idle. */
  if (currentCommand == 'S' && now - lastUartTxMs >= 1000UL) {
    writeUartCommand('S');
  }

  if (now - lastStatusBroadcastMs >= 1000UL) {
    broadcastStatus();
  }

  if (stm32Version.isEmpty() && now - lastVersionRequestMs >= 1000UL) {
    requestStm32Version();
  }

  if (isStm32Online() && hasServoFirmware() && servoAngle < 0 &&
      now - lastServoQueryMs >= 1000UL) {
    carSerial.print("Q\n");
    lastServoQueryMs = now;
  }

  delay(2);
}
