#include <Arduino.h>
#include <FastLED.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <CertStoreBearSSL.h>
#include <map>
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <Ticker.h>
#include <FS.h>

#define NUM_LEDS 29
#define DATA_PIN 5
#define LED_PIN 2
#define SWITCH_PIN 8
#define LIGHT_SENSOR A0
#define BUTTON_PIN 0

#define LED_STEP_SIZE 10
#define UPDATE_MINUTES 5

Ticker led_updater;
CRGB led_current[NUM_LEDS];
CRGB led_target[NUM_LEDS];
typedef enum {
  OFF,
  WAIT,
  WIFI,
  WX,
  ERROR
} led_mode_t;
led_mode_t led_mode = led_mode_t::OFF;

// Note: DO NOT use the "fields" argument. As of April 2020, it is broken.
String url = "https://www.aviationweather.gov/adds/dataserver_current/httpparam?dataSource=metars&requestType=retrieve&format=csv&mostRecentForEachStation=true&hoursBeforeNow=1.25&stationString=";
BearSSL::CertStore certStore;

std::map<String, CRGB> cat_lut {
  {"VFR",  CRGB::Green},
  {"MVFR", CRGB::Blue},
  {"IFR",  CRGB::Red},
  {"LIFR", CRGB(192, 0, 80)}
};

// Associate stations with LED identifiers. This is not the best way to do it, but it's what I did.
std::map<String, int> station_lut {
  {"KX60", 2},
  {"KFIN", 5},
  {"KINF", 25},
  {"KVDF", 23},
  {"KCTY", 28},
  {"K28J", 4},
  {"KCGC", 26},
  {"KXMR", 12},
  {"KCOF", 13},
  {"KISM", 18},
  {"KTTS", 11},
  {"KZPH", 21},
  {"KDED", 7},
  {"KPCM", 22},
  {"KSFB", 10},
  {"KLEE", 0},
  {"KDAB", 6},
  {"KORL", 16},
  {"KGIF", 19},
  {"KMCO", 17},
  {"KMLB", 14},
  {"KGNV", 3},
  {"KBKV", 24},
  {"KOCF", 1},
  {"KEVB", 9},
  {"KTIX", 15},
  {"KOMN", 8},
  {"KLAL", 20} 
};

// Some LEDs might be backwards. We should ignore them in animations.
const std::vector<int> led_ignore { 27 };

/**
 * @brief Read a comma-delimited string looking for specific information.
 * 
 * @param line Excerpt of a CSV string
 * @param station Station identifier
 * @param cat Weather category
 */
void line_parse(String &line, String& station, String& cat)
{
  int i = 0;
  int start = 0;
  int end;

  // TODO: This method misses info after the last comma. Consider tail recursion.
  while ((end = line.indexOf(',', start)) != -1) {
    if (i == 1) {
      station = line.substring(start, end);
    }

    if (i == 30) {
      cat = line.substring(start, end);
      // We won't need any more information
      return;
    }

    i += 1;
    start = end + 1;
  }
}

void payload_proc(const char *payload) {
  // Initialize a buffer for LED colors
  static CRGB led_tmp[NUM_LEDS];
  for (int i = 0; i < NUM_LEDS; i++) {
    led_tmp[i] = CRGB::Black;
  }

  // Disable the loading colors
  led_mode = led_mode_t::WX;

  int line_no = 0;
  char *tok = strtok((char *) payload, "\n");
  while (tok != nullptr) {
    if (line_no == 0) { // Errors
      if (strcmp(tok, "No errors") != 0) {
        Serial.println(tok);
      }
    }
    else if (line_no == 1) { // Warnings
      if (strcmp(tok, "No warnings") != 0) {
        Serial.println(tok);
      }
    }
    else if (line_no == 4) { // Num results
      // Ignore for now
    }
    else if (line_no >= 6) { // Result lines
      String station, category;
      String Tok(tok);
      line_parse(Tok, station, category);

      // If the station is in our list, give it a color
      if (station_lut.find(station) != station_lut.end()) {
        auto color = cat_lut.find(category) != cat_lut.end() ? cat_lut[category] : CRGB::Black;
        led_tmp[station_lut[station]] = color;
      }
    }

    ++line_no;
    tok = strtok(nullptr, "\n");
  }

  // Copy the local LED color buffer to the target for display
  for (int i = 0; i < NUM_LEDS; i++) {
    led_target[i] = led_tmp[i];
  }
}

/**
 * @brief Retrieve the weather and load the colors into the color target array.
 */
void fetch_weather() {
  auto _path = url;
  for (auto const& imap : station_lut) {
    _path.concat(imap.first + ",");
  }

  HTTPClient http;
  BearSSL::WiFiClientSecure bear = BearSSL::WiFiClientSecure();
  bear.setCertStore(&certStore);
  if (http.begin(dynamic_cast<WiFiClient&>(bear), _path)) {
    http.addHeader("User-Agent", "ESP8266");
    http.addHeader("Connection", "close");
    int code = http.GET();
    if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
      auto payload = http.getString();
      payload_proc(payload.c_str());
    }
    else {
      led_mode = led_mode_t::ERROR;
      Serial.print("!!! Error: server returned with code ");
      Serial.print(code);
      Serial.println(". !!!");
    }
    http.end();
  }
  else {
    led_mode = led_mode_t::ERROR;
    Serial.println("!!! Error: no connection to server. !!!");
  }
}

/**
 * @brief Control LED colors and animations.
 */
void led_update_handler() {
  // Wait animation is random color spin
  if (led_mode == led_mode_t::WAIT) {
    randomSeed(1); // Always generate random numbers in the same order
    int8_t time_offset = millis() * 255 / 2000; // Repeats every 2s

    for (int i = 0; i < NUM_LEDS; i++) {
      // Ignore LEDs in the list
      static const auto begin = led_ignore.begin();
      static const auto end = led_ignore.end();
      if (std::find(begin, end, i) != end) continue;

      int8_t rand_offset = random(255);
      led_current[i].setHue(time_offset + rand_offset);
    }

    randomSeed(esp_get_cycle_count()); // In case something else needs the PRNG
  }

  else if (led_mode == led_mode_t::WX) {
    // Loop over every LED and colors, linearly approaching the target color
    for (int i = 0; i < NUM_LEDS; i++) {
      for (int j = 0; j < 3; j++) {
        auto &tgt = led_target[i][j];
        auto &cur = led_current[i][j];

        if (tgt != cur) {
          if (tgt - LED_STEP_SIZE > cur)
            cur += LED_STEP_SIZE; 
          else if (tgt + LED_STEP_SIZE < cur)
            cur -= LED_STEP_SIZE; 
          else
            cur = tgt;
        }
      }
    }
  }

  else if (led_mode == led_mode_t::ERROR) {
    auto c = ((millis() / 1000) % 2) ? CRGB::Red : CRGB::Black;

    for (int i = 0; i < NUM_LEDS; i++) {
      // Ignore LEDs in the list
      static const auto begin = led_ignore.begin();
      static const auto end = led_ignore.end();
      if (std::find(begin, end, i) != end) continue;

      led_current[i] = c;
    }
  }

  else if (led_mode == led_mode_t::WIFI) {
    auto c = ((millis() % 1000) < 200) ? CRGB::Blue : CRGB::Black;

    for (int i = 0; i < NUM_LEDS; i++) {
      // Ignore LEDs in the list
      static const auto begin = led_ignore.begin();
      static const auto end = led_ignore.end();
      if (std::find(begin, end, i) != end) continue;

      led_current[i] = c;
    }
  }

  auto b = analogRead(LIGHT_SENSOR);
  const int min_ = 18;
  const int max_ = 255;

  float target = constrain(map(b, 1024, 800, min_, max_), min_, max_);
  static float current = 0;
  current = (current * 0.97f) + (target * 0.03f);
  FastLED.setBrightness(current);

  FastLED.show();
}

void setup() {
  Serial.begin(115200);
  Serial.println("WxSectional JAX");

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(LIGHT_SENSOR, INPUT);

  FastLED.addLeds<NEOPIXEL, DATA_PIN>(led_current, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 1000); // Protect the power source (such as a computer)
  FastLED.setBrightness(0); // Start off completely black and let the brightness controller fade in
  delay(100); // No idea why, but this seems to be required
  for (int i = 0; i < NUM_LEDS; i++) {
    led_current[i] = CRGB::Black;
    led_target[i] = CRGB::Black;
  }
  FastLED.show();
  led_mode = led_mode_t::WAIT;
  led_updater.attach_ms(20, led_update_handler);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  SPIFFS.begin();
  int num_certs = certStore.initCertStore(SPIFFS, "/certs.idx", "/certs.ar");
  Serial.print("Number of CA certs read: ");
  Serial.println(num_certs);
}

void loop() {
  static long updated_at = LONG_MIN;
  
  static bool btn_last = HIGH;
  static long btn_pressed_at = 0;

  static bool smart_config = false;
  static long smart_config_started_at = 0;

  auto now = time(nullptr);
  bool time_is_valid = (now >= 946080000); // Allow any time in the 21st century

  // Send an 
  if (WiFi.isConnected() && time_is_valid && ((millis() - updated_at) >= (UPDATE_MINUTES * 60 * 1000))) {
    Serial.print("Updated at ");
    Serial.print(ctime(&now));
    fetch_weather();
    updated_at = millis();
  }

  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && btn_last == HIGH) {
    btn_pressed_at = millis();
  }
  else if (btn == LOW && (millis() - btn_pressed_at) >= 2000 && !smart_config) {
    WiFi.disconnect();
    WiFi.beginSmartConfig();

    smart_config = true;
    smart_config_started_at = millis();
    btn_pressed_at = -1;
    led_mode = led_mode_t::WIFI;

    Serial.println("WiFi smart config started");
  }
  else if (btn == HIGH) {
    btn_pressed_at = 0;
  }
  btn_last = btn;

  if (smart_config) {
    if (WiFi.smartConfigDone()) {
      smart_config = false;
      Serial.println("WiFi connected!");

      led_mode = led_mode_t::WAIT;
      updated_at = LONG_MIN;
    }
    else if ((millis() - smart_config_started_at) >= (60 * 1000)) {
      WiFi.stopSmartConfig();

      smart_config = false;
      Serial.println("WiFi smart config timeout");

      led_mode = led_mode_t::ERROR;
    }
    
    auto led = ((millis() - smart_config_started_at) % 1000) >= 500;
    digitalWrite(LED_PIN, led);
  }
  else {
    digitalWrite(LED_PIN, HIGH);
  }

  auto wstat = WiFi.status();
  static auto last_wstat = WL_DISCONNECTED;
  if (wstat == WL_NO_SSID_AVAIL && last_wstat != WL_NO_SSID_AVAIL) {
    led_mode = led_mode_t::ERROR;
    Serial.println("WiFi could not find SSID");
  }
  else if (wstat == WL_CONNECT_FAILED && last_wstat != WL_CONNECT_FAILED) {
    led_mode = led_mode_t::ERROR;
    Serial.println("WiFi could not connect to SSID");
  }
  else if (wstat == WL_CONNECTION_LOST && last_wstat != WL_CONNECTION_LOST) {
    led_mode = led_mode_t::ERROR;
    Serial.println("WiFi connection dropped");
  }
  else if (wstat == WL_CONNECTED && last_wstat != WL_CONNECTED) {
    led_mode = led_mode_t::WAIT;
    updated_at = LONG_MIN;
  }
  last_wstat = wstat;
}
