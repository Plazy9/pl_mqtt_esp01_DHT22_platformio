#include "../include/config.h"
#include <ESP8266WiFi.h>

#include <ArduinoJson.h>

#include <Ticker.h>
#include <AsyncMqttClient.h>

//temp sensor DHT
#include <DHT.h>
#define DHTPIN            2         // Pin which is connected to the DHT sensor.

// Uncomment the type of sensor in use:
//#define DHTTYPE           DHT11     // DHT 11 
#define DHTTYPE           DHT22     // DHT 22 (AM2302)
//#define DHTTYPE           DHT21     // DHT 21 (AM2301)
DHT dht = DHT(DHTPIN, DHTTYPE);

// #define MQTT_HOST IPAddress(192, 168, 1, 31)
// #define MQTT_PORT 1883

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

const char* ssid = wifiSSID;
const char* password = wifiPassword;
const char* mqtt_server = mqttServer;
const int mqtt_server_port = mqttServerPort;
const char* mqttServerUser = mqttUser;
const char* mqttServerPWD = mqttPassword;

const char* mqttMainTopic = mqttMainTopic_CFG;
const char* mqttDeviceName = mqttDeviceName_CFG; 

char full_mqtt_topic[100];
char tempMqttTopic[120];
char lwtTopic[120];

float myTemperature = 0, myHumidity = 0; 
float myTemperature_last = 0, myHumidity_last = 0; 

unsigned long lastMsg = 0; // for DHT read interval
#define MSG_BUFFER_SIZE	(50)
char msg[MSG_BUFFER_SIZE];
int packageNotSentNumber = 0;


void connectToWifi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.hostname(mqttDeviceName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  Serial.println("Connected to Wi-Fi.");
  connectToMqtt();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  wifiReconnectTimer.once(2, connectToWifi);
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
 
  snprintf(lwtTopic, sizeof(lwtTopic), "%s/%s", full_mqtt_topic, "availability");
  mqttClient.publish(lwtTopic, 2, true, "online");

  snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "commandTopic");
  mqttClient.subscribe(tempMqttTopic, 2);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "commandTopic");
  if(strcmp(topic, tempMqttTopic) == 0){
    Serial.println("led toggle, pull relay");
  }
}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  snprintf(full_mqtt_topic, sizeof(full_mqtt_topic), "%s/%s", mqttMainTopic, mqttDeviceName);
  
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // D4 LED TURN OFF HIGH STATE

  dht.begin();

  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setCredentials(mqttServerUser, mqttServerPWD);
  
  
  snprintf(lwtTopic, sizeof(lwtTopic), "%s/%s", full_mqtt_topic, "availability");
  mqttClient.setWill(lwtTopic, 1, true, "offline");
  mqttClient.setServer(mqtt_server, mqtt_server_port);

  connectToWifi();
}

void loop() {
    unsigned long now = millis();
    
    if (now - lastMsg > 15000) {
        lastMsg = now;
        myTemperature = dht.readTemperature();
        myHumidity = dht.readHumidity();

        JsonDocument doc;
        doc["temp"] = myTemperature;
        doc["humi"] = myHumidity;

        snprintf(tempMqttTopic, sizeof(tempMqttTopic), "%s/%s", full_mqtt_topic, "sensors");   
        serializeJson(doc, msg, MSG_BUFFER_SIZE);
        mqttClient.publish(tempMqttTopic, 0, true, msg);
    }
}