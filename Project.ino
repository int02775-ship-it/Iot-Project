
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <ThingerESP32.h>

// ========== ΠΙΝΑΚΕΣ ==========
#define LCD_ADDR 0x27
#define I2C_SDA 8
#define I2C_SCL 9
#define DHTPIN 4
#define LED_PIN 2
#define MOTION_PIN 13
#define DHTTYPE DHT11

// WiFi & Thinger
const char* WIFI_SSID = "free wifi";
const char* WIFI_PASS = "FiatPanda2015";
const char* THINGER_USER = "Mpalis";
const char* THINGER_DEVICE = "esp32c6_sensor";
const char* THINGER_SECRET = "6975542731Mpalis";

// ========== ΧΡΟΝΙΚΑ ΔΙΑΣΤΗΜΑΤΑ ==========
#define SENSOR_INTERVAL 2000      // 2 δευτερόλεπτα για live data
#define HUMIDITY_INTERVAL 60000   // 1 λεπτό
#define FIVE_MIN_INTERVAL 300000  // 5 λεπτά 
#define HOURLY_INTERVAL 3600000   // 1 ώρα 

// ========== ΑΝΤΙΚΕΙΜΕΝΑ ==========
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
DHT dht(DHTPIN, DHTTYPE);
ThingerESP32 thing(THINGER_USER, THINGER_DEVICE, THINGER_SECRET);

// ========== ΜΕΤΑΒΛΗΤΕΣ ==========
float temp = 0, hum = 0;
bool wifiOK = false, thingerOK = false;
bool motionDetected = false;
bool motionSensorEnabled = true;

// Χρονόμετρα
unsigned long lastRead = 0;
unsigned long lastHumSend = 0;
unsigned long last5MinSend = 0;
unsigned long lastHourSend = 0;

// Buffers για υπολογισμούς
static float buffer5min[60];
static float bufferHourly[24];
static bool motionHistory[60];

uint8_t idx5min = 0, idxHourly = 0, idxMotion = 0;
uint8_t count5min = 0, countHourly = 0;

// Τελευταίες τιμές
float avg5min = 0, avgHourly = 0, lastHumSent = 0;

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(MOTION_PIN, INPUT);
  
  // ========== ΑΡΧΙΚΑ ΜΗΝΥΜΑΤΑ ==========
  lcd.setCursor(0, 0);
  lcd.print("IoT project");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1000);
  
  // ========== WIFI CONNECTION ==========
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(300);
    attempts++;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  
  digitalWrite(LED_PIN, LOW);
  wifiOK = (WiFi.status() == WL_CONNECTED);
  
  if (wifiOK) {
    lcd.setCursor(0, 1);
    lcd.print("Success       ");
    delay(800);
    
    // ========== THINGER.IO SETUP ==========
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Thinger.io...");
    delay(500);
    
    thing.add_wifi(WIFI_SSID, WIFI_PASS);
    
    // 1. LIVE DATA
    thing["environment"] >> [](pson& out) {
      out["temperature"] = temp;
      out["humidity"] = hum;
    };
    
    // 2. 5-MIN AVERAGE
    thing["5min_avg"] >> [](pson& out) {
      out["avg_temperature"] = avg5min;
    };
    
    // 3. HOURLY AVERAGE
    thing["hourly_avg"] >> [](pson& out) {
      out["avg_temperature"] = avgHourly;
    };
    
    // 4. HUMIDITY
    thing["humidity"] >> [](pson& out) {
      out["humidity"] = lastHumSent;
    };
    
    // 5. LED CONTROL
    thing["led"] << [](pson& in) {
      if (in.is_empty()) {
        in = (bool)digitalRead(LED_PIN);
      } else {
        digitalWrite(LED_PIN, in ? HIGH : LOW);
      }
    };
    
    // 6. MOTION SENSOR
    thing["motion"] >> [](pson& out) {
      out["detected"] = motionDetected;
      out["enabled"] = motionSensorEnabled;
    };
    
    // 7. MOTION CONTROL
    thing["motion_control"] << [](pson& in) {
      if (in.is_empty()) {
        in = motionSensorEnabled;
      } else {
        motionSensorEnabled = in;
      }
    };
    
    // 8. MOTION HISTORY
    thing["motion_history"] >> [](pson& out) {
      out["detections_last_hour"] = getMotionDetectionsLastHour();
    };
    
    // 9. MOTION LED
    thing["motion_led"] >> [](pson& out) {
      out = motionDetected;
    };
    
    thingerOK = true;
    lcd.setCursor(0, 1);
    lcd.print("OK            ");
    delay(800);
  } else {
    lcd.setCursor(0, 1);
    lcd.print("Failed        ");
    delay(1000);
  }
  
  dht.begin();
  
  // Αρχικοποίηση χρονομέτρων
  unsigned long now = millis();
  lastRead = now;
  lastHumSend = now;
  last5MinSend = now;
  lastHourSend = now;
  
  lcd.clear();
}

// ========== LOOP ==========
void loop() {
  unsigned long now = millis();
  
  // ===== 1. LIVE DATA =====
  if (now - lastRead >= SENSOR_INTERVAL) {
    lastRead = now;
    
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    if (!isnan(t) && !isnan(h)) {
      temp = t;
      hum = h;
      
      // Προσθήκη στο buffer 5 λεπτών
      static unsigned long last5minSample = 0;
      if (now - last5minSample >= 5000) {
        last5minSample = now;
        buffer5min[idx5min] = temp;
        idx5min = (idx5min + 1) % 60;
        if (count5min < 60) count5min++;
      }
      
      // Έλεγχος αισθητήρα κίνησης
      if (motionSensorEnabled) {
        bool currentMotion = (digitalRead(MOTION_PIN) == HIGH);
        if (currentMotion) {
          motionDetected = true;
          motionHistory[idxMotion] = true;
          
          if (wifiOK && thingerOK) {
            thing.stream(thing["motion_led"]);
          }
        } else {
          motionDetected = false;
          static unsigned long lastMotionClear = 0;
          if (now - lastMotionClear >= 2000) {
            lastMotionClear = now;
            if (wifiOK && thingerOK) {
              thing.stream(thing["motion_led"]);
            }
          }
        }
      }
      
      // ===== LCD DISPLAY - ΣΤΑΘΕΡΕΣ ΘΕΣΕΙΣ =====
      // Γραμμή 1: T:XX.XC H:XX% (σταθερές θέσεις)
      lcd.setCursor(0, 0);
      lcd.print("T:");
      lcd.print(temp, 1);
      lcd.print("C");
      
      lcd.setCursor(8, 0);
      lcd.print("H:");
      lcd.print(hum, 0);
      lcd.print("%   ");  // Κενά για καθαρισμό
      
      // Γραμμή 2: W:X L:X M:X (σταθερές θέσεις)
      lcd.setCursor(0, 1);
      lcd.print("W:");
      lcd.print(wifiOK ? "Y" : "N");
      
      lcd.setCursor(4, 1);
      lcd.print("L:");
      lcd.print(digitalRead(LED_PIN) ? "On " : "Off");
      
      lcd.setCursor(10, 1);
      lcd.print("M:");
      lcd.print(motionDetected ? "Yes" : "No ");
      
      // Στέλνει live data
      if (wifiOK && thingerOK) {
        thing.stream(thing["environment"]);
        thing.stream(thing["motion"]);
      }
    }
  }
  
  // ===== 2. HUMIDITY =====
  if (now - lastHumSend >= HUMIDITY_INTERVAL) {
    lastHumSend = now;
    lastHumSent = hum;
    
    if (wifiOK && thingerOK) {
      thing.write_bucket("30min_humidity", "humidity");
    }
  }
  
  // ===== 3. 5-MIN AVERAGE =====
  if (now - last5MinSend >= FIVE_MIN_INTERVAL) {
    last5MinSend = now;
    
    if (count5min > 0) {
      float sum = 0;
      for (uint8_t i = 0; i < count5min; i++) {
        sum += buffer5min[i];
      }
      avg5min = sum / count5min;
      
      if (wifiOK && thingerOK) {
        thing.write_bucket("5min_averages", "5min_avg");
      }
      
      bufferHourly[idxHourly] = avg5min;
      idxHourly = (idxHourly + 1) % 24;
      if (countHourly < 24) countHourly++;
      
      count5min = 0;
      idx5min = 0;
    }
  }
  
  // ===== 4. HOURLY AVERAGE =====
  if (now - lastHourSend >= HOURLY_INTERVAL) {
    lastHourSend = now;
    
    if (countHourly > 0) {
      float sum = 0;
      for (uint8_t i = 0; i < countHourly; i++) {
        sum += bufferHourly[i];
      }
      avgHourly = sum / countHourly;
      
      if (wifiOK && thingerOK) {
        thing.write_bucket("24h_temperature", "hourly_avg");
      }
      
      idxMotion = (idxMotion + 1) % 60;
      motionHistory[idxMotion] = false;
    }
  }
  
  // ===== 5. THINGER HANDLE =====
  if (wifiOK && thingerOK) {
    thing.handle();
  }
  
  // ===== 6. WIFI CHECK =====
  static unsigned long lastWiFiCheck = 0;
  if (now - lastWiFiCheck >= 30000) {
    lastWiFiCheck = now;
    
    if (WiFi.status() != WL_CONNECTED) {
      wifiOK = false;
      thingerOK = false;
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      
      uint8_t attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        delay(500);
        attempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        wifiOK = true;
        thingerOK = true;
      }
    }
  }
  
  delay(10);
}

// ========== ΒΟΗΘΗΤΙΚΗ ΣΥΝΑΡΤΗΣΗ ==========
uint8_t getMotionDetectionsLastHour() {
  uint8_t detections = 0;
  for (uint8_t i = 0; i < 60; i++) {
    if (motionHistory[i]) {
      detections++;
    }
  }
  return detections;
}