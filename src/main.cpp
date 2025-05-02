#include <WiFi.h>              // Library to connect and manage Wi-Fi functionality on ESP32.
#include "ThingSpeak.h"        // Library to interact with ThingSpeak API for uploading sensor data to the cloud.
#include <DHT.h>               // Library to interact with DHT sensors (e.g., DHT11/DHT22) for temperature and humidity readings.
#include "MAX30105.h"          // Library to interact with MAX30105 sensor (used for heart rate and SpO2 measurements).
#include <Wire.h>              // Library to communicate with I2C devices (such as the MAX30105) over I2C bus.
#include <LiquidCrystal_I2C.h> // Library to control LCD displays over I2C interface (used for showing real-time sensor data).
#include "heartRate.h"         // External library (or code) to compute heart rate from MAX30105 sensor data using Maxim's algorithm.
#include "spo2_algorithm.h"    // External library (or code) to compute SpO2 (blood oxygen saturation) using MAX30105 sensor data.
#include <freertos/FreeRTOS.h> // FreeRTOS library to manage tasks, semaphores, and other real-time operating system features on ESP32.
#include <freertos/task.h>     // Provides task-related functionality for FreeRTOS, such as creating and managing tasks.
#include <freertos/semphr.h>   // Provides semaphore-related functionality for synchronizing access to shared resources in FreeRTOS.

// Explicitly define I2C_BUFFER_LENGTH to avoid redefinition warning
#define I2C_BUFFER_LENGTH 128

// ---- DHT22 Config ----
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---- MAX30102 Config ----
MAX30105 particleSensor;

// ---- LCD Config ----
LiquidCrystal_I2C lcd(0x27, 16, 2); // Change to 0x3F if needed

// ---- AD8232 ECG Config ----
#define ECG_PIN 34 // ADC1_CH6 (GPIO34)
#define LO_PLUS_PIN 32
#define LO_MINUS_PIN 33

// ---- Shared Data Structure ----
struct SensorData
{
  float bpm; // Now stores Maxim BPM
  int32_t spo2;
  int32_t ecgValue;
  float ecgBpm;
  float temperature;
  float humidity;
  bool spo2_valid;
  bool hr_valid;
};

// ---- Global Variables ----
SensorData sensorData = {0, -999, 0, 0, 0, 0, false, false};
SemaphoreHandle_t dataMutex;

// ---- MAX30102 Buffers ----
#define BUFFER_SIZE 100 // Matches Maxim algorithm requirements
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int bufferIndex = 0;
SemaphoreHandle_t bufferMutex;

// ---- AD8232 Heartbeat Variables ----
unsigned long lastEcgBeat = 0;
const int ecgThreshold = 600; // Adjust this threshold to reduce noise

// Wi-Fi configuration
const char *ssid = "mohin";           // Replace with your Wi-Fi SSID
const char *password = "jioairfiber"; // Replace with your Wi-Fi password

// ThingSpeak channel configuration
unsigned long channelID = 2938936;       // Replace with your ThingSpeak channel ID
const char *apiKey = "VLQSMNBV0G4AD7JW"; // Replace with your ThingSpeak write API key

WiFiClient client; // Create a WiFi client to send data to ThingSpeak

// Function prototypes for FreeRTOS tasks
void max30102Task(void *pvParameters);
void dht22Task(void *pvParameters);
void ecgTask(void *pvParameters);
void lcdTask(void *pvParameters);
void thingSpeakTask(void *pvParameters);

// Function to initialize sensors
void initializeSensors()
{
  // --- MAX30102 Setup ---
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD))
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sensor Error");
    Serial.println("MAX30102 not found. Check wiring.");
    while (1)
      ;
  }
  particleSensor.setup(0x3F, 4, 2, 100, 411, 16384);
  particleSensor.setPulseAmplitudeRed(0x3F);
  particleSensor.setPulseAmplitudeIR(0x3F);

  // --- DHT22 Setup ---
  dht.begin();

  // --- AD8232 Setup ---
  pinMode(ECG_PIN, INPUT);
  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);
}

// Function to connect to Wi-Fi with timeout
bool connectToWiFi()
{
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  { // 20 attempts, 1 second each
    delay(1000);
    Serial.println("Connecting to WiFi...");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Connected to WiFi");
    return true;
  }
  else
  {
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

// Function to filter and smooth BPM readings (Moving Average)
#define FILTER_SIZE 5
int bpmHistory[FILTER_SIZE] = {0}; // Holds the last 5 BPM values
int bpmIndex = 0;

int applyBpmFilter(int newBpm)
{
  bpmHistory[bpmIndex] = newBpm;
  bpmIndex = (bpmIndex + 1) % FILTER_SIZE;

  int sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++)
  {
    sum += bpmHistory[i];
  }

  return sum / FILTER_SIZE;
}

void setup()
{
  Serial.begin(115200);
  Wire.begin();

  // Create mutexes
  dataMutex = xSemaphoreCreateMutex();
  bufferMutex = xSemaphoreCreateMutex();

  // --- LCD Setup ---
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  initializeSensors();
  lcd.clear();

  // --- Wi-Fi Setup ---
  if (!connectToWiFi())
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Error");
    while (1)
      ;
  }

  // --- ThingSpeak Setup ---
  ThingSpeak.begin(client);

  // --- Create FreeRTOS Tasks ---
  xTaskCreatePinnedToCore(max30102Task, "MAX30102 Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(dht22Task, "DHT22 Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ecgTask, "ECG Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(lcdTask, "LCD Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(thingSpeakTask, "ThingSpeak Task", 4096, NULL, 1, NULL, 0);
}

void max30102Task(void *pvParameters)
{
  while (1)
  {
    // --- MAX30102 Sensor Readings ---
    uint32_t irValue = particleSensor.getIR();
    uint32_t redValue = particleSensor.getRed();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      if (irValue < 5000)
      {
        Serial.println("No finger detected (IR < 5000)");
        sensorData.bpm = 0;
        sensorData.spo2 = -999;
        sensorData.spo2_valid = false;
        sensorData.hr_valid = false;
        xSemaphoreGive(dataMutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      xSemaphoreGive(dataMutex);
    }

    // Save to buffer
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      irBuffer[bufferIndex] = irValue;
      redBuffer[bufferIndex] = redValue;
      bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
      xSemaphoreGive(bufferMutex);
    }

    // --- Maxim's Algorithm for BPM and SpO2 ---
    if (bufferIndex == 0)
    { // Buffer is full, process BPM and SpO2
      int32_t spo2 = -999;
      int8_t spo2_valid = 0;
      int32_t heart_rate = -999;
      int8_t hr_valid = 0;

      if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_SIZE, redBuffer, &spo2, &spo2_valid, &heart_rate, &hr_valid);
        xSemaphoreGive(bufferMutex);
      }

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        sensorData.spo2 = spo2;
        sensorData.spo2_valid = spo2_valid;
        sensorData.hr_valid = hr_valid;
        if (hr_valid)
        {
          // Apply filter to smooth BPM value
          int filteredBpm = applyBpmFilter(heart_rate);
          sensorData.bpm = filteredBpm;
          Serial.println("=== Heart Rate ===");
          Serial.print("Maxim BPM: ");
          Serial.println(filteredBpm);
        }
        else
        {
          sensorData.bpm = 0; // Clear BPM if invalid
          Serial.println("Invalid Maxim BPM");
        }
        Serial.println("=== SpO2 ===");
        if (spo2_valid)
        {
          Serial.print("SpO2 (%): ");
          Serial.println(spo2);
        }
        else
        {
          Serial.println("Invalid SpO2 from Maxim algorithm");
        }
        xSemaphoreGive(dataMutex);
      }
      bufferIndex = 0; // Reset buffer index
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // 100Hz sampling (10ms delay)
  }
}

void dht22Task(void *pvParameters)
{
  while (1)
  {
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      sensorData.temperature = temperature;
      sensorData.humidity = humidity;
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(2000)); // Read every 2 seconds
  }
}

void ecgTask(void *pvParameters)
{
  while (1)
  {
    int ecgValue = analogRead(ECG_PIN);
    unsigned long now = millis();

    if (ecgValue > ecgThreshold && (now - lastEcgBeat > 300))
    {
      float newEcgBpm = 60000.0 / (now - lastEcgBeat);
      lastEcgBeat = now;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        sensorData.ecgValue = ecgValue;
        sensorData.ecgBpm = newEcgBpm;
        xSemaphoreGive(dataMutex);
      }
    }

    // Always print raw ECG value for plotting
    Serial.println(ecgValue); // <--- THIS LINE IS CRUCIAL

    vTaskDelay(pdMS_TO_TICKS(10)); // 100Hz sampling
  }
}

void lcdTask(void *pvParameters)
{
  static int displayState = 0;
  SensorData lastDisplayedData = {0, -999, 0, 0, 0, 0, false, false};
  while (1)
  {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      if (memcmp(&sensorData, &lastDisplayedData, sizeof(SensorData)) != 0)
      { // Only update if data has changed
        lcd.clear();
        lcd.setCursor(0, 0);
        switch (displayState)
        {
        case 0:
          if (sensorData.hr_valid && sensorData.bpm > 0)
          {
            lcd.print("BPM: ");
            lcd.print((int)sensorData.bpm);
          }
          else
          {
            lcd.print("BPM: N/A");
          }
          break;
        case 1:
          if (sensorData.spo2_valid && sensorData.spo2 >= 0 && sensorData.spo2 <= 100)
          {
            lcd.print("SpO2: ");
            lcd.print(sensorData.spo2);
            lcd.print("%");
          }
          else
          {
            lcd.print("SpO2: N/A");
          }
          break;
        case 2:
          if (!isnan(sensorData.temperature))
          {
            lcd.print("Temp: ");
            lcd.print(sensorData.temperature, 1);
            lcd.print((char)223);
            lcd.print("C");
          }
          else
          {
            lcd.print("Temp: N/A");
          }
          break;
        case 3:
          if (!isnan(sensorData.humidity))
          {
            lcd.print("Hum: ");
            lcd.print(sensorData.humidity, 0);
            lcd.print("%");
          }
          else
          {
            lcd.print("Hum: N/A");
          }
          break;
        }
        displayState = (displayState + 1) % 4; // Rotate through 4 data types
        lastDisplayedData = sensorData;        // Save last displayed data
      }
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Update display every 1 second
  }
}

void thingSpeakTask(void *pvParameters)
{
  while (1)
  {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      ThingSpeak.setField(1, sensorData.bpm);
      ThingSpeak.setField(2, sensorData.spo2);
      ThingSpeak.setField(3, sensorData.temperature);
      ThingSpeak.setField(4, sensorData.humidity);

      long responseCode = ThingSpeak.writeFields(channelID, apiKey);
      if (responseCode == 200)
      {
        Serial.println("Data uploaded successfully.");
      }
      else
      {
        Serial.print("Failed to upload data. Response code: ");
        Serial.println(responseCode);
      }
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(15000)); // Upload data every 15 seconds
  }
}

void loop()
{
  // Empty as FreeRTOS tasks handle everything
}
