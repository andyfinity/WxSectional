#include <Arduino.h>
#include <FastLED.h>
#include <ESP8266WiFi.h>
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
Ticker brightness_updater;
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

String host = "www.aviationweather.gov";
const int https_port = 443;
// Note: DO NOT use the "fields" argument. As of April 2020, it is broken.
String path = "/adds/dataserver_current/httpparam?dataSource=metars&requestType=retrieve&format=csv&mostRecentForEachStation=true&hoursBeforeNow=1.25&stationString=";
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

int atoi( const char*);

/**
 * @brief Read a comma-delimited string looking for specific information.
 * 
 * @param line Excerpt of a CSV string
 * @param station Station identifier
 * @param cat Weather category
 */
void line_parse(String line, String& station, String& cat)
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

/**
 * @brief Retrieve the weather and load the colors into the color target array.
 */
void fetch_weather() {
  auto _path = path;
  for (auto const& imap : station_lut) {
    _path.concat(imap.first + ",");
  }
  Serial.printf("https://%s%s\n", host.c_str(), _path.c_str());

  // TODO: Move to HTTPClient for proper certificate checking. See https://medium.com/@dfa_31434/doing-ssl-requests-on-esp8266-correctly-c1f60ad46f5e
  WiFiClientSecure client;
  client.setInsecure(); // Don't verify fingerprint
  if (!client.connect(host, https_port)) {
    Serial.println("Wx fetch could not connect to https://" + host);
    return;
  }

  client.print(String("GET ") + _path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP8266\r\n" +
               "Connection: close\r\n\r\n");

  // Ignore HTTP headers
  // TODO: Check for "200 OK"
  while (client.connected()) {
    auto line = client.readStringUntil('\n');
    if (line == "\r") {
      break;
    }
    Serial.println(line);
  }

  // Data statistics (mostly ignored)
  String errors = client.readStringUntil('\n');
  String warnings = client.readStringUntil('\n');
  String duration = client.readStringUntil('\n');
  String data_source = client.readStringUntil('\n');
  String num_results = client.readStringUntil('\n');

  if (errors != "No errors") Serial.println("Server returned an error: " + errors);
  if (warnings != "No warnings") Serial.println("Server returned a warning: " + warnings);
  Serial.print(num_results.toInt());
  Serial.println(" stations");
  Serial.print(duration);
  Serial.println(" server time");

  if (errors != "No errors") {
    led_mode = led_mode_t::ERROR;
    Serial.println("!!! Aborting due to server error. !!!");
    return;
  }

  // Ignore column headers (assume the format won't change)
  String col_headers = client.readStringUntil('\n');

  // Initialize a buffer for LED colors
  static CRGB led_tmp[NUM_LEDS];
  for (int i = 0; i < NUM_LEDS; i++) {
    led_tmp[i] = CRGB::Black;
  }

  // Disable the loading colors
  led_mode = led_mode_t::WX;

  // Loop over lines
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    Serial.printf(" %s\n", line.c_str());

    // Fetch the relevant tokens in the line
    String station;
    String category;
    line_parse(line, station, category);
    Serial.printf(" %s %s\n", station.c_str(), category.c_str());

    // If the station is in our list, give it a color
    if (station_lut.find(station) != station_lut.end()) {
      auto color = cat_lut.find(category) != cat_lut.end() ? cat_lut[category] : CRGB::Black;
      led_tmp[station_lut[station]] = color;
      Serial.printf("  color (%d, %d, %d)\n", color.r, color.g, color.b);
    }
  }

  // Copy the local LED color buffer to the target for display
  for (int i = 0; i < NUM_LEDS; i++) {
    led_target[i] = led_tmp[i];
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

  FastLED.show();
}

/**
 * @brief Monitor brightness.
 * 
 * TODO: Consider moving this to led_update_handler(). Requires changing some time constants.
 */
void brightness_update_handler() {
  auto b = analogRead(LIGHT_SENSOR);
  const int min_ = 18;
  const int max_ = 255;

  float target = constrain(map(b, 1024, 800, min_, max_), min_, max_);
  static float current = 0;
  current = (current * 0.9f) + (target * 0.1f);
  FastLED.setBrightness(current);
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  Serial.println("WxSectional JAX");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

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
  led_mode = led_mode_t::OFF;
  led_updater.attach_ms(20, led_update_handler);
  brightness_updater.attach_ms(100, brightness_update_handler);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);

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

      led_mode == led_mode_t::WAIT;
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
