#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// ---------- PINS ----------
#define RELAY_PIN D1
#define TRIG_PIN  D5
#define ECHO_PIN  D6

ESP8266WebServer server(80);

// ---------- UNIQUE MAC-BASED DEVICE ID ----------
String deviceID;
char macID[13];  // MAC: A1B2C3D4E5F6

// ---------- TANK SETTINGS ----------
const float tankHeightInches = 52.0;
const float tankHeightCM = tankHeightInches * 2.54;
float waterAvg = 0;
const float SMOOTHING = 0.2;

// ---------- MQTT ----------
const char* mqtt_server   = "93bad3def29641e399de9a7b9c676613.s1.eu.hivemq.cloud";
const int   mqtt_port     = 8883;
const char* mqtt_user     = "tohits";
const char* mqtt_password = "toh123@TOH";

WiFiClientSecure espClient;
PubSubClient client(espClient);

bool motorState = false;
unsigned long bootTime = 0;
bool mqttConnected = false;

void publishMotorStatus() {
  client.publish("motor/status", motorState ? "ON" : "OFF", true);
}

void motorON() { digitalWrite(RELAY_PIN, LOW); motorState = true; publishMotorStatus(); }
void motorOFF() { digitalWrite(RELAY_PIN, HIGH); motorState = false; publishMotorStatus(); }

void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topic) == "motor/control") {
    if (msg == "ON") motorON(); else if (msg == "OFF") motorOFF();
  }
}

// ---------- UNIQUE STATUS PUBLISHING ----------
void publishStatus(bool online) {
  String topic = String("devices/") + macID + "/status";
  if (client.connected()) {
    client.publish(topic.c_str(), online ? "ONLINE" : "OFFLINE", true);
    Serial.println("📡 " + topic + " → " + String(online ? "ONLINE" : "OFFLINE"));
  }
}

void publishIP() {
  String topic = String("devices/") + macID + "/ip";
  if (client.connected()) client.publish(topic.c_str(), WiFi.localIP().toString().c_str(), true);
}

void publishUptime() {
  String topic = String("devices/") + macID + "/uptime";
  unsigned long uptimeMs = millis() - bootTime;
  unsigned long sec = uptimeMs / 1000;
  unsigned int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
  char up[16];
  snprintf(up, sizeof(up), "%02u:%02u:%02u", h, m, s);
  if (client.connected()) client.publish(topic.c_str(), up, true);
}

void reconnectMQTT() {
  if (client.connected()) return;
  
  String clientID = String("Motor_") + macID;
  Serial.print("🔄 Connecting MQTT: "); Serial.println(clientID);
  
  if (client.connect(clientID.c_str(), mqtt_user, mqtt_password)) {
    client.setCallback(callback);
    client.subscribe("motor/control");
    delay(100);
    publishStatus(true);
    publishIP();
    publishUptime();
    Serial.println("✅ MQTT Motor_" + String(macID) + " ONLINE");
  } else {
    Serial.println("❌ MQTT FAILED rc=" + String(client.state()));
    delay(5000);
  }
}

void checkWaterLevel() {
  static int lastSent = -1;
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return;
  
  float distance = duration * 0.0343 / 2.0;
  if (distance < 2.0 || distance > tankHeightCM) return;
  
  float percent = ((tankHeightCM - distance) / tankHeightCM) * 100.0;
  percent = constrain(percent, 0, 100);
  waterAvg = (SMOOTHING * percent) + ((1.0 - SMOOTHING) * waterAvg);
  
  int sendVal = round(waterAvg);
  if (sendVal != lastSent && mqttConnected) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%d", sendVal);
    client.publish("sensor/waterLevel", buf, true);
    lastSent = sendVal;
  }
}

void setup() {
  Serial.begin(9600);
  
  // ✅ UNIQUE MAC ADDRESS ID (100% DIFFERENT per ESP)
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(macID, sizeof(macID), "%02x%02x%02x%02x%02x%02x", 
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  deviceID = String(macID);
  bootTime = millis();
  
  Serial.println("🚀 MOTOR CONTROLLER");
  Serial.println("🆔 MAC ID: " + deviceID);
  Serial.println("📡 Topics: devices/" + deviceID + "/status");

  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);

  WiFiManager wm;
  char apName[20];
  snprintf(apName, sizeof(apName), "Motor_%s", macID);
  wm.autoConnect(apName);

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();
  static unsigned long lastCheck = 0;
  
  if (now - lastCheck > 5000) {
    mqttConnected = client.connected();
    if (mqttConnected) client.loop();
    else reconnectMQTT();
    
    if (mqttConnected != (now - lastCheck > 25000)) {
      publishStatus(mqttConnected);
    }
    lastCheck = now;
  }
  
  if (mqttConnected && now % 1000 < 50) checkWaterLevel();
  if (mqttConnected && now - lastCheck > 15000) publishUptime();
}
