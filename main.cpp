#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// === PINS & CONSTANTEN ===
#define SEALEVELPRESSURE_HPA (1007.1)
#define BMP280_ADR           0x76
#define SDA_2                32
#define SCL_2                33
#define DELAY_TIME           1000
#define SD_CSPIN             25

// === GLOBALE VARIABELEN ===
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
TwoWire I2Ctwo = TwoWire(1);
Adafruit_BMP280 bmp(&I2Ctwo);
bool bmp_connected = false;
String activeLogFile = "";
bool logFileCreated = false;

// === FUNCTIEDECLARATIES ===
void printValues();
bool initSD();
void createLogFileOnce();
void appendLog(const char* filepath, const char* data);
void mqttPublish(const char* data);
bool I2C_check(TwoWire *bus, byte address);
int getHighestFileNumber();

// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n=== CanSat ESP32 Start ===");

  // --- BMP280 ---
  I2Ctwo.begin(SDA_2, SCL_2);
  int bmp_tries = 0;
  while (!bmp.begin(BMP280_ADR) && bmp_tries < 5) {
    Serial.println("BMP280 niet gevonden! Probeer opnieuw...");
    delay(500);
    bmp_tries++;
  }
  if (bmp_tries < 5) {
    bmp_connected = true;
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("BMP280 OK");
  } else {
    Serial.println("BMP280 definitief mislukt");
  }

  // --- SD KAART ---
  int sd_tries = 0;
  while (!initSD() && sd_tries < 3) {
    Serial.println("SD mislukt! Probeer opnieuw...");
    delay(1000);
    sd_tries++;
  }
  if (sd_tries >= 3) {
    Serial.println("SD definitief mislukt → alleen MQTT");
  }

  // --- WIFI ---
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  Serial.print("Verbinding WiFi");
  int wifi_tries = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_tries < 60) {
    Serial.print(".");
    delay(500);
    wifi_tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi verbonden: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi mislukt → ga door zonder");
  }

  // --- MQTT ---
  mqttClient.setServer(BROKER, PORT);
  int mqtt_tries = 0;
  while (!mqttClient.connected() && mqtt_tries < 10) {
    if (mqttClient.connect("CanSatESP32")) {
      Serial.println("MQTT verbonden");
      break;
    }
    Serial.print("MQTT.");
    delay(1000);
    mqtt_tries++;
  }

  delay(500); // SD stabilisatie

  // --- LOGBESTAND ---
  createLogFileOnce();
}

// =============================================================
// LOOP
// =============================================================
void loop() {
  mqttClient.loop();

  if (bmp_connected && I2C_check(&I2Ctwo, BMP280_ADR) && logFileCreated) {
    printValues();
  }
  delay(DELAY_TIME);
}

// =============================================================
// SD INITIALISATIE
// =============================================================
bool initSD() {
  if (!SD.begin(SD_CSPIN)) {
    return false;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    return false;
  }
  Serial.printf("SD OK - Type: %s, Grootte: %llu MB\n",
                cardType == CARD_SDHC ? "SDHC" : "SDSC",
                SD.cardSize() / (1024ULL * 1024));
  return true;
}

// =============================================================
// Huidig hoogste genummerd bestand bepalen
// =============================================================
int getHighestFileNumber() {
  int maxVer = -1;
  File root = SD.open("/");
  if (!root || !root.isDirectory()) return -1;

  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      if (name.startsWith("/")) name = name.substring(1);
      
      if ((name.startsWith("CanSatSend ") || name.startsWith("CanSatSend_")) && 
          name.endsWith(".txt")) {
        int numStart = name.indexOf('_');
        if (numStart == -1) numStart = name.indexOf(' ');
        if (numStart != -1) {
          int ver = name.substring(numStart + 1, name.length() - 4).toInt();
          if (ver > maxVer) maxVer = ver;
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  return maxVer;
}
// =============================================================
// LOGBESTAND CREËREN + ARCHIVEREN
// =============================================================
void createLogFileOnce() {
  if (logFileCreated) return;
  delay(300);

  if (SD.exists("/CanSatSend.txt")) {
    File f = SD.open("/CanSatSend.txt", FILE_READ);
    if (f && f.size() > 0) {
      f.close();
      int nextVersion = getHighestFileNumber() + 1;
      if (nextVersion > 9999) {
        Serial.println("Opslag vol: verwijder oude bestanden!");
        return;
      }

      char versionedFile[32];
      snprintf(versionedFile, sizeof(versionedFile), "/CanSatSend_%04d.txt", nextVersion);
      
      if (SD.rename("/CanSatSend.txt", versionedFile)) {
        Serial.printf("Oud bestand gearchiveerd als: %s\n", versionedFile);
      }
    } else {
      if(f) f.close();
      SD.remove("/CanSatSend.txt");
    }
  }

  File fNew = SD.open("/CanSatSend.txt", FILE_WRITE);
  if (fNew) {
    fNew.println("T;P;A");
    fNew.close();
    activeLogFile = "/CanSatSend.txt";
    logFileCreated = true;
  }
}
// =============================================================
// METINGEN + LOGGING
// =============================================================
void printValues() {
  float t = bmp.readTemperature();
  float p = bmp.readPressure() / 100.0F;
  float a = bmp.readAltitude(SEALEVELPRESSURE_HPA);

  char csv[64];
  snprintf(csv, sizeof(csv), "%.2f;%.2f;%.2f\n", t, p, a);

  Serial.printf("T=%.2f °C | P=%.2f hPa | A=%.2f m\n", t, p, a);

  mqttPublish(csv);
  appendLog(activeLogFile.c_str(), csv);
}

void mqttPublish(const char* data) {
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC, data);
  }
}

void appendLog(const char* filepath, const char* data) {
  File f = SD.open(filepath, FILE_APPEND);
  if (f) {
    f.print(data);
    f.close();
  }
}

bool I2C_check(TwoWire *bus, byte address) {
  bus->beginTransmission(address);
  byte error = bus->endTransmission();
  return (error == 0);
}
