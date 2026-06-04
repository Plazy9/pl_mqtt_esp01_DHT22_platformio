#include <FS.h>                   // Fontos: a fájlrendszer legyen az első!
#include <LittleFS.h>             // Modern és stabilabb, mint az elavult SPIFFS

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          // WiFiManager könyvtár

#include <ArduinoJson.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <DHT.h>

#define DHTPIN          2         // GPIO2 az ESP-01-en
#define DHTTYPE         DHT22     // DHT 22 (AM2302)
DHT dht = DHT(DHTPIN, DHTTYPE);

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
Ticker wifiReconnectTimer;

#define WIFI_CONNECT_TIMEOUT_SEC 90
#define WIFI_CONNECT_RETRIES 5
#define RESET_BUTTON_PIN 0
#define RESET_HOLD_MS 5000

// Mentendő egyedi MQTT paraméterek (alapértelmezett értékekkel)
char mqtt_server[40] = "192.168.1.31";
char mqtt_server_port[6] = "1883";
char mqtt_user[40] = "";
char mqtt_password[40] = "";
char mqtt_main_topic[50] = "pl_devices";
char mqtt_device_name[40] = "esp01_DHT22_teracce_01";

// Dinamikusan felépített topikok
char full_mqtt_topic[100];
char tempMqttTopic[120];
char lwtTopic[120];

float myTemperature = 0, myHumidity = 0; 
unsigned long lastMsg = 0; 
#define MSG_BUFFER_SIZE (50)
char msg[MSG_BUFFER_SIZE];

// Flag, ami jelzi, ha a WiFiManager felületén új adatokat mentettek el
bool shouldSaveConfig = false;
bool dhtReady = false;
bool resetHoldTriggered = false;
unsigned long resetPressStartedAt = 0;

void initDht() {
  if (dhtReady) {
    return;
  }
  Serial.println("DHT init (MQTT utan)...");
  dht.begin();
  delay(2000);
  dhtReady = true;
  Serial.println("DHT keszen all.");
}

bool readDHTSensor(float &temperature, float &humidity) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    temperature = dht.readTemperature(false);
    humidity = dht.readHumidity(false);
    if (!isnan(temperature) && !isnan(humidity)) {
      return true;
    }
    delay(250);
  }
  return false;
}

void saveConfigCallback() {
  Serial.println("Config változás észlelve, mentés szükséges...");
  shouldSaveConfig = true;
}

void connectToMqtt();

void performFactoryReset() {
  Serial.println("Gyari visszaallitas indul (WiFi + MQTT config torles)...");

  mqttReconnectTimer.detach();
  wifiReconnectTimer.detach();
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }

  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      LittleFS.remove("/config.json");
      Serial.println("MQTT config torolve: /config.json");
    } else {
      Serial.println("MQTT config nem letezett.");
    }
  } else {
    Serial.println("LittleFS nem elerheto reset alatt.");
  }

  WiFi.persistent(true);
  WiFi.disconnect(true);
  delay(500);
  ESP.restart();
}

void handleResetButton() {
  bool isPressed = (digitalRead(RESET_BUTTON_PIN) == LOW);
  unsigned long now = millis();

  if (!isPressed) {
    resetPressStartedAt = 0;
    resetHoldTriggered = false;
    return;
  }

  if (resetPressStartedAt == 0) {
    resetPressStartedAt = now;
    Serial.println("Reset gomb nyomva...");
    return;
  }

  if (!resetHoldTriggered && (now - resetPressStartedAt >= RESET_HOLD_MS)) {
    resetHoldTriggered = true;
    Serial.println("Reset gomb 5mp-ig nyomva: adatok torlese.");
    performFactoryReset();
  }
}

bool waitForValidIp(uint8_t maxSeconds = 30) {
  for (uint8_t i = 0; i < maxSeconds; i++) {
    if (WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      return true;
    }
    delay(1000);
    yield();
  }
  return WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void tryReconnectWifi() {
  if (WiFi.isConnected()) {
    return;
  }
  Serial.println("WiFi ujracsatlakozas...");
  WiFi.disconnect();
  delay(100);
  WiFi.reconnect();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& evt) {
  Serial.print("WiFi bontva, reason: ");
  Serial.println(evt.reason);
  mqttReconnectTimer.detach();
  if (!wifiReconnectTimer.active()) {
    wifiReconnectTimer.once(15, tryReconnectWifi);
  }
}

void onWifiGotIP(const WiFiEventStationModeGotIP& evt) {
  wifiReconnectTimer.detach();
  Serial.print("WiFi IP: ");
  Serial.println(evt.ip);
  if (!mqttClient.connected() && !mqttReconnectTimer.active()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void connectToMqtt() {
  if (!WiFi.isConnected() || mqttClient.connected()) {
    return;
  }
  Serial.print("MQTT csatlakozas: ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.println(mqtt_server_port);
  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
  mqttReconnectTimer.detach();
  Serial.println("MQTT-hez csatlakozva.");

  snprintf(lwtTopic, sizeof(lwtTopic), "%s/%s", full_mqtt_topic, "availability");
  mqttClient.publish(lwtTopic, 1, true, "online");

  snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "commandTopic");
  mqttClient.subscribe(tempMqttTopic, 2);

  initDht();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.print("MQTT bontva, ok: ");
  Serial.println(static_cast<int>(reason));
  if (WiFi.isConnected() && !mqttReconnectTimer.active()) {
    mqttReconnectTimer.once(10, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Feliratkozás nyugtázva.");
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "commandTopic");
  if(strcmp(topic, tempMqttTopic) == 0){
    Serial.println("Relé vagy LED kapcsolási parancs érkezett");
  }
}

void onMqttPublish(uint16_t packetId) {}

// Konfiguráció betöltése a LittleFS-ből
void loadConfig() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, configFile);
        if (!error) {
          strcpy(mqtt_server, doc["mqtt_server"] | mqtt_server);
          strcpy(mqtt_server_port, doc["mqtt_server_port"] | mqtt_server_port);
          strcpy(mqtt_user, doc["mqtt_user"] | mqtt_user);
          strcpy(mqtt_password, doc["mqtt_password"] | mqtt_password);
          strcpy(mqtt_main_topic, doc["mqtt_main_topic"] | mqtt_main_topic);
          strcpy(mqtt_device_name, doc["mqtt_device_name"] | mqtt_device_name);
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("LittleFS hiba indításkor!");
  }
}

// Konfiguráció mentése a LittleFS-be
void saveConfig() {
  JsonDocument doc;
  doc["mqtt_server"] = mqtt_server;
  doc["mqtt_server_port"] = mqtt_server_port;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_password"] = mqtt_password;
  doc["mqtt_main_topic"] = mqtt_main_topic;
  doc["mqtt_device_name"] = mqtt_device_name;

  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("Beállítások sikeresen mentve a LittleFS-be.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nInditas...");
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // DHT init szandekosan kesleltetve: GPIO2 zavarhatja a WiFi/MQTT csatlakozast.
  // Az initDht() csak az onMqttConnect-ben fut le.

  // 1. Mentett beállítások betöltése a flash memóriából
  loadConfig();

  // Mód kényszerítése és Hostname beállítása ---
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  WiFi.onStationModeDisconnected(onWifiDisconnect);
  WiFi.onStationModeGotIP(onWifiGotIP);

  if (strlen(mqtt_device_name) > 0) {
    WiFi.hostname(mqtt_device_name);
  } else {
    WiFi.hostname("esp01_dht22_fallback");
  }
  // -----------------------------------------------------------

  // 2. WiFiManager paraméterek előkészítése a webes felülethez
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_server_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_password, 40);
  WiFiManagerParameter custom_mqtt_main_topic("main_topic", "MQTT Main Topic", mqtt_main_topic, 50);
  WiFiManagerParameter custom_mqtt_device_name("device_name", "Device Name (Hostname)", mqtt_device_name, 40);

  WiFiManager wifiManager;

  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SEC);
  wifiManager.setConnectRetries(WIFI_CONNECT_RETRIES);
  wifiManager.setConfigPortalTimeout(180);

  // Callback beállítása mentés esetére
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Egyedi mezők hozzáadása a WiFiManager portálhoz
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.addParameter(&custom_mqtt_main_topic);
  wifiManager.addParameter(&custom_mqtt_device_name);

  // Ha nem tud csatlakozni a mentett hálózathoz, elindítja az AP-t
  if (!wifiManager.autoConnect("ESP01_DHT22_AP", "12345678")) {
    Serial.println("Sikertelen csatlakozas (autoConnect), ujrainditas...");
    delay(3000);
    ESP.restart();
  }

  if (!waitForValidIp(30)) {
    Serial.println("Nincs ervenyes IP (DHCP), ujrainditas...");
    delay(3000);
    ESP.restart();
  }

  // Új adatok kiolvasása a felületről
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_server_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_password, custom_mqtt_pass.getValue());
  strcpy(mqtt_main_topic, custom_mqtt_main_topic.getValue());
  strcpy(mqtt_device_name, custom_mqtt_device_name.getValue());

  // Mentés után újraindítás az új hostname érvényesítéséhez ---
  if (shouldSaveConfig) {
    saveConfig();
    Serial.println("Beállítások mentve. Újraindítás az új adatokkal...");
    delay(1500);
    ESP.restart(); // Újraindul, így már az ÚJ hostname-mel fog bejelentkezni a routerbe!
  }
  // ------------------------------------------------------------------------

  Serial.println("Wi-Fi kapcsolat aktív!");
  Serial.print("IP cím: ");
  Serial.println(WiFi.localIP());
  Serial.print("Hostname: ");
  Serial.println(WiFi.hostname());

  // 3. Topikok és MQTT kliens összeállítása a beolvasott adatokból
  snprintf(full_mqtt_topic, sizeof(full_mqtt_topic), "%s/%s", mqtt_main_topic, mqtt_device_name);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  
  mqttClient.setClientId(mqtt_device_name);
  if (strlen(mqtt_user) > 0) {
    mqttClient.setCredentials(mqtt_user, mqtt_password);
  }

  Serial.print("MQTT beallitasok -> szerver: ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqtt_server_port);
  Serial.print(", user: ");
  Serial.println(strlen(mqtt_user) > 0 ? mqtt_user : "(ures)");
  Serial.print("MQTT topic: ");
  Serial.println(full_mqtt_topic);

  snprintf(lwtTopic, sizeof(lwtTopic), "%s/%s", full_mqtt_topic, "availability");
  mqttClient.setWill(lwtTopic, 1, true, "offline");
  mqttClient.setServer(mqtt_server, atoi(mqtt_server_port));

  connectToMqtt();
}

void loop() {
  unsigned long now = millis();
  handleResetButton();

  if (!WiFi.isConnected()) {
    if (!wifiReconnectTimer.active()) {
      wifiReconnectTimer.once(20, tryReconnectWifi);
    }
    return;
  }

  if (!mqttClient.connected() && !mqttReconnectTimer.active()) {
    mqttReconnectTimer.once(5, connectToMqtt);
  }

  if (!mqttClient.connected() || !dhtReady) {
    return;
  }

  if (now - lastMsg > 15000) {
    lastMsg = now;

    if (!readDHTSensor(myTemperature, myHumidity)) {
      char errorTopic[120];
      snprintf(errorTopic, sizeof(errorTopic), "%s/%s", full_mqtt_topic, "debug");
      
      if (mqttClient.connected()) {
        mqttClient.publish(errorTopic, 0, true, "HIBA: A DHT22 szenzor nem ad vissza erteket!");
      }
      return; 
    }

    // Ha jók az adatok, mehet a rendes küldés
    JsonDocument doc;
    doc["temp"] = round(myTemperature * 10) / 10.0;
    doc["humi"] = round(myHumidity * 10) / 10.0;

    char outBuffer[128];
    serializeJson(doc, outBuffer, sizeof(outBuffer));

    snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "sensors");   
    
    if (mqttClient.connected()) {
      // Küldés előtt töröljük az esetleges korábbi hibaüzenetet (üres üzenettel)
      char errorTopic[120];
      snprintf(errorTopic, sizeof(errorTopic), "%s/%s", full_mqtt_topic, "debug");
      mqttClient.publish(errorTopic, 0, true, "");

      // Rendes adatok küldése
      mqttClient.publish(tempMqttTopic, 0, true, outBuffer);
    }
  }
}