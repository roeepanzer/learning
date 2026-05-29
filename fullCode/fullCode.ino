#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
// #include <FirebaseJson.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_AS7341.h>
#include <DHT22.h>
#include <ESP32Time.h>

//Defining data for WiFi connection and Firebase connection
#define ssid "Vodafone-053608"
#define password "6UecGH4gpUuk4GNp"
#define apiKey "AIzaSyAVMPlPrVKRvZ6VPdyXHlmxEQjmL1tmges"
#define databaseURL "https://learning-d6e02-default-rtdb.europe-west1.firebasedatabase.app"
#define user_email "roee.panzer@gmail.com"
#define user_pass "Shiba852!"

//Defining pins of the components
#define DHTpin 17
#define yLEDpin 16
#define gLEDpin 18
#define rLEDpin 5

void processData(AsyncResult &aResult);
UserAuth user_auth(apiKey, user_email, user_pass);

// Firebase components
FirebaseApp app;
// FirebaseJson json;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database(databaseURL);

//Objects of the BH1750 and the DHT22 and realtime clock of ESP
// BH1750 lightMeter;
Adafruit_AS7341 lightMeter;
DHT22 dht22(DHTpin);
ESP32Time rtc;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600; //GMT+1
const int   daylightOffset_sec = 3600; //Summer time +1
bool timeInitialized = false;
const int timeInterval = 20000; //20 seconds
const int historyInterval = 1800000; //30 minutes
long currentTime = 0;
long lastHistoryTime = millis()-historyInterval;

static uint8_t wifiFailCount = 0;
static unsigned long lastWifiAttempt = 0;

const uint8_t WIFI_FAIL_LIMIT = 10;
const unsigned long WIFI_RETRY_BASE_MS = 2000;      // 2 sec base
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000; // 15 sec per attempt


void setup() {
  Serial.begin(115200);
  pinMode(yLEDpin, OUTPUT);
  pinMode(rLEDpin, OUTPUT);
  pinMode(gLEDpin, OUTPUT);
  digitalWrite(yLEDpin, LOW);
  digitalWrite(rLEDpin, LOW);
  digitalWrite(gLEDpin, LOW);
  digitalWrite(2, LOW);

  WiFiStart();
  checkTimeIsLive();

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(databaseURL);

  Wire.begin();
  lightMeter.begin();
  lightMeter.setATIME(17); 
  lightMeter.setASTEP(999);
  lightMeter.setGain(AS7341_GAIN_8X);

  /*
    AS7341_GAIN_1X: The typical starter code defaults to GAIN_256X (designed for dark indoor rooms). 
    For sunlight, keeping it there will instantly over-expose the sensor. 
    Dropping it to 1X behaves like lowering a digital camera's sensitivity down to ISO 100.
    setASTEP(999): Keeping ASTEP locked at 999 fixes your integration step interval at exactly \(2.78\text{ ms}\) per step.
    setATIME(17): Setting ATIME to 17 creates 18 integration iterations (\(17 + 1\)). 
    Mutliplied by the step interval, this yields a highly responsive \(50\text{ ms}\) window, 
    giving the clear and near-infrared (NIR) spectrum channels enough time to sample accurately without clipping.
    */

  delay(1000);
}

void loop() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    delay(50);
    return; // skip Firebase operations if offline
  }
  if(!timeInitialized) {checkTimeIsLive();}
  sync();

  if(app.ready()) {
    if (millis() - currentTime >= timeInterval) {
      currentTime = millis();
      
      //BH1750 - measuring light
      // float lux = lightMeter.readLightLevel();
      // char luxPrint[8];
      // dtostrf(lux, 5, 1, luxPrint);

      uint16_t lightReading[12];
      lightMeter.readAllChannels(lightReading);
      // Database.set<float>(aClient, "liveData/light", lux, processData);

      //DHT22 - measuring temp and humidity
      float temp = dht22.getTemperature();
      float humidity = dht22.getHumidity();

      object_t payload = JsonFileCreator(lightReading, 12, temp, humidity);
      Database.set<object_t>(aClient, "liveData", payload, processData);

      // History: write a snapshot under historyData/<epoch> every historyInterval
      if (millis() - lastHistoryTime >= historyInterval) {
        lastHistoryTime = millis();
        String pathHis = "historyData/" + String(rtc.getEpoch()+7200); //compensating for different time zones AND +1 hour for summer time

        Database.set<object_t>(aClient, pathHis, payload, processData);
      }
    }
  }
  delay(10);
}

// object_t JsonFileCreator(float lux, float temp, float humidity) {
//   object_t tempJson, humJson, luxJson, luxConvertedJson, timestamp, payload;
//   JsonWriter writer;

//   writer.create(luxJson, "/light", number_t(lux));
//   writer.create(luxConvertedJson, "/lightConverted", number_t(lux*0.0185));
//   writer.create(tempJson, "/temp", number_t(temp));
//   writer.create(humJson, "/humidity", number_t(humidity));
//   writer.create(timestamp, "/timestamp", number_t(rtc.getEpoch()));

//   writer.join(payload, 5, luxJson, luxConvertedJson, tempJson, humJson, timestamp);

//   return payload;
// }

object_t JsonFileCreator(uint16_t lightReading[], int LR_len, float temp, float humidity) {
    // Build the light array
    String json = "{\"light\":[";
    for (int i = 0; i < LR_len; i++) {
        json += lightReading[i];
        if (i < LR_len - 1) json += ",";
    }
    json += "]";

    // Append scalar values
    json += ",\"temp\":"     + String(temp, 2);
    json += ",\"humidity\":" + String(humidity, 2);
    json += ",\"timestamp\":" + String(rtc.getEpoch());
    json += "}";

    return object_t(json.c_str());
}

void WiFiReset() {
  WiFi.disconnect();
  delay(500);
  WiFi.reconnect();

  while (WiFi.status() != WL_CONNECTED) { //Wifi isn't connected - blinking slowly
    digitalWrite(yLEDpin, HIGH);
    delay(500);
    digitalWrite(yLEDpin, LOW);
    delay(500);
    Serial.print(".");
  }
}

void sync() {
  static unsigned long lastSync = 0; // Make lastSync static to preserve its value
  const unsigned long syncInterval = 3600000; // 1 hour in milliseconds

  if (millis() - lastSync >= syncInterval) {
    if (WiFi.status() == WL_CONNECTED) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      lastSync = millis();
    } 
    else {
      WiFiReset();
    }
  }
}

void checkTimeIsLive() 
{
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  int i = 0;
  while (rtc.getYear() <= 2020) { //Waits till the year is bigger then 2020, to initialize that we have time.  && i < 30
    delay(300);
    i++;

    if(i>=30)
    {
      WiFiReset();
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

      i=0;
    }
  }
  if (rtc.getYear() > 2020) {
    timeInitialized = true;
  } else {
    for(int j=0; j<30; j++){ //Time isn't updated and isn't live.
      digitalWrite(yLEDpin, HIGH);
      delay(50);
      digitalWrite(yLEDpin, LOW);
      delay(50);
    }
  }
}

bool waitForWiFi(unsigned long timeoutMs)
{
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) { //wifi isn't connected
    digitalWrite(yLEDpin, HIGH);
    delay(150);
    digitalWrite(yLEDpin, LOW);
    delay(150);
    Serial.print(".");
  }
  return (WiFi.status() == WL_CONNECTED);
}

void WiFiStart()
{
  WiFi.mode(WIFI_STA);

  WiFi.persistent(false);       // do not write credentials to flash repeatedly
  WiFi.setAutoReconnect(true);  // allow auto reconnect
  WiFi.setSleep(false);         // IMPORTANT: reduce long-run disconnect issues

  // Optional if the signal is weak:
  // WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  if (!waitForWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("\nWiFi connect timed out.");
  } else {
    Serial.print("\nConnected. IP: ");
    Serial.println(WiFi.localIP());
    wifiFailCount = 0;
  }
}

void WiFiFullRestart()
{
  Serial.println("\n[WiFi] Full restart...");

  WiFi.disconnect(true, true);   // erase old state + disconnect
  delay(500);

  WiFi.mode(WIFI_OFF);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

void ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  // Exponential backoff: 2s, 4s, 8s, 16s, 32s...
  unsigned long backoff = WIFI_RETRY_BASE_MS << min(wifiFailCount, (uint8_t)5);

  if (millis() - lastWifiAttempt < backoff) return;
  lastWifiAttempt = millis();

  Serial.print("\n[WiFi] Disconnected. Reconnect attempt ");
  Serial.println(wifiFailCount + 1);

  // 1) Try a simple reconnect first
  WiFi.reconnect();
  if (waitForWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("\n[WiFi] Reconnected.");
    wifiFailCount = 0;
    return;
  }

  // 2) If reconnect fails, restart WiFi stack every few tries
  if ((wifiFailCount % 3) == 2) {
    WiFiFullRestart();
    if (waitForWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
      Serial.println("\n[WiFi] Reconnected after full restart.");
      wifiFailCount = 0;
      return;
    }
  }

  wifiFailCount++;

  // 3) Give up after too many failures (cleanest recovery)
  if (wifiFailCount >= WIFI_FAIL_LIMIT) {
    Serial.println("[WiFi] Too many failures → restarting ESP32.");
    delay(1000);
    ESP.restart();
  }
}


void processData(AsyncResult &aResult) {

static int firebaseErrorCount = 0;
const int FIREBASE_ERROR_LIMIT = 10;
  if(!aResult.isResult())
    return;

  if(aResult.isEvent())
    Firebase.printf("Event task %s: %s, msg: %s, code: %d\n", rtc.getTime().c_str(), aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());

  if (aResult.isDebug())
    Firebase.printf("Debug task %s: %s, msg: %s\n", rtc.getTime().c_str(), aResult.uid().c_str(), aResult.debug().c_str());

  if (aResult.isError()) {
    Firebase.printf("Error task %s: %s, msg: %s, code: %d\n", rtc.getTime().c_str(), aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    firebaseErrorCount++;

    // If Firebase keeps failing, restart ESP32
    if (firebaseErrorCount >= FIREBASE_ERROR_LIMIT) {
      Serial.println("Firebase stuck — restarting ESP32");
      delay(1000);
      ESP.restart();
    }
  } 
  else {
    // Success → reset error counter
    firebaseErrorCount = 0;
  }

  if (aResult.available())
    Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
}