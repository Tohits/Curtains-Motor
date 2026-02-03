#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>      
#include <WiFiClientSecure.h> 
#include <PubSubClient.h>     
#include <EEPROM.h> 
#include <ESP8266httpUpdate.h> // <--- NEW: The WAN Updater

// ==========================================
//      ⚙️ PIN CONFIGURATION
// ==========================================
const int PIN_FOR_UP   = D2;  
const int PIN_FOR_DOWN = D1;  
const int CLK_PIN      = D6;  
const int DT_PIN       = D5;  

const int MAX_PULSE = 100;    

#define RELAY_ON  LOW   
#define RELAY_OFF HIGH

// ==========================================
//      MQTT SETTINGS
// ==========================================
const char* mqtt_server   = "93bad3def29641e399de9a7b9c676613.s1.eu.hivemq.cloud";
const int   mqtt_port     = 8883; 
const char* mqtt_user     = "tohits";
const char* mqtt_password = "toh123@TOH";

const char* topic_pub    = "tohits/status";      
const char* topic_sub    = "tohits/set";         
const char* topic_online = "tohits/online";      
const char* topic_info   = "tohits/info";  
const char* topic_update = "tohits/update"; // <--- NEW TOPIC FOR URL

// ==========================================
//      VARIABLES
// ==========================================
volatile long encoderValue = 0; 
long targetPulse = 0;           
int currentTask = 0; 
volatile unsigned long lastInterruptTime = 0;
unsigned long lastWiFiCheck = 0; 

WiFiClientSecure espClient;   
PubSubClient client(espClient);

// ==========================================
//      MEMORY HELPER
// ==========================================
void savePosition() {
  long storedVal;
  EEPROM.get(0, storedVal); 
  if (storedVal != encoderValue) {
    EEPROM.put(0, encoderValue);
    EEPROM.commit(); 
  }
}

// ==========================================
//      INTERRUPT
// ==========================================
void ICACHE_RAM_ATTR handleEncoder() {
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > 5) {
    if (digitalRead(DT_PIN) == LOW) {
      if (encoderValue < MAX_PULSE) encoderValue++; 
    } else {
      if (encoderValue > 0) encoderValue--;         
    }
    if (encoderValue > MAX_PULSE) encoderValue = MAX_PULSE;
    if (encoderValue < 0) encoderValue = 0;
    lastInterruptTime = interruptTime;
  }
}

// ==========================================
//      RELAY LOGIC
// ==========================================
void setTargetPercentage(int percent) {
  targetPulse = map(percent, 0, 100, 0, MAX_PULSE);
  if (targetPulse > MAX_PULSE) targetPulse = MAX_PULSE;
  if (targetPulse < 0) targetPulse = 0;
  if (abs(encoderValue - targetPulse) <= 2) return;

  if (targetPulse > encoderValue) {
    if (currentTask != 1) {
      digitalWrite(PIN_FOR_DOWN, RELAY_OFF); 
      delay(50); 
      digitalWrite(PIN_FOR_UP, RELAY_ON);    
      currentTask = 1;
    }
  } 
  else if (targetPulse < encoderValue) {
    if (currentTask != 2) {
      digitalWrite(PIN_FOR_UP, RELAY_OFF);   
      delay(50);
      digitalWrite(PIN_FOR_DOWN, RELAY_ON);  
      currentTask = 2;
    }
  }
}

// ==========================================
//      WAN UPDATE FUNCTION
// ==========================================
void performUpdate(String url) {
  Serial.println("Update Command Received!");
  Serial.println("URL: " + url);
  
  // Stop motors for safety
  digitalWrite(PIN_FOR_UP, RELAY_OFF);
  digitalWrite(PIN_FOR_DOWN, RELAY_OFF);
  client.publish(topic_info, "{\"status\":\"Updating...\"}");

  // Allow insecure HTTPS (needed for GitHub/Dropbox links)
  espClient.setInsecure();
  
  // Start Update
  t_httpUpdate_return ret = ESPhttpUpdate.update(espClient, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Update Failed Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      client.publish(topic_info, "{\"status\":\"Update Failed!\"}");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("No Update Found");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Update OK! Restarting...");
      break;
  }
}

// ==========================================
//      MQTT CALLBACK
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  
  if (String(topic) == topic_sub) {
    int percent = message.toInt();
    if (percent >= 0 && percent <= 100) setTargetPercentage(percent);
  }
  
  // --- CHECK FOR UPDATE URL ---
  if (String(topic) == topic_update) {
    performUpdate(message);
  }
}

void reconnect() {
  if (!client.connected()) {
    String clientId = "NodeMCU-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password, topic_online, 1, true, "Offline")) {
      client.publish(topic_online, "Online", true);
      client.subscribe(topic_sub); 
      client.subscribe(topic_update); // Listen for updates
      
      int currentPercent = map(encoderValue, 0, MAX_PULSE, 0, 100);
      client.publish(topic_pub, String(currentPercent).c_str(), true);
    } else {
      delay(2000);
    }
  }
}

// ==========================================
//      SETUP
// ==========================================
void setup() {
  Serial.begin(9600);
  EEPROM.begin(512); 
  
  long storedVal;
  EEPROM.get(0, storedVal);
  if (storedVal < 0 || storedVal > MAX_PULSE) encoderValue = 0;
  else encoderValue = storedVal;
  
  pinMode(PIN_FOR_UP, OUTPUT);
  pinMode(PIN_FOR_DOWN, OUTPUT);
  digitalWrite(PIN_FOR_UP, RELAY_OFF);
  digitalWrite(PIN_FOR_DOWN, RELAY_OFF);

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), handleEncoder, RISING);

  WiFiManager wifiManager;
  wifiManager.autoConnect("Smart-Knob-Setup"); 

  espClient.setInsecure(); 
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// ==========================================
//      MAIN LOOP
// ==========================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiCheck > 180000) ESP.restart(); 
  } else {
    lastWiFiCheck = millis();
    if (!client.connected()) reconnect(); 
    client.loop();
  }

  static long lastPublishedValue = -1;
  static unsigned long lastPubTime = 0;
  int currentPercent = map(encoderValue, 0, MAX_PULSE, 0, 100);
  
  if (lastPublishedValue != currentPercent && (millis() - lastPubTime > 200)) {
      client.publish(topic_pub, String(currentPercent).c_str(), true);
      lastPublishedValue = currentPercent;
      lastPubTime = millis();
  }

  static unsigned long lastInfoTime = 0;
  if (millis() - lastInfoTime > 5000) {
    lastInfoTime = millis();
    unsigned long allSeconds = millis() / 1000;
    int runDays = allSeconds / 86400;
    int runHours = (allSeconds % 86400) / 3600;
    int runMins = (allSeconds % 3600) / 60;
    String timeStr = String(runDays) + "d " + String(runHours) + "h " + String(runMins) + "m";
    String json = "{\"ip\":\"" + WiFi.localIP().toString() + "\", \"up\":\"" + timeStr + "\"}";
    client.publish(topic_info, json.c_str());
  }

  if (currentTask != 0) {
    bool stopNow = false;
    if (abs(encoderValue - targetPulse) <= 2) stopNow = true;
    if (currentTask == 1 && encoderValue >= MAX_PULSE) stopNow = true;
    if (currentTask == 2 && encoderValue <= 0) stopNow = true;

    if (stopNow) {
       digitalWrite(PIN_FOR_UP, RELAY_OFF);
       digitalWrite(PIN_FOR_DOWN, RELAY_OFF);
       currentTask = 0;
       savePosition(); 
    }
  }
}