#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <OneButton.h>
#include <Wire.h>

/*************************************************
 * Hardware config
 *************************************************/
#define STATUS_LED_PIN 1

// ESP32-S3 BOOT key is usually GPIO0.
// Active level is LOW.
#define BOOT_KEY_PIN 0

// TFT screen size, same style as the reference repo.
#define TFT_SCREEN_WIDTH 320
#define TFT_SCREEN_HEIGHT 240

// XL9555 I2C config.
// Keep this part close to the reference repo to avoid LCD backlight issues.
#define XL9555_ADDR 0x20
#define XL9555_SDA_PIN 41
#define XL9555_SCL_PIN 42
#define XL9555_REG_OUTP1 0x03
#define XL9555_REG_CFG1 0x07

/*************************************************
 * WiFi config
 * Change these two values to your own WiFi.
 *************************************************/
const char *WIFI_SSID = "Rei";
const char *WIFI_PASSWORD = "200562hm";

/*************************************************
 * MQTT config
 * MQTTX can connect to the same broker for testing.
 *************************************************/
const char *MQTT_SERVER = "broker.emqx.io";
const int MQTT_PORT = 1883;

// Public EMQX broker usually does not require username/password.
const char *MQTT_USER = "";
const char *MQTT_PASSWORD = "";

// Try to make the client ID unique.
const char *MQTT_CLIENT_ID = "Doorbell_ESP32S3_001";

// MQTT topics.
const char *MQTT_TOPIC_PUB = "e11a330/doorbell/status";
const char *MQTT_TOPIC_SUB = "e11a330/doorbell/control";

/*************************************************
 * Global objects
 *************************************************/
WiFiClient espClient;
PubSubClient client(espClient);
TFT_eSPI tft = TFT_eSPI();

// OneButton(pin, activeLow, pullupActive)
// BOOT key: active low, internal pull-up enabled.
OneButton bootButton(BOOT_KEY_PIN, true, true);

/*************************************************
 * System status
 *************************************************/
bool wifiOK = false;
bool mqttOK = false;
bool xl9555OK = false;

uint32_t doorbellCount = 0;

unsigned long lastWiFiRetryMs = 0;
unsigned long lastMqttRetryMs = 0;
unsigned long lastUploadMs = 0;
unsigned long lastLedBlinkMs = 0;

bool ledState = false;

/*************************************************
 * UI cache
 * Update only changed fields to reduce flicker.
 *************************************************/
String lastCountText = "";
String lastWifiText = "";
String lastMqttText = "";
String lastHintText = "";
String lastIpText = "";

/*************************************************
 * Function declarations
 *************************************************/
void wifi_init();
void wifi_check();

void mqtt_init();
void mqtt_reconnect();
void mqtt_callback(char *topic, byte *payload, unsigned int length);
void publishStatus();

void led_init();
void led_update();

bool xl9555_writeReg(uint8_t reg, uint8_t val);
void xl9555_init();

void button_init();
void onDoorbellClick();
void onDoorbellLongPress();
void resetDoorbellCount(const char *source);

void tft_init();
void tft_drawStaticUI();
void tft_updateDynamicUI();
void drawValueField(int x, int y, int w, int h, const String &value, uint16_t color, int font = 2);

/*************************************************
 * setup
 *************************************************/
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32-S3 Doorbell Counter System");
  Serial.println("================================");

  led_init();
  xl9555_init();
  tft_init();

  button_init();

  wifi_init();
  mqtt_init();

  tft_updateDynamicUI();

  Serial.println("System ready.");
}

/*************************************************
 * loop
 *************************************************/
void loop()
{
  bootButton.tick();

  wifi_check();

  if (wifiOK)
  {
    if (!client.connected())
    {
      mqtt_reconnect();
    }

    if (client.connected())
    {
      mqttOK = true;
      client.loop();
    }
    else
    {
      mqttOK = false;
    }
  }
  else
  {
    mqttOK = false;
  }

  // Upload status periodically.
  if (millis() - lastUploadMs >= 2000)
  {
    lastUploadMs = millis();
    publishStatus();
    tft_updateDynamicUI();
  }

  led_update();
}

/*************************************************
 * WiFi module
 *************************************************/
void wifi_init()
{
  Serial.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiOK = true;

    Serial.println("WiFi connected.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    wifiOK = false;
    Serial.println("WiFi connection failed.");
  }

  tft_updateDynamicUI();
}

void wifi_check()
{
  bool previousWifiOK = wifiOK;

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiOK = true;
  }
  else
  {
    wifiOK = false;

    if (millis() - lastWiFiRetryMs >= 5000)
    {
      lastWiFiRetryMs = millis();

      Serial.println("Reconnecting WiFi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (previousWifiOK != wifiOK)
  {
    tft_updateDynamicUI();
  }
}

/*************************************************
 * MQTT module
 *************************************************/
void mqtt_init()
{
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(mqtt_callback);

  Serial.println("MQTT initialized.");
}

void mqtt_reconnect()
{
  if (!wifiOK)
    return;
  if (client.connected())
    return;

  if (millis() - lastMqttRetryMs < 3000)
  {
    return;
  }

  lastMqttRetryMs = millis();

  Serial.println("Connecting MQTT...");

  bool connected = false;

  if (strlen(MQTT_USER) > 0)
  {
    connected = client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  }
  else
  {
    connected = client.connect(MQTT_CLIENT_ID);
  }

  if (connected)
  {
    mqttOK = true;

    Serial.println("MQTT connected.");
    Serial.print("Subscribe topic: ");
    Serial.println(MQTT_TOPIC_SUB);

    client.subscribe(MQTT_TOPIC_SUB);

    publishStatus();
  }
  else
  {
    mqttOK = false;

    Serial.print("MQTT connection failed. State: ");
    Serial.println(client.state());
  }

  tft_updateDynamicUI();
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  char msg[256] = {0};

  if (length >= sizeof(msg))
  {
    length = sizeof(msg) - 1;
  }

  memcpy(msg, payload, length);
  msg[length] = '\0';

  Serial.println("========== MQTT Received ==========");
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(msg);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, msg);

  if (error)
  {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  String cmd = doc["cmd"] | "";

  if (cmd == "reset" || cmd == "clear")
  {
    resetDoorbellCount("MQTT");
    publishStatus();
  }
  else if (cmd == "set")
  {
    uint32_t value = doc["value"] | doorbellCount;
    doorbellCount = value;

    Serial.print("Doorbell count set by MQTT: ");
    Serial.println(doorbellCount);

    tft_updateDynamicUI();
    publishStatus();
  }
  else
  {
    Serial.print("Unknown command: ");
    Serial.println(cmd);
  }
}

void publishStatus()
{
  if (!client.connected())
  {
    mqttOK = false;
    return;
  }

  JsonDocument doc;

  doc["device_id"] = MQTT_CLIENT_ID;
  doc["count"] = doorbellCount;
  doc["wifi"] = wifiOK;
  doc["mqtt"] = mqttOK;
  doc["ip"] = WiFi.localIP().toString();

  char buf[256];
  size_t len = serializeJson(doc, buf);

  bool ok = client.publish(MQTT_TOPIC_PUB, (uint8_t *)buf, len);

  Serial.println("========== MQTT Publish ==========");
  Serial.println(buf);
  Serial.println(ok ? "Publish OK." : "Publish failed.");
}

/*************************************************
 * LED module
 *************************************************/
void led_init()
{
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
}

void led_update()
{
  unsigned long interval = 1000;

  if (!wifiOK)
  {
    interval = 150;
  }
  else if (!mqttOK)
  {
    interval = 300;
  }
  else
  {
    interval = 1000;
  }

  if (millis() - lastLedBlinkMs >= interval)
  {
    lastLedBlinkMs = millis();
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState);
  }
}

/*************************************************
 * XL9555 module
 * Keep this module because the reference board may use XL9555
 * for LCD backlight control.
 *************************************************/
bool xl9555_writeReg(uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void xl9555_init()
{
  Wire.begin(XL9555_SDA_PIN, XL9555_SCL_PIN);
  Wire.setClock(400000);

  Wire.beginTransmission(XL9555_ADDR);

  if (Wire.endTransmission() == 0)
  {
    xl9555OK = true;
  }
  else
  {
    xl9555OK = false;
    Serial.println("XL9555 not found. LCD backlight may not work.");
    return;
  }

  // Reference repo behavior:
  // Port1 P1_2 and P1_3 are configured as outputs and pulled high.
  uint8_t cfg1 = 0xFF;
  cfg1 &= ~(1 << 2);
  cfg1 &= ~(1 << 3);

  xl9555_writeReg(XL9555_REG_CFG1, cfg1);
  xl9555_writeReg(XL9555_REG_OUTP1, 0xFF);

  Serial.println("XL9555 initialized. LCD backlight enabled.");
}

/*************************************************
 * Button module
 *************************************************/
void button_init()
{
  bootButton.setDebounceMs(30);
  bootButton.setClickMs(300);
  bootButton.setPressMs(1000);

  bootButton.attachClick(onDoorbellClick);
  bootButton.attachLongPressStart(onDoorbellLongPress);

  Serial.println("BOOT button initialized.");
}

void onDoorbellClick()
{
  doorbellCount++;

  Serial.print("Doorbell pressed. Count: ");
  Serial.println(doorbellCount);

  tft_updateDynamicUI();
  publishStatus();
}

void onDoorbellLongPress()
{
  resetDoorbellCount("Long press");
  publishStatus();
}

void resetDoorbellCount(const char *source)
{
  doorbellCount = 0;

  Serial.print("Doorbell count reset by ");
  Serial.println(source);

  tft_updateDynamicUI();
}

/*************************************************
 * TFT module
 *************************************************/
void tft_init()
{
  Serial.println("Initializing TFT...");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft_drawStaticUI();
  tft_updateDynamicUI();

  Serial.println("TFT initialized.");
}

void tft_drawStaticUI()
{
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Smart Doorbell Counter", 8, 6, 2);

  tft.drawFastHLine(0, 26, TFT_SCREEN_WIDTH, TFT_DARKGREY);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Count:", 10, 44, 2);
  tft.drawString("WiFi :", 10, 112, 2);
  tft.drawString("MQTT :", 10, 138, 2);
  tft.drawString("IP   :", 10, 164, 2);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("BOOT click      : count +1", 10, 194, 2);
  tft.drawString("BOOT long press : reset", 10, 214, 2);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Pub: e11a330/doorbell/status", 170, 112, 1);
  tft.drawString("Sub: e11a330/doorbell/control", 170, 126, 1);
}

void drawValueField(int x, int y, int w, int h, const String &value, uint16_t color, int font)
{
  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(value, x, y, font);
}

void tft_updateDynamicUI()
{
  String countText = String(doorbellCount);
  String wifiText = wifiOK ? "OK" : "OFF";
  String mqttText = mqttOK ? "OK" : "OFF";
  String ipText = wifiOK ? WiFi.localIP().toString() : "0.0.0.0";
  String hintText = doorbellCount == 0 ? "Waiting..." : "Ring received";

  if (countText != lastCountText)
  {
    drawValueField(105, 38, 200, 60, countText, TFT_WHITE, 6);
    lastCountText = countText;
  }

  if (wifiText != lastWifiText)
  {
    drawValueField(82, 112, 75, 18, wifiText, wifiOK ? TFT_GREEN : TFT_RED, 2);
    lastWifiText = wifiText;
  }

  if (mqttText != lastMqttText)
  {
    drawValueField(82, 138, 75, 18, mqttText, mqttOK ? TFT_GREEN : TFT_RED, 2);
    lastMqttText = mqttText;
  }

  if (ipText != lastIpText)
  {
    drawValueField(82, 164, 220, 18, ipText, TFT_WHITE, 2);
    lastIpText = ipText;
  }

  if (hintText != lastHintText)
  {
    drawValueField(170, 146, 135, 22, hintText, TFT_ORANGE, 2);
    lastHintText = hintText;
  }
}
