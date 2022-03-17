// REQUIRES the following Arduino libraries:
// - DHT Sensor Library: https://github.com/adafruit/DHT-sensor-library
// - Adafruit Unified Sensor Lib: https://github.com/adafruit/Adafruit_Sensor

#include "DHT.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

#define UDP_PORT 4210
#define SLEEP_SECONDS 300

// from https://www.amazon.com/gp/product/B010O1G1ES/ref=ppx_yo_dt_b_asin_title_o00_s00?ie=UTF8&th=1
static const uint8_t D0 = 16;
static const uint8_t D1 = 5;
static const uint8_t D2 = 4;
static const uint8_t D3 = 0;
static const uint8_t D4 = 2;
static const uint8_t D5 = 14;
static const uint8_t D6 = 12;
static const uint8_t D7 = 13;
static const uint8_t D8 = 15;
static const uint8_t D9 = 3;
static const uint8_t D10 = 1;

#define DEVICE_ID 5

#define DHTPIN D2     // Digital pin connected to the DHT sensor
// Feather HUZZAH ESP8266 note: use pins 3, 4, 5, 12, 13 or 14 --
// Pin 15 can work but DHT must be disconnected during program upload.

#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
#define CONFIG_INPUT_TIMEOUT_SECONDS 30

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP UDP;

struct Config {
  char magic[5];
  char ssid[64];
  char password[64];
  int reporting_interval_seconds;
  char destination_addr[32];
  int destination_port;
  char device_id[64];
  bool deep_sleep;
};

Config config;

void print_configuration(const Config &c) {
  char buf[512];
  sprintf(buf, "===Configuration===\r\nDevice ID [%s]\r\nSSID [%s]\r\nReporting Interval (seconds) [%d]\r\nDestination Address [%s:%d]\r\nDeep Sleep [%s]",
    c.device_id,
    c.ssid,
    c.reporting_interval_seconds,
    c.destination_addr,
    c.destination_port,
    c.deep_sleep?"Y":"N");
  Serial.println(buf);
}

bool get_int(int *dest, char *descr) {
  begin_get_int:
  char buf[128];
  sprintf(buf, "%s [%d]: ", descr, *dest);
  Serial.print(buf);
  bool done = false;
  bool gotchar = false;
  int mult = 1;
  int val = 0;
  
  while(!done) {
    if(!Serial.available()) { delay(10); continue; }
    int c = Serial.read();
    if(c=='\n' || c=='\r') {
      Serial.println((char)c);
      if(!gotchar) {
        // User hit enter. Just keep current value.
        return true;
      }
      done = true;
    } else if(!gotchar && c == '-') {
      mult = -1; 
      gotchar = true;
      Serial.print((char)c);
    } else if (c >= '0' && c <= '9') {
      gotchar = true;
      int newval = 10*val + (c-'0');
      if (newval < val) {
        Serial.println("\r\nOverflow");
        goto begin_get_int;
      }
      val = newval;
      Serial.print((char)c);
    } else {
      Serial.println("\r\nInvalid character!");
      goto begin_get_int;
    }
  }
  // Serial.print("Saving value ");
  // Serial.println(val*mult);
  *dest = val*mult;
  return true;
}

bool get_bool(bool *dest, char * descr) {
  try_again:
  char val[2];
  val[0] = *dest?'Y':'N';
  val[1] = 0;
  get_string(val, descr, 2, false);
  if(val[0] != 'y' && val[0] != 'Y' && val[0] != 'n' && val[0] != 'N') {
    Serial.println("Please respond Y or N.");
    return false;
  }
  *dest = (val[0] == 'Y' || val[0] == 'y');
  // Serial.print("Saving value ");
  Serial.println(*dest?"Y":"N");
  return true;
}

bool get_string(char *dest, char * descr, size_t max_len, bool hide_curval) {
  begin_get_string:
  char buf[256];
  sprintf(buf, "%s [%s]: ", descr, hide_curval?"********":dest);
  Serial.print(buf);
  memset(buf, 0, sizeof(buf));
  
  bool done = false;
  int idx = 0;
  while(!done) {
    if(!Serial.available()) { delay(10); continue; }
    int c = Serial.read();
    if(c=='\n' || c=='\r') {
      Serial.println((char)c);
      if(idx > 0 && buf[idx-1] != 0) {
        buf[idx++] = 0;
      } else if(idx == 0) {
        // User hit enter. Just keep current value.
        return true;
      }
      done = true;
    } else if (c >= 0x20 && c <= 0x7e) {
      buf[idx++] = c;
      Serial.print((char)c);
    } else {
      Serial.println("\r\nInvalid character!");
      goto begin_get_string;
    }
    if(idx > max_len) {
      Serial.println("\r\nToo long!");
      goto begin_get_string;
    }
  }
  // Serial.print("Saving value [");
  // Serial.print(buf);
  // Serial.println("]");
  strcpy(dest, buf);
  return true;
}

bool config_timeout = false;
// If user has D6 and D7 connected, this indicates they want to reconfigure
// the device. (Pins 3 and 5 on the header are connected).
void maybe_configure() {
  // First, load the config from the eeprom into memory
  EEPROM.begin(sizeof(config));
  EEPROM.get(0, config);
  config.magic[4] = 0;
  pinMode(D5, INPUT_PULLUP);
  pinMode(D6, OUTPUT);
  pinMode(D7, INPUT_PULLUP);
  digitalWrite(D6, LOW);
  delay(10);
  bool eeprom_inited = true;
  // Reset config with some reasonable defaults
  if(strcmp(config.magic, "LUTZ") != 0) {
    Serial.println("EEPROM not initialized.");
    eeprom_inited = false;
    memset(&config, 0, sizeof(config));
    config.reporting_interval_seconds = 300;
    config.deep_sleep = true;
    strcpy(config.destination_addr, "255.255.255.255");
    config.destination_port = 4210;
    strcpy(config.ssid, "somewifi");
    strcpy(config.password, "somepassword");
    strcpy(config.device_id, "0");
  }
  
  if(config.magic[0]!=0 && /* eeprom initialized */
     (digitalRead(D5)==LOW /* definitely in run mode */
     || digitalRead(D7)!=LOW /* user lost the jumper? */)) {
    Serial.println("In RUN mode");
    return;
  }

  Serial.println("In CONFIG mode");
  if(eeprom_inited) {
    // Bail out of configuring after a set timeout
    Serial.println("Press any key within 30 seconds to reconfigure device...");
    unsigned long until = millis() + CONFIG_INPUT_TIMEOUT_SECONDS*1000;
    while(!Serial.available() && millis() < until) {
      delay(10);
    }
    if(!Serial.available()) {
      Serial.println("Configuring timed out. Running normally.");
      config_timeout = true;
      return;
    }
    while(Serial.available()) Serial.read();
  }

  Config newconfig;
  memcpy(&newconfig, &config, sizeof(config));
  retry:
  while(!get_string(newconfig.device_id, "Device ID", sizeof(config.device_id), false)){}
  while(!get_string(newconfig.ssid, "Wifi SSID", sizeof(config.ssid), false)){}
  while(!get_string(newconfig.password, "Wifi Password", sizeof(config.password), true)){}
  while(!get_string(newconfig.destination_addr, "Destination Address", sizeof(config.destination_addr), false)){}
  while(!get_int(&newconfig.destination_port, "Destination Port")){}
  while(!get_int(&newconfig.reporting_interval_seconds, "Reporting Interval (seconds)")){}
  while(!get_bool(&newconfig.deep_sleep, "Deep Sleep")){}

  bool commit = false;
  print_configuration(newconfig);
  while(!get_bool(&commit, "Commit Configuration?")){}
  if(!commit) goto retry;
  
  // Mark EEPROM as initialized
  strcpy(newconfig.magic, "LUTZ");
  memcpy(&config, &newconfig, sizeof(Config));
  EEPROM.put(0, config);
  Serial.println("CONFIGURATION COMMITTED!");
  EEPROM.end();
}

unsigned long stamp;

void setup() {
  pinMode(D0, WAKEUP_PULLUP);
  pinMode(DHTPIN, INPUT_PULLUP);
  dht.begin();
  Serial.begin(9600);
  Serial.println("\r\n====BOOT====");
  maybe_configure();

  print_configuration(config);
  
  WiFi.begin(config.ssid, config.password);
  stamp = millis();
}

bool ran_once = false;

void loop() {
  if(!config.deep_sleep && ran_once) {
    unsigned long now = millis();
    if(now < stamp || /* overflow */
       now > (stamp + config.reporting_interval_seconds*1000)) {
      Serial.println("OK to proceed!");
      stamp = now;  
    } else {
      // Serial.println("_");
      delay(1000);
      return;
    }
  }
  ran_once = true;
  
  if(WiFi.status() != WL_CONNECTED) {
    while (WiFi.status() != WL_CONNECTED)
    {
      Serial.print(".");
      delay(200);
    }
  
    Serial.println();
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
  }

  // Reading temperature or humidity takes about 250 milliseconds!
  // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
  float h = dht.readHumidity();
  // Read temperature as Fahrenheit (isFahrenheit = true)
  float f = dht.readTemperature(true);

  // Check if any reads failed and exit early (to try again).
  if (isnan(h) || isnan(f)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    return;
  }

  // Compute heat index in Fahrenheit (the default)
  float hif = dht.computeHeatIndex(f, h);

  char reply[512];
  memset(reply, 0, 255);

  Serial.print(F("Humidity: "));
  Serial.print(h);
  Serial.print(F("%  Temperature: "));
  Serial.print(f);
  Serial.print(F("°F  Heat index: "));
  Serial.print(hif);
  Serial.println(F("°F"));
  sprintf(reply, "DEVICE_ID=%s,HUMIDITY=%f,TEMP_F=%f,HEAT_INDEX=%f", config.device_id, h, f, hif);
  Serial.println(reply);
  UDP.beginPacket(config.destination_addr, config.destination_port);
  UDP.write(reply);
  UDP.endPacket();

  if (config.deep_sleep) {
    Serial.print("Going to deep sleep for ");
    int to_sleep = config.reporting_interval_seconds - (config_timeout? CONFIG_INPUT_TIMEOUT_SECONDS : 0);
    if(to_sleep <= 0) to_sleep=1;
    Serial.print(to_sleep);
    Serial.println(" seconds");
    delay(50);
    ESP.deepSleep(to_sleep*1000000UL, WAKE_RF_DEFAULT);    
  } else {
    Serial.print("Delaying for ");
    Serial.print(config.reporting_interval_seconds);
    Serial.println(" seconds");
  }
}
