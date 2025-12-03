// FOR REFERENCE AND GRADING

#include <Arduino.h>
#include <SparkFunLSM6DSO.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "Wire.h"
#include "DHT20.h"
#include <ArduinoJson.h>
#include <string>

// Wi-Fi credentials
#define WIFI_SSID "" // NOTE: Please delete this value before submitting assignment
#define WIFI_PASSWORD "" // NOTE: Please delete this value before submitting assignment

// Azure IoT Hub configuration - delete before submission
#define SAS_TOKEN ""
// Root CA certificate for Azure IoT Hub - delete before submission
const char* root_ca = "";


String iothubName = "g65"; //Your hub name (replace if needed)
String deviceName = "lilygo"; //Your device name (replace if needed)
String url = "https://" + iothubName + ".azure-devices.net/devices/" + deviceName + "/messages/events?api-version=2021-04-12";

// Telemetry interval
#define TELEMETRY_INTERVAL 500  // Send data every half seconds

uint32_t lastTelemetryTime = 0;

// initialize global IMU object
LSM6DSO myIMU; 

// ESP32 pin definitions
#define BUZZER_PIN 32
#define GREEN_BUTTON_PIN 26
#define RED_BUTTON_PIN 27
int ledPin = 25; // LED pin

int sState; // system state
unsigned long sTimer; // system timer
int sendState; // more human timed
#define READ_SENSOR_STATE 1
#define FALL_DETECTED_STATE 2
#define WAITING_USER_RESPONSE 3
#define SEND_ALERT 4
#define TEMP 5
#define RESUME 6

// initialize accelerometer and gyroscope offsets
float ax_offset = 0, ay_offset = 0, az_offset = 0;
float gx_offset = 0, gy_offset = 0, gz_offset = 0;

float acc_vec = 0;
float gyro_vec = 0;

// reads the raw acceleration value across each access
void measure_accel(float *ax, float *ay, float *az) {
  *ax = myIMU.readFloatAccelX();
  *ay = myIMU.readFloatAccelY();
  *az = myIMU.readFloatAccelZ();
}

// reads the raw angular velocity values across each access
void measure_gyro(float *gx, float *gy, float *gz) {
  *gx = myIMU.readFloatGyroX();
  *gy = myIMU.readFloatGyroY();
  *gz = myIMU.readFloatGyroZ();
}

// computes the combined acceleration and gyroscopic vector length
float AGV(float ax, float ay, float az, float gx, float gy, float gz) {
  float a = sqrt((ax * ax) + (ay * ay) + (az * az));
  acc_vec = a;
  float g = sqrt((gx * gx) + (gy * gy) + (gz * gz));
  gyro_vec = g;
  return a + g;
}


/*
Finite-State Machine
1) Reading sensor data (continues in state 1 until fall is detected, then transitions to state 2)
2) Fall detected (triggers buzzer and transitions to state 3)
3) Waiting user response (LED signals that user response is needed), Reads user response (green means to send alert, and red means go to TEMP state)
4) Sends alert (if green button is pressed or 30 seconds has passed and nothing was pressed, send alert to the cloud)
5) In TEMP state, repeat WAITING_USER_RESPONSE, but if red button is pressed again, go to RESUME
6) Resumes (back to State 1)
*/

void setup() {

  Wire.begin();
  Serial.begin(9600);

  delay(1000);
  Serial.println("Fall Detection System Starting!");

  // LSM6DSO
  // checks I2C connection with LSM6DSO
  if (!myIMU.begin(0x6B, Wire)) {
    Serial.println("Error connecting LSM6DSO sensor!");
  } else {
    Serial.println("LSM6DSO sensor connected successfully!");
  }

  // checks whether sensor actually measures things
  if (!myIMU.initialize(BASIC_SETTINGS)) {
    Serial.println("Error loading sensor!");
  } else {
    Serial.println("LSM6DSO sensor loaded successfully!");
  }

  // calibration (average of 10 readings)
  Serial.println("Measuring background acc...");
  float ax, ay, az;
  for (int i = 0; i < 10; i++) {
    measure_accel(&ax, &ay, &az);
    ax_offset += ax;
    ay_offset += ay;
    az_offset += az;
  }
  ax_offset = ax_offset / 10;
  ay_offset = ay_offset / 10;
  az_offset = az_offset / 10;

  Serial.println("Background acc are: ");
  Serial.println(ax_offset, 2);
  Serial.println(ay_offset, 2);
  Serial.println(az_offset, 2);

  Serial.println("Measuring background gyro...");
  float gx, gy, gz;
  for (int i = 0; i < 10; i++) {
    measure_gyro(&gx, &gy, &gz);
    gx_offset += gx;
    gy_offset += gy;
    gz_offset += gz;
  }
  gx_offset /= 10;
  gy_offset /= 10;
  gz_offset /= 10;

  Serial.println("Background gyro are: ");
  Serial.println(gx_offset, 2);
  Serial.println(gy_offset, 2);
  Serial.println(gz_offset, 2);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(GREEN_BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(RED_BUTTON_PIN, INPUT_PULLDOWN);
  sState = READ_SENSOR_STATE;
  sendState = READ_SENSOR_STATE;
  // Wifi
  delay(500);
  Serial.println("\nInitializing WiFi...");

  WiFi.mode(WIFI_STA);
  delay(1000);
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      Serial.print(WiFi.status());
  }
  Serial.println("\nWiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println("MAC address: ");
  Serial.println(WiFi.macAddress());
}

int fall_counter = 0; // initialize fall counter
bool aboveThreshold = false; // tracks whether current agv value is above threshold (a fall is only counted when aboveThreshold is triggered from false to true)

// to decrease false positives (violently shaking the board will not trigger the fall counter)
unsigned long free_fall_stamp = 0; // records when free fall (very low acceleration) was last detected
unsigned long window = 150; // a fall is only counted when AGV spike happens within 150ms after free fall

// defines fall detection threshold
float agv_threshold = 500;

#define USER_MILLIS 5000 // user must respond within 30 seconds

bool buzzer_sounding = false;
unsigned long buzzTime = 0; // initialize time buzzer starts
unsigned long waiting_time = 0; // records time before the user response waiting


void loop() {
    switch (sState) {
    case READ_SENSOR_STATE: {
      // continuously read in data
      float ax, ay, az;
      measure_accel(&ax, &ay, &az);
      float ax_temp = ax - ax_offset;
      float ay_temp = ay - ay_offset;
      float az_temp = az - az_offset;
      ax = abs(ax_temp);
      ay = abs(ay_temp);
      az = abs(az_temp);

      float gx, gy, gz;
      measure_gyro(&gx, &gy, &gz);
      float gx_temp = gx - gx_offset;
      float gy_temp = gy - gy_offset;
      float gz_temp = gz - gz_offset;
      gx = abs(gx_temp);
      gy = abs(gy_temp);
      gz = abs(gz_temp);

      float agv = AGV(ax, ay, az, gx, gy, gz);
      // calculates the magnitude of the acceleration vector
      float a = sqrt((ax * ax) + (ay * ay) + (az * az));
      bool lowAccel = (a < 1.0); // records the time acceleration was low
      if (lowAccel) {
        free_fall_stamp = millis();
      }
      bool after_free_fall = (millis() - free_fall_stamp) < window; // track whether free fall happened within the defined window
      
      if (aboveThreshold == false) {
        if ((agv >= agv_threshold) && after_free_fall) {
          aboveThreshold = true;
          fall_counter++;
          Serial.println(String("Fall Detected! Count: ") + fall_counter);
          sState = FALL_DETECTED_STATE;
          if (sendState != SEND_ALERT) {
            sendState = FALL_DETECTED_STATE;
          }
        }
      } else if ((agv < agv_threshold)) {
        aboveThreshold = false;
      }
      break;
    }

    case FALL_DETECTED_STATE: {
      // turn on buzzer
      if (!buzzer_sounding) {
        tone(BUZZER_PIN, 150);
        buzzer_sounding = true;
        buzzTime = millis();
      }
      if ((buzzer_sounding) && (millis() - buzzTime >= 1000)) { // buzz for 1 second
        noTone(BUZZER_PIN);
        buzzer_sounding = false;
        waiting_time = millis();
        sState = WAITING_USER_RESPONSE;
      }
      break;
    }

    case WAITING_USER_RESPONSE: { // LED on (connect longer leg to 330 ohm resistor)
      float ax, ay, az;
      measure_accel(&ax, &ay, &az);
      float ax_temp = ax - ax_offset;
      float ay_temp = ay - ay_offset;
      float az_temp = az - az_offset;
      ax = abs(ax_temp);
      ay = abs(ay_temp);
      az = abs(az_temp);

      float gx, gy, gz;
      measure_gyro(&gx, &gy, &gz);
      float gx_temp = gx - gx_offset;
      float gy_temp = gy - gy_offset;
      float gz_temp = gz - gz_offset;
      gx = abs(gx_temp);
      gy = abs(gy_temp);
      gz = abs(gz_temp);

      float agv = AGV(ax, ay, az, gx, gy, gz);
      if (millis() - waiting_time <= USER_MILLIS) {
        digitalWrite(ledPin, HIGH);

        if (digitalRead(GREEN_BUTTON_PIN)) {
          digitalWrite(ledPin, LOW);
          sState = SEND_ALERT;
          sendState = SEND_ALERT;
          break;
        }

        if (digitalRead(RED_BUTTON_PIN)) {
          digitalWrite(ledPin, LOW);
          delay(500); // sending data takes some time
          waiting_time = millis(); // restart window for temp
          sState = TEMP;
          if (sendState != SEND_ALERT) {
            sendState = TEMP;
          }
          break;
        }
      } else {
        digitalWrite(ledPin, LOW);
        sState = SEND_ALERT;
        sendState = SEND_ALERT;
        break;
      }
      break;
    }

    case TEMP: {
      float ax, ay, az;
      measure_accel(&ax, &ay, &az);
      float ax_temp = ax - ax_offset;
      float ay_temp = ay - ay_offset;
      float az_temp = az - az_offset;
      ax = abs(ax_temp);
      ay = abs(ay_temp);
      az = abs(az_temp);

      float gx, gy, gz;
      measure_gyro(&gx, &gy, &gz);
      float gx_temp = gx - gx_offset;
      float gy_temp = gy - gy_offset;
      float gz_temp = gz - gz_offset;
      gx = abs(gx_temp);
      gy = abs(gy_temp);
      gz = abs(gz_temp);

      float agv = AGV(ax, ay, az, gx, gy, gz);
      if (millis() - waiting_time <= USER_MILLIS) {
        digitalWrite(ledPin, HIGH);

        if (digitalRead(GREEN_BUTTON_PIN)) {
          digitalWrite(ledPin, LOW);
          sState = SEND_ALERT;
          sendState = SEND_ALERT;
          break;
        }

        if (digitalRead(RED_BUTTON_PIN)) {
          digitalWrite(ledPin, LOW);
          sState = RESUME;
          if (sendState != SEND_ALERT) {
            sendState = READ_SENSOR_STATE;
          }
          break;
        }
      } else {
        digitalWrite(ledPin, LOW);
        sState = SEND_ALERT;
        sendState = SEND_ALERT;
        break;
      }
      break;
    }

    case SEND_ALERT: {
      Serial.println("ALERT!");
      delay(200);
      sState = RESUME;
      if (sendState != SEND_ALERT) {
        sendState = READ_SENSOR_STATE;
      }
      break;
    }

    case RESUME: {
      aboveThreshold = false;
      free_fall_stamp = 0;
      waiting_time = 0;
      pinMode(GREEN_BUTTON_PIN, INPUT_PULLDOWN);
      pinMode(RED_BUTTON_PIN, INPUT_PULLDOWN);
      sState = READ_SENSOR_STATE;
      break;
    }
  }

  // send data via wifi
  if (millis() - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    ArduinoJson::JsonDocument doc;
    doc["MessageDate"] = ((float) millis())/1000;
    doc["Acceleration"] = acc_vec;
    doc["Gyroscopic"] = gyro_vec;
    doc["Count"] = fall_counter;
    switch (sendState) {
      case FALL_DETECTED_STATE:
        doc["FallState"] = "detected";
        break;
      case TEMP:
        doc["FallState"] = "canceled";
        break;
      case SEND_ALERT:
        doc["FallState"] = "alert";
        sendState = READ_SENSOR_STATE;
        break;
      default:
        doc["FallState"] = "ok";
        break;
    }
    char buffer[256];
    serializeJson(doc, buffer, sizeof(buffer));

    // send telemetry via https
    WiFiClientSecure client;
    client.setCACert(root_ca); // Set root CA certificate
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", SAS_TOKEN);

    int httpCode = http.POST(buffer);
    if (httpCode == 204) {  // IoT Hub returns 204 No Content for successful telemetry
      Serial.println("Telemetry sent: " + String(buffer));
    } else {
      Serial.println("Failed to send telemetry. HTTP code: " + String(httpCode));
    }

    http.end();
    lastTelemetryTime = millis();
  }
}