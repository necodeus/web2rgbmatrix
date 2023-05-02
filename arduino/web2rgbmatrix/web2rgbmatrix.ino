#include "FS.h"
#include "SPI.h"
#include <AnimatedGIF.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32Ping.h>
#include <ESPmDNS.h>
#include <ezTime.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <SD.h>
#include <SdFat.h>
#include <TetrisMatrixDraw.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "bitmaps.h"

#define CONFIG_LITTLEFS_SPIFFS_COMPAT 1

#define VERSION "20230502"

#define DEFAULT_TIMEZONE "Europe/London"
#define DEFAULT_TWELVEHOUR false
#define DEFAULT_SSID "Dawid"
#define DEFAULT_PASSWORD "password"
#define DEFAULT_AP_SSID "Dawid"
#define DEFAULT_AP_PASSWORD "password"
#define DEFAULT_HOSTNAME "rgbmatrix"
#define DEFAULT_TEXT_COLOR "#FFFFFF"
#define DEFAULT_BRIGHTNESS 255
#define DEFAULT_GIF_PLAYBACK "Both" // Animated | Static | Both | Fallback
#define DEFAULT_SCREENSAVER "Blank" // Blank | Clock | Plasma | Starfield | Toasters
#define DEFAULT_SCREENSAVER_COLOR "#FFFFFF"
#define DEFAULT_PING_FAIL_COUNT 0 // 0 - disabled | 1 - 30 seconds | 2 - 60 seconds
#define DEFAULT_SD_STATIC_GIF_FOLDER "/static/"
#define DEFAULT_SD_ANIMATED_GIF_FOLDER "/animated/"
#define DBG_OUTPUT_PORT Serial

#define E 18
#define B1 26 // warning: "B1" already defined in .arduino15/packages/esp32/hardware/esp32/2.0.8/cores/esp32/binary.h
#define B2 12
#define G1 27
#define G2 13
#define R1 25
#define R2 14
#define SD_SCLK 33
#define SD_MISO 32
#define SD_MOSI 21
#define SD_SS 22
#define ALT 2
// note: No hardware SPI pins defined. All SPI access will default to bitbanged output

char timezone[80] = DEFAULT_TIMEZONE;
bool twelvehour = DEFAULT_TWELVEHOUR;
char ssid[80] = DEFAULT_SSID;
char password[80] = DEFAULT_PASSWORD;
char ap[80] = DEFAULT_AP_SSID;
char ap_password[80] = DEFAULT_PASSWORD;
char hostname[80] = DEFAULT_HOSTNAME;
String textcolor = DEFAULT_TEXT_COLOR;
uint8_t matrix_brightness = DEFAULT_BRIGHTNESS;
String playback = DEFAULT_GIF_PLAYBACK;
String screensaver = DEFAULT_SCREENSAVER;
String accentcolor = DEFAULT_SCREENSAVER_COLOR;
int ping_fail_count = DEFAULT_PING_FAIL_COUNT;
char gif_folder[80] = DEFAULT_SD_STATIC_GIF_FOLDER;
char animated_gif_folder[80] = DEFAULT_SD_ANIMATED_GIF_FOLDER;

SPIClass spi = SPIClass(HSPI);

MatrixPanel_I2S_DMA *matrix_display = nullptr;
const int panelResX = 64;
const int panelResY = 32;
const int panels_in_X_chain = 2;
const int panels_in_Y_chain = 1;
const int totalWidth = panelResX * panels_in_X_chain;
const int totalHeight = panelResY * panels_in_Y_chain;
int16_t xPos = 0, yPos = 0;

WebServer server(80);
IPAddress my_ip;
IPAddress no_ip(0, 0, 0, 0);
IPAddress client_ip(0, 0, 0, 0);

String style = "<style>#file-input,input{width:100%;height:44px;border-radius:4px;margin:10px auto;font-size:15px}a{outline:none;text-decoration:none;padding: 2px 1px 0;color:#777;}a:link{color:#777;}a:hover{solid;color:#3498db;}input{background:#f1f1f1;border:0;padding:0 15px}body{background:#3498db;font-family:sans-serif;font-size:14px;color:#777}#file-input{padding:0;border:1px solid #ddd;line-height:44px;text-align:left;display:block;cursor:pointer}#bar,#prgbar{background-color:#f1f1f1;border-radius:10px}#bar{background-color:#3498db;width:0%;height:10px}form{background:#fff;max-width:258px;margin:75px auto;padding:30px;border-radius:5px;text-align:center}.btn{background:#3498db;color:#fff;cursor:pointer}.btn:disabled,.btn.disabled{background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}.actionbtn{background:#218838;color:#fff;cursor:pointer}.actionbtn:disabled,.actionbtn.disabled{background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}.cautionbtn{background:#c82333;color:#fff;cursor:pointer}.cautionbtn:disabled,.cautionbtn.disabled{background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}input[type=\"checkbox\"]{margin:0px;width:22px;height:22px;}table{width: 100%;}select{width: 100%;height:44px;}</style>";

const char *config_filename = "/secrets.json";
const char *gif_filename = "/temp2.gif";

String wifi_mode = "AP";
String sd_status = "";
bool card_mounted = false;
bool config_display_on = true;
bool tty_client = false;
unsigned long last_seen, start_tick;
int ping_fail = 0;
String sd_filename = "";
File gif_file, upload_file;
String new_command = "";
String current_command = "";
AnimatedGIF gif;

uint16_t time_counter = 0, cycles = 0;
CRGB currentColor;
CRGBPalette16 palettes[] = { HeatColors_p, LavaColors_p, RainbowColors_p, RainbowStripeColors_p, CloudColors_p };
CRGBPalette16 currentPalette = palettes[0];
CRGB ColorFromCurrentPalette(uint8_t index = 0, uint8_t brightness = 255, TBlendType blendType = LINEARBLEND) {
  return ColorFromPalette(currentPalette, index, brightness, blendType);
}

const int starCount = 256;
const int maxDepth = 32;
double stars[starCount][3];
CRGB *matrix_buffer;

bool forceRefresh = true;
unsigned long animationDue = 0;
unsigned long animationDelay = 100;
TetrisMatrixDraw tetris(*matrix_display);
TetrisMatrixDraw tetris2(*matrix_display);
TetrisMatrixDraw tetris3(*matrix_display);
Timezone myTZ;
unsigned long oneSecondLoopDue = 0;
bool showColon = true;
volatile bool finishedAnimating = false;
String lastDisplayedTime = "";
String lastDisplayedAmPm = "";
const int tetrisYOffset = (totalHeight) / 2;
const int tetrisXOffset = (panelResX) / 2;

#define N_FLYERS 5
struct Flyer {
  int16_t x, y;
  int8_t depth;
  uint8_t frame;
} flyer[N_FLYERS];

TaskHandle_t Task1;

bool uploading = false;

const char *th_filePath = "";
bool th_isExternalDevice;
bool allowPlaying = true;
bool isPlayable = false;

void setGIF(const char *filePath, bool isExternalDevice) {
  th_filePath = filePath;
  th_isExternalDevice = isExternalDevice;
  isPlayable = true; // możliwość wznowienia dla zakończonych
  allowPlaying = false; // zatrzymanie dla trwających
}

void codeForTask1(void *parameter) {
  while (true) {
    if (
      isPlayable
      && (
        SD.exists(th_filePath) || LittleFS.exists(th_filePath)
      )
    ) {
      showGIF(th_filePath, th_isExternalDevice);
      continue;
    }
    delay(50);
  }
}

void setup(void) {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(1000);

  pinMode(ALT, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.printf("[ERROR] LittleFS Mount Failed!\n");
  }

  LittleFS.remove(gif_filename);

  parseConfig();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.printf("[OK] Connecting to: %s\n", ssid);

  uint8_t i = 0;
  while ((WiFi.status() != WL_CONNECTED) && (i++ < 20)) {
    delay(500);
  }

  if (i == 21) {
    Serial.printf("[ERROR] Could not connect to: %s\n", ssid);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap, ap_password);
    my_ip = WiFi.softAPIP();
    Serial.printf("[ERROR] IP address: %s\n", my_ip.toString().c_str());
  } else {
    my_ip = WiFi.localIP();
    wifi_mode = "Infrastructure";
    Serial.printf("[OK] Connected to WIFI! IP address: %s\n", my_ip.toString().c_str());
  }
  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  if (WiFi.status() == WL_CONNECTED) {
    setDebug(INFO);
    waitForSync();

    Serial.printf("[INFO] UTC: %s\n", UTC.dateTime().c_str());

    myTZ.setLocation(F(timezone));

    Serial.printf("[INFO] Time in your timezone: %s\n", myTZ.dateTime().c_str());
  }

  spi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_SS);
  if (SD.begin(SD_SS, spi, 8000000)) {
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.printf("[INFO] No SD card\n");
      sd_status = "No card";
    } else {
      card_mounted = true;
      if (cardType == CARD_MMC) {
        Serial.printf("[INFO] SD Card Type: MMC\n");
      } else if (cardType == CARD_SD) {
        Serial.printf("[INFO] SD Card Type: SDSC\n");
      } else if (cardType == CARD_SDHC) {
        Serial.printf("[INFO] SD Card Type: SDHC\n");
      } else {
        Serial.printf("[INFO] SD Card Type: UNKNOWN\n");
      }
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("[INFO] SD Card Size: %lluMB\n", cardSize);
      sd_status = String(cardSize) + "MB";
    }
  } else {
    Serial.printf("[ERROR] Card Mount Failed\n");
    sd_status = "Mount Failed";
  }

  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[OK] MDNS responder started! You can now connect to %s.local\n", hostname);
  }

  server.client().setTimeout(50000);
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/sdcard", handleSD);
  server.on("/upload", HTTP_POST, []() { server.sendHeader("Connection", "close"); }, handleUpload);
  server.on("/list", HTTP_GET, handleFileList);
  server.on("/delete", HTTP_GET, handleFileDelete);
  server.on("/sdplay", handleSDPlay);
  server.on("/localplay", handleLocalPlay);
  server.on("/remoteplay", HTTP_POST, []() { server.send(200); }, handleRemotePlay);
  server.on("/text", handleText);
  server.on("/version", handleVersion);
  server.on("/clear", handleClear);
  server.on("/reboot", handleReboot);
  server.onNotFound(handleNotFound);

  const char *headerkeys[] = { "Content-Length" };
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();

  gif.begin(LITTLE_ENDIAN_PIXELS);

  displaySetup();

  for (int i = 0; i < starCount; i++) {
    stars[i][0] = getRandom(-25, 25);
    stars[i][1] = getRandom(-25, 25);
    stars[i][2] = getRandom(0, maxDepth);
  }
  matrix_buffer = (CRGB *)malloc(((totalWidth) * (totalHeight)) * sizeof(CRGB));
  bufferClear(matrix_buffer);

  tetris.display = matrix_display;
  tetris2.display = matrix_display;
  tetris3.display = matrix_display;
  finishedAnimating = false;
  tetris.scale = 2;

  randomSeed(analogRead(2));
  for (uint8_t i = 0; i < N_FLYERS; i++) {
    flyer[i].x = (-32 + random(160)) * 16;
    flyer[i].y = (-32 + random(96)) * 16;
    flyer[i].frame = random(3) ? random(4) : 255;
    flyer[i].depth = 10 + random(16);
  }
  qsort(flyer, N_FLYERS, sizeof(struct Flyer), compare);

  showStatus();

  start_tick = millis();

  xTaskCreatePinnedToCore(codeForTask1, "FibonacciTask", 5000, NULL, 2, &Task1, 0);

  Serial.printf("[OK] Setup Complete!\n");
}

void loop(void) {
  if (config_display_on && (millis() - start_tick >= 60 * 1000UL)) {
    config_display_on = false;
    matrix_display->clearScreen();
    matrix_display->setBrightness8(matrix_brightness);
  }
  server.handleClient();
  checkSerialClient();
  checkClientTimeout();
}

bool parseConfig() {
  File config_file = LittleFS.open(config_filename);
  if (!config_file) {
    Serial.printf("[ERROR] Could not open secrets.json file for reading!\n");
    return false;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, config_file);
  if (err) {
    Serial.printf("[ERROR] JSON Deserialization failed! Code: %s\n", err.c_str());
    return false;
  }

  strlcpy(ssid, doc["ssid"] | DEFAULT_SSID, sizeof(ssid));
  strlcpy(password, doc["password"] | DEFAULT_PASSWORD, sizeof(password));
  ping_fail_count = doc["timeout"] | DEFAULT_PING_FAIL_COUNT;
  matrix_brightness = doc["brightness"] | DEFAULT_BRIGHTNESS;
  textcolor = doc["textcolor"] | DEFAULT_TEXT_COLOR;
  playback = doc["playback"] | DEFAULT_GIF_PLAYBACK;
  screensaver = doc["screensaver"] | DEFAULT_SCREENSAVER;
  accentcolor = doc["accentcolor"] | DEFAULT_SCREENSAVER_COLOR;
  twelvehour = doc["twelvehour"] | DEFAULT_TWELVEHOUR;
  strlcpy(timezone, doc["timezone"] | DEFAULT_TIMEZONE, sizeof(timezone));

  config_file.close();
  return true;
}

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[OK] Connected to AP successfully!\n");
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[OK] WiFi connected. IP address: %s\n", WiFi.localIP().toString().c_str());
  my_ip = WiFi.localIP();
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[ERROR] WiFi lost connection. Reason: %d\nTrying to Reconnect...\n", info.wifi_sta_disconnected.reason);
  WiFi.begin(ssid, password);
}

// Endpoint: /
// Description:
// Usage:
void handleRoot() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  String image_status = "";

  if (LittleFS.exists(gif_filename)) {
    image_status = "<tr><td>Client</td><td>" + client_ip.toString() + "</td></tr><tr><td>Current Image</td><td><img src=\"" + String(gif_filename) + "\"><img></td></tr>";
  } else if (sd_filename != "") {
    image_status = "<tr><td>Client</td><td>" + ((tty_client) ? "Serial" : client_ip.toString()) + "</td></tr><tr><td>Current Image</td><td><img src=\"" + String(sd_filename) + "\"><img></td></tr>";
  }

  server.send(
    200,
    F("text/html"),
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>web2rgbmatrix</title>" + style + "</head><body><form action=\"/\"><a href=\"\"><h1>web2rgbmatrix</h1></a><br><p><h3>Status</h3><table><tr><td>Version</td><td>" + VERSION + "</td></tr><tr><td>SD Card</td><td>" + sd_status + "</td></tr><tr><td>Wifi Mode</td><td>" + wifi_mode + "</td></tr><tr><td>rgbmatrix IP</td><td>" + my_ip.toString() + "</td></tr>" + image_status + "</table></p><input type=\"button\" class=actionbtn onclick=\"location.href='/clear';\" value=\"Clear Display\" />" + ((card_mounted) ? "<input type=\"button\" class=btn onclick=\"location.href='/sdcard';\" value=\"GIF Upload\" /><input type=\"button\" class=btn onclick=\"location.href='/list?dir=/';\" value=\"File Browser\" />" : "") + "<input type=\"button\" class=btn onclick=\"location.href='/settings';\" value=\"Settings\" /><input type=\"button\" class=btn onclick=\"location.href='/ota';\" value=\"OTA Update\" /><input type=\"button\" class=cautionbtn onclick=\"location.href='/reboot';\" value=\"Reboot\" /></form></body></html>\r\n"
  );
}

// Endpoint: /settings
// Description:
// Usage:
void handleSettings() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  // Jeżeli pola nie są uzupełnione, wyświetlamy widok ustawień
  if (
    server.arg("ssid") == ""
    || server.arg("password") == ""
    || server.arg("brightness") == ""
    || server.arg("timeout") == ""
    || server.arg("textcolor") == ""
    || server.arg("screensaver") == ""
    || server.arg("timezone") == ""
  ) {
    String playback_select_items =
      "<option value=\"Animated\"" + String((playback == "Animated") ? " selected" : "") + ">Animated</option>"
      "<option value=\"Static\""   + String((playback == "Static")   ? " selected" : "") + ">Static</option>"
      "<option value=\"Both\""     + String((playback == "Both")     ? " selected" : "") + ">Animated then Static</option>"
      "<option value=\"Fallback\"" + String((playback == "Fallback") ? " selected" : "") + ">Static Fallback</option>";

    String saver_select_items =
      "<option value=\"Blank\""     + String((screensaver == "Blank")     ? " selected" : "") + ">Blank</option>"
      "<option value=\"Clock\""     + String((screensaver == "Clock")     ? " selected" : "") + ">Clock</option>"
      "<option value=\"Plasma\""    + String((screensaver == "Plasma")    ? " selected" : "") + ">Plasma</option>"
      "<option value=\"Starfield\"" + String((screensaver == "Starfield") ? " selected" : "") + ">Starfield</option>"
      "<option value=\"Toasters\""  + String((screensaver == "Toasters")  ? " selected" : "") + ">Toasters</option>";

    String tz_select_items =
      "<option value=\"Etc/GMT+12\""                     + String((String(timezone) == "Etc/GMT+12")                     ? " selected" : "") + ">(GMT-12:00) International Date Line West</option>"
      "<option value=\"Pacific/Midway\""                 + String((String(timezone) == "Pacific/Midway")                 ? " selected" : "") + ">(GMT-11:00) Midway Island, Samoa</option>"
      "<option value=\"Pacific/Honolulu\""               + String((String(timezone) == "Pacific/Honolulu")               ? " selected" : "") + ">(GMT-10:00) Hawaii</option>"
      "<option value=\"America/Anchorage\""              + String((String(timezone) == "America/Anchorage")              ? " selected" : "") + ">(GMT-09:00) Alaska</option>"
      "<option value=\"America/Los_Angeles\""            + String((String(timezone) == "America/Los_Angeles")            ? " selected" : "") + ">(GMT-08:00) Pacific Time (US & Canada)</option>"
      "<option value=\"America/Tijuana\""                + String((String(timezone) == "America/Tijuana")                ? " selected" : "") + ">(GMT-08:00) Tijuana, Baja California</option>"
      "<option value=\"America/Phoenix\""                + String((String(timezone) == "America/Phoenix")                ? " selected" : "") + ">(GMT-07:00) Arizona</option>"
      "<option value=\"America/Chihuahua\""              + String((String(timezone) == "America/Chihuahua")              ? " selected" : "") + ">(GMT-07:00) Chihuahua, La Paz, Mazatlan</option>"
      "<option value=\"America/Denver\""                 + String((String(timezone) == "America/Denver")                 ? " selected" : "") + ">(GMT-07:00) Mountain Time (US & Canada)</option>"
      "<option value=\"America/Managua\""                + String((String(timezone) == "America/Managua")                ? " selected" : "") + ">(GMT-06:00) Central America</option>"
      "<option value=\"America/Chicago\""                + String((String(timezone) == "America/Chicago")                ? " selected" : "") + ">(GMT-06:00) Central Time (US & Canada)</option>"
      "<option value=\"America/Mexico_City\""            + String((String(timezone) == "America/Mexico_City")            ? " selected" : "") + ">(GMT-06:00) Guadalajara, Mexico City, Monterrey</option>"
      "<option value=\"Canada/Saskatchewan\""            + String((String(timezone) == "Canada/Saskatchewan")            ? " selected" : "") + ">(GMT-06:00) Saskatchewan</option>"
      "<option value=\"America/Bogota\""                 + String((String(timezone) == "America/Bogota")                 ? " selected" : "") + ">(GMT-05:00) Bogota, Lima, Quito, Rio Branco</option>"
      "<option value=\"America/New_York\""               + String((String(timezone) == "America/New_York")               ? " selected" : "") + ">(GMT-05:00) Eastern Time (US & Canada)</option>"
      "<option value=\"America/Indiana/Knox\""           + String((String(timezone) == "America/Indiana/Knox")           ? " selected" : "") + ">(GMT-05:00) Indiana (East)</option>"
      "<option value=\"Canada/Atlantic\""                + String((String(timezone) == "Canada/Atlantic")                ? " selected" : "") + ">(GMT-04:00) Atlantic Time (Canada)</option>"
      "<option value=\"America/Caracas\""                + String((String(timezone) == "America/Caracas")                ? " selected" : "") + ">(GMT-04:00) Caracas, La Paz</option>"
      "<option value=\"America/Manaus\""                 + String((String(timezone) == "America/Manaus")                 ? " selected" : "") + ">(GMT-04:00) Manaus</option>"
      "<option value=\"America/Santiago\""               + String((String(timezone) == "America/Santiago")               ? " selected" : "") + ">(GMT-04:00) Santiago</option>"
      "<option value=\"Canada/Newfoundland\""            + String((String(timezone) == "Canada/Newfoundland")            ? " selected" : "") + ">(GMT-03:30) Newfoundland</option>"
      "<option value=\"America/Sao_Paulo\""              + String((String(timezone) == "America/Sao_Paulo")              ? " selected" : "") + ">(GMT-03:00) Brasilia</option>"
      "<option value=\"America/Argentina/Buenos_Aires\"" + String((String(timezone) == "America/Argentina/Buenos_Aires") ? " selected" : "") + ">(GMT-03:00) Buenos Aires, Georgetown</option>"
      "<option value=\"America/Godthab\""                + String((String(timezone) == "America/Godthab")                ? " selected" : "") + ">(GMT-03:00) Greenland</option>"
      "<option value=\"America/Montevideo\""             + String((String(timezone) == "America/Montevideo")             ? " selected" : "") + ">(GMT-03:00) Montevideo</option>"
      "<option value=\"America/Noronha\""                + String((String(timezone) == "America/Noronha")                ? " selected" : "") + ">(GMT-02:00) Mid-Atlantic</option>"
      "<option value=\"Atlantic/Cape_Verde\""            + String((String(timezone) == "Atlantic/Cape_Verde")            ? " selected" : "") + ">(GMT-01:00) Cape Verde Is.</option>"
      "<option value=\"Atlantic/Azores\""                + String((String(timezone) == "Atlantic/Azores")                ? " selected" : "") + ">(GMT-01:00) Azores</option>"
      "<option value=\"Africa/Casablanca\""              + String((String(timezone) == "Africa/Casablanca")              ? " selected" : "") + ">(GMT+00:00) Casablanca, Monrovia, Reykjavik</option>"
      "<option value=\"Etc/Greenwich\""                  + String((String(timezone) == "Etc/Greenwich")                  ? " selected" : "") + ">(GMT+00:00) Greenwich Mean Time : Dublin, Edinburgh, Lisbon, London</option>"
      "<option value=\"Europe/Amsterdam\""               + String((String(timezone) == "Europe/Amsterdam")               ? " selected" : "") + ">(GMT+01:00) Amsterdam, Berlin, Bern, Rome, Stockholm, Vienna</option>"
      "<option value=\"Europe/Belgrade\""                + String((String(timezone) == "Europe/Belgrade")                ? " selected" : "") + ">(GMT+01:00) Belgrade, Bratislava, Budapest, Ljubljana, Prague</option>"
      "<option value=\"Europe/Brussels\""                + String((String(timezone) == "Europe/Brussels")                ? " selected" : "") + ">(GMT+01:00) Brussels, Copenhagen, Madrid, Paris</option>"
      "<option value=\"Europe/Sarajevo\""                + String((String(timezone) == "Europe/Sarajevo")                ? " selected" : "") + ">(GMT+01:00) Sarajevo, Skopje, Warsaw, Zagreb</option>"
      "<option value=\"Africa/Lagos\""                   + String((String(timezone) == "Africa/Lagos")                   ? " selected" : "") + ">(GMT+01:00) West Central Africa</option>"
      "<option value=\"Asia/Amman\""                     + String((String(timezone) == "Asia/Amman")                     ? " selected" : "") + ">(GMT+02:00) Amman</option>"
      "<option value=\"Europe/Athens\""                  + String((String(timezone) == "Europe/Athens")                  ? " selected" : "") + ">(GMT+02:00) Athens, Bucharest, Istanbul</option>"
      "<option value=\"Asia/Beirut\""                    + String((String(timezone) == "Asia/Beirut")                    ? " selected" : "") + ">(GMT+02:00) Beirut</option>"
      "<option value=\"Africa/Cairo\""                   + String((String(timezone) == "Africa/Cairo")                   ? " selected" : "") + ">(GMT+02:00) Cairo</option>"
      "<option value=\"Africa/Harare\""                  + String((String(timezone) == "Africa/Harare")                  ? " selected" : "") + ">(GMT+02:00) Harare, Pretoria</option>"
      "<option value=\"Europe/Helsinki\""                + String((String(timezone) == "Europe/Helsinki")                ? " selected" : "") + ">(GMT+02:00) Helsinki, Kyiv, Riga, Sofia, Tallinn, Vilnius</option>"
      "<option value=\"Asia/Jerusalem\""                 + String((String(timezone) == "Asia/Jerusalem")                 ? " selected" : "") + ">(GMT+02:00) Jerusalem</option>"
      "<option value=\"Europe/Minsk\""                   + String((String(timezone) == "Europe/Minsk")                   ? " selected" : "") + ">(GMT+02:00) Minsk</option>"
      "<option value=\"Africa/Windhoek\""                + String((String(timezone) == "Africa/Windhoek")                ? " selected" : "") + ">(GMT+02:00) Windhoek</option>"
      "<option value=\"Asia/Kuwait\""                    + String((String(timezone) == "Asia/Kuwait")                    ? " selected" : "") + ">(GMT+03:00) Kuwait, Riyadh, Baghdad</option>"
      "<option value=\"Europe/Moscow\""                  + String((String(timezone) == "Europe/Moscow")                  ? " selected" : "") + ">(GMT+03:00) Moscow, St. Petersburg, Volgograd</option>"
      "<option value=\"Africa/Nairobi\""                 + String((String(timezone) == "Africa/Nairobi")                 ? " selected" : "") + ">(GMT+03:00) Nairobi</option>"
      "<option value=\"Asia/Tbilisi\""                   + String((String(timezone) == "Asia/Tbilisi")                   ? " selected" : "") + ">(GMT+03:00) Tbilisi</option>"
      "<option value=\"Asia/Tehran\""                    + String((String(timezone) == "Asia/Tehran")                    ? " selected" : "") + ">(GMT+03:30) Tehran</option>"
      "<option value=\"Asia/Muscat\""                    + String((String(timezone) == "Asia/Muscat")                    ? " selected" : "") + ">(GMT+04:00) Abu Dhabi, Muscat</option>"
      "<option value=\"Asia/Baku\""                      + String((String(timezone) == "Asia/Baku")                      ? " selected" : "") + ">(GMT+04:00) Baku</option>"
      "<option value=\"Asia/Yerevan\""                   + String((String(timezone) == "Asia/Yerevan")                   ? " selected" : "") + ">(GMT+04:00) Yerevan</option>"
      "<option value=\"Asia/Kabul\""                     + String((String(timezone) == "Asia/Kabul")                     ? " selected" : "") + ">(GMT+04:30) Kabul</option>"
      "<option value=\"Asia/Yekaterinburg\""             + String((String(timezone) == "Asia/Yekaterinburg")             ? " selected" : "") + ">(GMT+05:00) Yekaterinburg</option>"
      "<option value=\"Asia/Karachi\""                   + String((String(timezone) == "Asia/Karachi")                   ? " selected" : "") + ">(GMT+05:00) Islamabad, Karachi, Tashkent</option>"
      "<option value=\"Asia/Calcutta\""                  + String((String(timezone) == "Asia/Calcutta")                  ? " selected" : "") + ">(GMT+05:30) Chennai, Kolkata, Mumbai, New Delhi</option>"
      "<option value=\"Asia/Katmandu\""                  + String((String(timezone) == "Asia/Katmandu")                  ? " selected" : "") + ">(GMT+05:45) Kathmandu</option>"
      "<option value=\"Asia/Almaty\""                    + String((String(timezone) == "Asia/Almaty")                    ? " selected" : "") + ">(GMT+06:00) Almaty, Novosibirsk</option>"
      "<option value=\"Asia/Dhaka\""                     + String((String(timezone) == "Asia/Dhaka")                     ? " selected" : "") + ">(GMT+06:00) Astana, Dhaka</option>"
      "<option value=\"Asia/Rangoon\""                   + String((String(timezone) == "Asia/Rangoon")                   ? " selected" : "") + ">(GMT+06:30) Yangon (Rangoon)</option>"
      "<option value=\"Asia/Bangkok\""                   + String((String(timezone) == "Asia/Bangkok")                   ? " selected" : "") + ">(GMT+07:00) Bangkok, Hanoi, Jakarta</option>"
      "<option value=\"Asia/Krasnoyarsk\""               + String((String(timezone) == "Asia/Krasnoyarsk")               ? " selected" : "") + ">(GMT+07:00) Krasnoyarsk</option>"
      "<option value=\"Asia/Hong_Kong\""                 + String((String(timezone) == "Asia/Hong_Kong")                 ? " selected" : "") + ">(GMT+08:00) Beijing, Chongqing, Hong Kong, Urumqi</option>"
      "<option value=\"Asia/Kuala_Lumpur\""              + String((String(timezone) == "Asia/Kuala_Lumpur")              ? " selected" : "") + ">(GMT+08:00) Kuala Lumpur, Singapore</option>"
      "<option value=\"Asia/Irkutsk\""                   + String((String(timezone) == "Asia/Irkutsk")                   ? " selected" : "") + ">(GMT+08:00) Irkutsk, Ulaan Bataar</option>"
      "<option value=\"Australia/Perth\""                + String((String(timezone) == "Australia/Perth")                ? " selected" : "") + ">(GMT+08:00) Perth</option>"
      "<option value=\"Asia/Taipei\""                    + String((String(timezone) == "Asia/Taipei")                    ? " selected" : "") + ">(GMT+08:00) Taipei</option>"
      "<option value=\"Asia/Tokyo\""                     + String((String(timezone) == "Asia/Tokyo")                     ? " selected" : "") + ">(GMT+09:00) Osaka, Sapporo, Tokyo</option>"
      "<option value=\"Asia/Seoul\""                     + String((String(timezone) == "Asia/Seoul")                     ? " selected" : "") + ">(GMT+09:00) Seoul</option>"
      "<option value=\"Asia/Yakutsk\""                   + String((String(timezone) == "Asia/Yakutsk")                   ? " selected" : "") + ">(GMT+09:00) Yakutsk</option>"
      "<option value=\"Australia/Adelaide\""             + String((String(timezone) == "Australia/Adelaide")             ? " selected" : "") + ">(GMT+09:30) Adelaide</option>"
      "<option value=\"Australia/Darwin\""               + String((String(timezone) == "Australia/Darwin")               ? " selected" : "") + ">(GMT+09:30) Darwin</option>"
      "<option value=\"Australia/Brisbane\""             + String((String(timezone) == "Australia/Brisbane")             ? " selected" : "") + ">(GMT+10:00) Brisbane</option>"
      "<option value=\"Australia/Canberra\""             + String((String(timezone) == "Australia/Canberra")             ? " selected" : "") + ">(GMT+10:00) Canberra, Melbourne, Sydney</option>"
      "<option value=\"Australia/Hobart\""               + String((String(timezone) == "Australia/Hobart")               ? " selected" : "") + ">(GMT+10:00) Hobart</option>"
      "<option value=\"Pacific/Guam\""                   + String((String(timezone) == "Pacific/Guam")                   ? " selected" : "") + ">(GMT+10:00) Guam, Port Moresby</option>"
      "<option value=\"Asia/Vladivostok\""               + String((String(timezone) == "Asia/Vladivostok")               ? " selected" : "") + ">(GMT+10:00) Vladivostok</option>"
      "<option value=\"Asia/Magadan\""                   + String((String(timezone) == "Asia/Magadan")                   ? " selected" : "") + ">(GMT+11:00) Magadan, Solomon Is., New Caledonia</option>"
      "<option value=\"Pacific/Auckland\""               + String((String(timezone) == "Pacific/Auckland")               ? " selected" : "") + ">(GMT+12:00) Auckland, Wellington</option>"
      "<option value=\"Pacific/Fiji\""                   + String((String(timezone) == "Pacific/Fiji")                   ? " selected" : "") + ">(GMT+12:00) Fiji, Kamchatka, Marshall Is.</option>"
      "<option value=\"Pacific/Tongatapu\""              + String((String(timezone) == "Pacific/Tongatapu")              ? " selected" : "") + ">(GMT+13:00) Nuku'alofa</option>";

    server.send(
      200,
      F("text/html"),
      "<html><head><title>web2rgbmatrix - Settings</title><script>function myFunction() {var x = document.getElementById(\"idPassword\");if (x.type === \"password\") {x.type = \"text\";} else {x.type = \"password\";}}</script>" + style + "</head><body><form action=\"/settings\"><h1>Settings</h1><p><h3>Wifi Client Settings</h3>SSID<br><input type=\"text\" name=\"ssid\" value=\"" + String(ssid) + "\"><br>Password<br><input type=\"password\" name=\"password\" id=\"idPassword\" value=\"" + String(password) + "\"><br><div><label for=\"showpass\" class=\"chkboxlabel\"><input type=\"checkbox\"id=\"showpass\" onclick=\"myFunction()\"> Show Password</label></div></p><p><h3>Display Settings</h3><label for=\"textcolor\">Text Color</label><input type=\"color\" id=\"textcolor\" name=\"textcolor\" value=\"" + textcolor + "\"><label for=\"brightness\">LED Brightness</label><input type=\"number\" id=\"brightness\" name=\"brightness\" min=\"0\" max=\"255\" value=" + matrix_brightness + "><label for=\"playback\">GIF Playback</label><br><br><select id=\"playback\" name=\"playback\" value=\"" + playback + "\">" + playback_select_items + "</select><h3>Screen Saver Settings</h3><label for=\"screensaver\">Screen Saver</label><br><br><select id=\"screensaver\" name=\"screensaver\" value=\"" + screensaver + "\">" + saver_select_items + "</select><br><br><label for=\"accentcolor\">Accent Color</label><input type=\"color\" id=\"accentcolor\" name=\"accentcolor\" value=\"" + accentcolor + "\"><label for=\"timeout\">Client Timeout(Minutes)</label><input type=\"number\" id=\"timeout\" name=\"timeout\" min=\"0\" max=\"60\" value=" + (ping_fail_count / 2) + "><label for=\"timezone\">Timezone</label><br><br><select id=\"timezone\" name=\"timezone\" value=\"" + String(timezone) + "\">" + tz_select_items + "</select><br><br><label for=\"twelvehour\" class=\"chkboxlabel\"><input type=\"checkbox\" id=\"twelvehour\" name=\"twelvehour\" value=\"true\"" + (twelvehour ? " checked=\"checked\"" : "") + "/> 12hr Format</label></p><input type=\"submit\" class=actionbtn value=\"Save\"><br><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n"
    );

    return;
  }

  // Jeżeli zostały uzupełnione, zapisujemy ustawienia

  server.arg("ssid").toCharArray(ssid, sizeof(ssid));
  server.arg("password").toCharArray(password, sizeof(password));
  if (server.arg("timeout") == "0") {
    ping_fail_count = 0;
  } else {
    ping_fail_count = (server.arg("timeout").toInt() * 2);
  }
  if (server.arg("brightness") == "0") {
    matrix_brightness = 0;
  } else {
    matrix_brightness = (server.arg("brightness").toInt());
  }
  matrix_display->setBrightness8(matrix_brightness);
  textcolor = server.arg("textcolor");
  textcolor.replace("%23", "#");
  playback = server.arg("playback");
  screensaver = server.arg("screensaver");
  accentcolor = server.arg("accentcolor");
  accentcolor.replace("%23", "#");
  String tz = server.arg("timezone");
  tz.replace("%2F", "/");
  tz.toCharArray(timezone, sizeof(timezone));
  if (server.arg("twelvehour") == "true") {
    twelvehour = true;
  } else {
    twelvehour = false;
  }

  StaticJsonDocument<512> doc;
  doc["ssid"] = server.arg("ssid");
  doc["password"] = server.arg("password");
  doc["timeout"] = (server.arg("timeout").toInt() * 2);
  doc["brightness"] = server.arg("brightness").toInt();
  doc["textcolor"] = textcolor;
  doc["playback"] = playback;
  doc["screensaver"] = screensaver;
  doc["accentcolor"] = accentcolor;
  doc["timezone"] = timezone;
  doc["twelvehour"] = twelvehour;

  File config_file = LittleFS.open(config_filename, FILE_WRITE);

  if (!config_file) {
    server.send(500, F("text/plain"), "[ERROR] Failed to open config file for writing\r\n");
    return;
  }

  serializeJson(doc, config_file);

  server.send(200, F("text/html"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Settings Saved</title>" + style + "</head><body><form><p>Settings saved, you must reboot for WiFi and timezone changes to take effect.</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/settings';\" value=\"Back\" /><input type=\"button\" class=cautionbtn onclick=\"location.href='/reboot';\" value=\"Reboot\" /></form></body></html>\r\n");
}

// Endpoint: /list
// Description:
// Usage:
void handleFileList() {
  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  if (!server.hasArg("dir") && !server.hasArg("file")) {
    server.send(500, F("text/plain"), "[ERROR] Required args not specified: file, dir\r\n");
    return;
  }

  if (server.hasArg("dir")) {
    String path = server.arg("dir");
    File root = SD.open(path);
    path = String();

    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent("<html><head><title>web2rgbmatrix - File Browser</title>" + style + "</head><body><form><h1>File Browser:</h1><h2>" + server.arg("dir") + "</h2><p><table>");

    if (root.isDirectory()) {
      if (server.arg("dir") != "/") {
        String parent = server.arg("dir");
        parent.replace(String(root.name()), "");
        if (parent != "/" && parent.endsWith("/")) {
          parent.remove(parent.length() - 1, 1);
        }
        server.sendContent("<tr><td><a href=\"/list?dir=" + parent + "\">../</a></td></tr>");
      }

      File file = root.openNextFile();
      while (file) {
        if (!String(file.name()).startsWith(".")) {
          server.sendContent("<tr><td><a href=\"/list?" + String((file.isDirectory()) ? "dir=" : "file=") + String(file.path()) + "\">" + String(file.name()) + String((file.isDirectory()) ? "/" : "") + "</a></td></tr>");
        }
        file = root.openNextFile();
      }
    }

    server.sendContent("</table></p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>");
    server.sendContent(F(""));
    server.client().stop();
  } else if (server.hasArg("file")) {
    String path = server.arg("file");
    File file = SD.open(path);
    path = String();

    if (!file) {
      server.send(
        404,
        F("text/plain"),
        "[ERROR] File Not Found\r\n");
      return;
    }

    file.close();

    server.send(
      200,
      F("text/html"),
      "<html><head><title>web2rgbmatrix - File Browser</title>" + style + "</head><body><form><h1>File Browser:</h1><h3>" + server.arg("file") + "</h3><table><p><tr><td>Image</td><td><img src=\"" + server.arg("file") + "\" /></td></tr><tr><td>Size</td><td>" + ((file.size() >= 1024) ? String(file.size() / 1024) + "KB" : String(file.size()) + "B") + "</td></tr></table></p><input id='play-button' type=\"button\" class=actionbtn onclick=\"location.href='/sdplay?file=" + server.arg("file") + "';\" value=\"Play\" /><input id='delete-button' type=\"button\" class=cautionbtn onclick=\"location.href='/delete?file=" + server.arg("file") + "';\" value=\"Delete\" /><input id='back-button' type=\"button\" class=btn onclick=\"history.back()\" value=\"Back\" /></form></body></html>\r\n");
  }
}

// Endpoint: /delete
// Description:
// Usage:
void handleFileDelete() {
  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  if (!server.hasArg("file")) {
    server.send(500, F("text/plain"), "[ERROR] Required args not specified: file\r\n");
    return;
  }

  File file = SD.open(server.arg("file"));

  if (!file) {
    server.send(404, F("text/plain"), "[ERROR] File Not Found\r\n");
    return;
  }

  SD.remove(server.arg("file"));

  server.send(200, F("text/html"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>web2rgbmatrix - File Delete</title>" + style + "</head><body><form><h1>File Deleted: " + server.arg("file") + "</h1><input id='back-button' type=\"button\" class=btn onclick=\"window.history.go(-2)\" value=\"Back\" /></form></body></html>\r\n");
}

// Endpoint: /sdcard
// Description:
// Usage:
void handleSD() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  server.send(
    200,
    F("text/html"),
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>web2rgbmatrix - GIF Upload</title>" + style + "</head><body>" + "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>" + "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'><h1>GIF Upload</h1><input type='file' name='update' id='file' onchange='sub(this)' style=display:none><label id='file-input' for='file'>Choose file...</label><div><label for=\"animated\" class=\"chkboxlabel\"><input type=\"checkbox\" name=\"animated\" id=\"animated\">Animated GIF</label></div><input id='sub-button' type='submit' class=actionbtn value='Upload'><br><br><div id='prg'></div><br><div id='prgbar'><div id='bar'></div></div><br><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form>" + "<script>function sub(obj){var fileName = obj.value.split('\\\\');document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];};$('form').submit(function(e){e.preventDefault();url = '/upload';if (document.getElementById('animated').checked) {url = '/upload?animated=true';}var form = $('#upload_form')[0];var data = new FormData(form);$.ajax({url: url,type: 'POST',data: data,contentType: false,processData: false,xhr: function() {var xhr = new window.XMLHttpRequest();xhr.upload.addEventListener('progress', function(evt) {if (evt.lengthComputable) {var per = evt.loaded / evt.total;if (evt.loaded != evt.total) {$('#sub-button').prop('disabled', true);$('#back-button').prop('disabled', true);$('#prg').html('Upload Progress: ' + Math.round(per*100) + '%');} else {$('#sub-button').prop('disabled', false);$('#back-button').prop('disabled', false);var inputFile = $('form').find(\"input[type=file]\");var fileName = inputFile[0].files[0].name;var folder = " + String(gif_folder) + ";if (document.getElementById('animated').checked) {folder = " + String(animated_gif_folder) + ";}$('#prg').html('Upload Success, click GIF to play<br><a href=\"/sdplay?file=' + folder + fileName.charAt(0).toUpperCase() + '/' + fileName +'\"><img src=\"' + folder + fileName.charAt(0) + '/' + fileName + '\"></a>');}$('#bar').css('width',Math.round(per*100) + '%');}}, false);return xhr;},success:function(d, s) {console.log('success!') },error: function (a, b, c) {}});});</script>" + "</body></html>\r\n"
  );
}

// Endpoint: /upload
// Description:
// Usage:
void handleUpload() {
  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  HTTPUpload &uploadfile = server.upload();

  if (uploadfile.status == UPLOAD_FILE_START) {
    String filename = String(uploadfile.filename);
    String folder = String(gif_folder);

    if (server.arg("animated") != "") {
      folder = String(animated_gif_folder);
    }

    if (!filename.startsWith("/")) {
      String letter_folder(filename.charAt(0));
      letter_folder.toUpperCase();
      filename = folder + String(letter_folder) + "/" + String(uploadfile.filename);
    }

    SD.remove(filename);
    upload_file = SD.open(filename, FILE_WRITE);
    filename = String();

    return;
  }

  if (uploadfile.status == UPLOAD_FILE_WRITE) {
    if (!upload_file) {
      return;
    }
    upload_file.write(uploadfile.buf, uploadfile.currentSize);
  }

  if (uploadfile.status == UPLOAD_FILE_END) {
    if (!upload_file) {
      server.send(500, F("text/plain"), "[ERROR] Couldn't create file\r\n");
      return;
    }

    upload_file.close();
    server.send(200, F("text/html"), "[OK] File Uploaded\r\n");
  }
}

// Endpoint: /remoteplay
// Description:
// Usage:
void handleRemotePlay() {
  static long contentLength = 0;
  HTTPUpload &uploadfile = server.upload();
  contentLength = server.header("Content-Length").toInt();

  if (uploadfile.status == UPLOAD_FILE_START && !uploading) {
    if (contentLength > (LittleFS.totalBytes() - LittleFS.usedBytes())) {
      Serial.printf("[ERROR] UPLOAD_FILE_START - File to upload via /remoteplay was too large! (%s)\n",  (String(contentLength) + " > " + String(LittleFS.totalBytes() - LittleFS.usedBytes())).c_str());
      server.send(500, F("text/plain"), "[ERROR] File too large\r\n");
      uploading = false;
      return;
    }

    Serial.printf("[OK] UPLOAD_FILE_START\n");

    uploading = true;
    LittleFS.remove(gif_filename);
    upload_file = LittleFS.open(gif_filename, FILE_WRITE);
    return;
  }

  if (uploadfile.status == UPLOAD_FILE_WRITE) {
    if (!upload_file) {
      uploading = false;
      Serial.printf("[ERROR] UPLOAD_FILE_WRITE\n");
      server.send(500, F("text/plain"), "[ERROR] Couldn't write file\r\n");
      return;
    }

    Serial.printf("[OK] UPLOAD_FILE_WRITE\n");

    uploading = true;
    upload_file.write(uploadfile.buf, uploadfile.currentSize);
    return;
  }

  if (uploadfile.status == UPLOAD_FILE_END) {
    if (!upload_file) {
      uploading = false;
      Serial.printf("[ERROR] UPLOAD_FILE_END\n");
      server.send(500, F("text/plain"), "[ERROR] Couldn't create file\r\n");
      return;
    }

    Serial.printf("[OK] UPLOAD_FILE_END\n");
    upload_file.close();

    client_ip = server.client().remoteIP();
    ping_fail = 0;
    sd_filename = "";
    tty_client = false;
    uploading = false;

    setGIF(gif_filename, false);

    server.send(200, F("text/html"), "[OK] SUCCESS\r\n");
    return;
  }

  if (uploadfile.status == UPLOAD_FILE_ABORTED) {
    Serial.printf("[ERROR] UPLOAD_FILE_ABORTED\n");
    uploading = false;
    server.send(409, F("text/plain"), "[ERROR] File upload via /remoteplay was aborted!\r\n");
    return;
  }
}

// Endpoint: /localplay
// Description: Play a GIF from SD card with CURL
// Usage: curl http://rgbmatrix.local/localplay?file=MENU
void handleLocalPlay() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Unsupported HTTP method - /localplay supports only GET method!\r\n");
    return;
  }

  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  if (server.arg("file") == "") {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  LittleFS.remove(gif_filename);

  client_ip = server.client().remoteIP();
  ping_fail = 0;
  tty_client = false;

  String letter_folder(server.arg("file").charAt(0));
  letter_folder.toUpperCase();

  bool gif_found = false;

  if (playback != "Static") {
    String agif_fullpath = String(animated_gif_folder) + String(letter_folder) + "/" + server.arg("file") + ".gif";
    const char *agif_requested_filename = agif_fullpath.c_str();

    if (SD.exists(agif_requested_filename)) {
      sd_filename = agif_fullpath;
      gif_found = true;

      setGIF(agif_requested_filename, true);

      server.send(200, F("text/html"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Local Play</title>" + style + "</head><body><form><p>Displaying GIF: " + sd_filename + "</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>");
    }
  }

  if (playback != "Animated") {
    String fullpath = String(gif_folder) + String(letter_folder) + "/" + server.arg("file") + ".gif";
    const char *requested_filename = fullpath.c_str();

    if (SD.exists(requested_filename)) {
      sd_filename = fullpath;
      gif_found = true;

      setGIF(requested_filename, true);

      server.send(200, F("text/html"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Local Play</title>" + style + "</head><body><form><p>Displaying GIF: " + sd_filename + "</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>");
    }
  }

  if (!gif_found) {
    sd_filename = "";

    showTextLine(server.arg("file"));

    server.send(404, F("text/plain"), "[ERROR] File Not Found\r\n");
  }
}

// Endpoint: /sdplay
// Description: Play a GIF from SD card with CURL
// Usage: curl http://rgbmatrix.local/sdplay?file=/gifs/M/MENU.gif
void handleSDPlay() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  if (!card_mounted) {
    server.send(500, F("text/plain"), "[ERROR] SD Card Not Mounted\r\n");
    return;
  }

  if (server.arg("file") == "") {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  LittleFS.remove(gif_filename);

  client_ip = server.client().remoteIP();
  ping_fail = 0;
  tty_client = false;

  if (!SD.exists(server.arg("file"))) {
    server.send(404, F("text/plain"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>SD Play</title>" + style + "</head><body><form><p>" + "File Not Found: " + server.arg("file") + "</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n");
    return;
  }

  sd_filename = server.arg("file");

  const char *requested_filename = server.arg("file").c_str();
  setGIF(requested_filename, true);

  server.send(200, F("text/html"), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>SD Play</title>" + style + "</head><body><form><p>" + "Displaying SD File: " + server.arg("file") + "</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n");
}

// Endpoint: /text
// Description: Display a line of text with CURL
// Usage: curl http://rgbmatrix.local/text?line=Text
void handleText() {
  if (server.method() != HTTP_GET) {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  if (server.arg("line") == "") {
    server.send(405, F("text/plain"), "[ERROR] Method Not Allowed\r\n");
    return;
  }

  LittleFS.remove(gif_filename);
  showTextLine(server.arg("line"));

  client_ip = server.client().remoteIP();
  ping_fail = 0;
  tty_client = false;

  server.send(
    200,
    F("text/html"),
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>SD Play</title>" + style + "</head><body><form><p>Displaying Text: " + server.arg("line") + "</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n");
}

// Endpoint: /version
void handleVersion() {
  server.send(
    200,
    F("text/html"), String(VERSION) + "\r\n");
}

// Endpoint: /clear
// Description:
// Usage:
void handleClear() {
  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);

  LittleFS.remove(gif_filename);

  client_ip = { 0, 0, 0, 0 };
  config_display_on = false;
  tty_client = false;

  sd_filename = "";

  server.send(
    200,
    F("text/html"),
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Display Cleared</title><meta http-equiv=\"refresh\" content=\"3;URL=\'/\'\" />" + style + "</head><body><form><p>Display Cleared</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n");
}

// Endpoint: /reboot
// Description:
// Usage:
void handleReboot() {
  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);

  LittleFS.remove(gif_filename);

  server.send(
    200,
    F("text/html"),
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Rebooting...</title><meta http-equiv=\"refresh\" content=\"60;URL=\'/\'\" />" + style + "</head><body><form><p>Rebooting...</p><input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" /></form></body></html>\r\n");

  ESP.restart();
}

void handleNotFound() {
  String path = server.uri();
  String data_type = "text/plain";
  if (path.endsWith("/")) {
    path += "index.html";
  }
  if (path.endsWith(".src")) {
    path = path.substring(0, path.lastIndexOf("."));
  } else if (path.endsWith(".htm")) {
    data_type = "text/html";
  } else if (path.endsWith(".html")) {
    data_type = "text/html";
  } else if (path.endsWith(".css")) {
    data_type = "text/css";
  } else if (path.endsWith(".js")) {
    data_type = "application/javascript";
  } else if (path.endsWith(".png")) {
    data_type = "image/png";
  } else if (path.endsWith(".gif")) {
    data_type = "image/gif";
  } else if (path.endsWith(".jpg")) {
    data_type = "image/jpeg";
  } else if (path.endsWith(".ico")) {
    data_type = "image/x-icon";
  } else if (path.endsWith(".xml")) {
    data_type = "text/xml";
  } else if (path.endsWith(".pdf")) {
    data_type = "application/pdf";
  } else if (path.endsWith(".zip")) {
    data_type = "application/zip";
  }

  File data_file;
  if (LittleFS.exists(path.c_str())) {
    data_file = LittleFS.open(path.c_str());
  } else if (card_mounted) {
    if (SD.exists(path.c_str())) {
      data_file = SD.open(path.c_str());
    } else {
      server.send(404, F("text/plain"), "[ERROR] File Not Found\r\n");
    }
  }

  if (!data_file) {
    server.send(500, F("text/plain"), "[ERROR] Couldn't open file\r\n");
  }

  if (server.streamFile(data_file, data_type) != data_file.size()) {
    Serial.printf("[WARNING] Sent less data than expected!\n");
  }

  data_file.close();
}

void checkClientTimeout() {
  if (client_ip != no_ip) {
    if (ping_fail_count != 0) {
      if (ping_fail <= ping_fail_count) {
        if (millis() - last_seen >= 60 * 1000UL) {
          last_seen = millis();
          bool success = Ping.ping(client_ip, 1);
          if (!success) {
            Serial.printf("[INFO] Ping failed");
            ping_fail = ping_fail + 1;
          } else {
            Serial.printf("[INFO] Ping success");
            ping_fail = 0;
          }
        }
      } else {
        Serial.printf("[INFO] Client gone, clearing display and deleting the GIF.\n");
        matrix_display->clearScreen();
        matrix_display->setBrightness8(matrix_brightness);
        client_ip = { 0, 0, 0, 0 };
        LittleFS.remove(gif_filename);
        sd_filename = "";
      }
    }
  } else {
    if (config_display_on == false && tty_client == false) {
      if (screensaver == "Clock") {
        clockScreenSaver();
      }
      else if (screensaver == "Plasma") {
        plasmaScreenSaver();
      }
      else if (screensaver == "Starfield") {
        starfieldScreenSaver();
      }
      else if (screensaver == "Toasters") {
        toasterScreenSaver();
      }
    }
  }
}

void checkSerialClient() {
  if (Serial.available()) {
    new_command = Serial.readStringUntil('\n');
    new_command.trim();
  }
  if (new_command != current_command) {
    if (new_command.endsWith("QWERTZ")) {
    } else if (new_command.startsWith("CMD")) {
    } else if (new_command == "cls") {
    } else if (new_command == "sorg") {
    } else if (new_command == "bye") {
    } else {
      tty_client = true;
      sd_filename = "";
      LittleFS.remove(gif_filename);
      client_ip = { 0, 0, 0, 0 };
      bool gif_found = false;
      if (card_mounted) {
        if (playback != "Static") {
          String agif_fullpath = String(animated_gif_folder) + new_command.charAt(0) + "/" + new_command + ".gif";
          const char *agif_requested_filename = agif_fullpath.c_str();
          if (SD.exists(agif_requested_filename)) {
            sd_filename = agif_fullpath;
            setGIF(agif_requested_filename, true);
            gif_found = true;
          }
        }
        if (playback != "Animated") {
          String fullpath = String(gif_folder) + new_command.charAt(0) + "/" + new_command + ".gif";
          const char *requested_filename = fullpath.c_str();
          if (SD.exists(requested_filename)) {
            sd_filename = fullpath;
            setGIF(requested_filename, true);
            gif_found = true;
          }
        }
      }
      if (!gif_found) {
        sd_filename = "";
        showTextLine(new_command);
      }
    }
    current_command = new_command;
  }
}

void displaySetup() {
  HUB75_I2S_CFG mxconfig(panelResX, panelResY, panels_in_X_chain);

  mxconfig.gpio.e = E;
  mxconfig.gpio.r1 = R1;
  mxconfig.gpio.r2 = R2;

  if (digitalRead(ALT) == HIGH) {
    mxconfig.gpio.b1 = B1;
    mxconfig.gpio.b2 = B2;
    mxconfig.gpio.g1 = G1;
    mxconfig.gpio.g2 = G2;
  } else {
    mxconfig.gpio.b1 = G1;
    mxconfig.gpio.b2 = G2;
    mxconfig.gpio.g1 = B1;
    mxconfig.gpio.g2 = B2;
  }

  mxconfig.clkphase = false;

  matrix_display = new MatrixPanel_I2S_DMA(mxconfig);
  matrix_display->begin();
  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);
}

void showTextLine(String text) {
  int char_width = 6;
  int char_heigth = 8;
  int x = (totalWidth - (text.length() * char_width)) / 2;
  int y = (totalHeight - char_heigth) / 2;

  String hexcolor = textcolor;
  hexcolor.replace("#", "");
  char charbuf[8];
  hexcolor.toCharArray(charbuf, 8);
  long int rgb = strtol(charbuf, 0, 16);
  byte r = (byte)(rgb >> 16);
  byte g = (byte)(rgb >> 8);
  byte b = (byte)(rgb);

  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);
  matrix_display->setTextColor(matrix_display->color565(r, g, b));
  matrix_display->setCursor(x, y);
  matrix_display->println(text);
}

void showText(String text) {
  String hexcolor = textcolor;
  hexcolor.replace("#", "");
  char charbuf[8];
  hexcolor.toCharArray(charbuf, 8);
  long int rgb = strtol(charbuf, 0, 16);
  byte r = (byte)(rgb >> 16);
  byte g = (byte)(rgb >> 8);
  byte b = (byte)(rgb);

  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);
  matrix_display->setTextColor(matrix_display->color565(r, g, b));
  matrix_display->setCursor(0, 0);
  matrix_display->println(text);
}

/**
 * Description: Copy a horizontal span of pixels from a source buffer to an X, Y position in matrix back buffer, applying horizontal clipping.
   Vertical clipping is handled in GIFDraw() below Y can safely be assumed valid here.
 */
void span(uint16_t *src, int16_t x, int16_t y, int16_t width) {
  // Span entirely off right of matrix
  if (x >= totalWidth) {
    return;
  }

  int16_t x2 = x + width - 1;

  // Span entirely off left of matrix
  if (x2 < 0) {
    return;
  }

  // Span partially off left of matrix
  if (x < 0) {
    width += x;
    src -= x;
    x = 0;
  }

  // Span partially off right of matrix
  if (x2 >= totalWidth) {
    width -= (x2 - totalWidth + 1);
  }

  while (x <= x2) {
    int16_t xOffset = (totalWidth - gif.getCanvasWidth()) / 2;
    matrix_display->drawPixel((x++) + xOffset, y, *src++);
  }
}

// Description: Draw a line of image directly on the matrix
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y;

  y = pDraw->iY + pDraw->y;

  int16_t screenY = yPos + y;
  if ((screenY < 0) || (screenY >= totalHeight)) return;

  usPalette = pDraw->pPalette;

  s = pDraw->pPixels;

  if (pDraw->ucHasTransparency) {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + pDraw->iWidth;
    x = 0;
    iCount = 0;
    while (x < pDraw->iWidth) {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {
          s--;
        } else {
          *d++ = usPalette[c];
          iCount++;
        }
      }
      if (iCount) {
        span(usTemp, xPos + pDraw->iX + x, screenY, iCount);
        x += iCount;
        iCount = 0;
      }
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {
          iCount++;
        }
        else {
          s--;
        }
      }
      if (iCount) {
        x += iCount;
        iCount = 0;
      }
    }
  } else {
    s = pDraw->pPixels;
    for (x = 0; x < pDraw->iWidth; x++) {
      usTemp[x] = usPalette[*s++];
    }
    span(usTemp, xPos + pDraw->iX, screenY, pDraw->iWidth);
  }
}

void *GIFOpenFile(const char *fname, int32_t *pSize) {
  Serial.printf("Playing GIF: %s\n", fname);
  gif_file = LittleFS.open(fname);
  if (gif_file) {
    *pSize = gif_file.size();
    return (void *)&gif_file;
  }
  return NULL;
}

void *GIFSDOpenFile(const char *fname, int32_t *pSize) {
  Serial.printf("Playing GIF from SD: %s\n", fname);
  gif_file = SD.open(fname);
  if (gif_file) {
    *pSize = gif_file.size();
    return (void *)&gif_file;
  }
  return NULL;
}

void GIFCloseFile(void *pHandle) {
  File *gif_file = static_cast<File *>(pHandle);
  if (gif_file != NULL)
    gif_file->close();
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead;
  iBytesRead = iLen;
  File *gif_file = static_cast<File *>(pFile->fHandle);

  if ((pFile->iSize - pFile->iPos) < iLen) {
  	iBytesRead = pFile->iSize - pFile->iPos - 1;
  }

  if (iBytesRead <= 0) {
  	return 0;
  }

  iBytesRead = (int32_t)gif_file->read(pBuf, iBytesRead);
  pFile->iPos = gif_file->position();

  return iBytesRead;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  int i = micros();
  File *gif_file = static_cast<File *>(pFile->fHandle);
  gif_file->seek(iPosition);
  pFile->iPos = (int32_t)gif_file->position();
  i = micros() - i;

  return pFile->iPos;
}

void showGIF(const char *name, bool sd) {
  config_display_on = false;
  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);
  if (sd && card_mounted) {
    if (gif.open(name, GIFSDOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      GIFINFO pInfo;
      gif.getInfo(&pInfo);

      Serial.printf("[INFO] W: %d | H: %d | Frames: %d | Duration: %d ms\n", gif.getCanvasWidth(), gif.getCanvasHeight(), pInfo.iFrameCount, pInfo.iDuration);
      int i = 0;

      do {
        i++;
        Serial.printf("[INFO] PLAYING FRAME: %d / %d\n", i, pInfo.iFrameCount);
      } while (allowPlaying && gif.playFrame(true, NULL));

      if (!allowPlaying) {
        Serial.printf("[OK] PLAYING ABORTED!\n");
        isPlayable = true;
        allowPlaying = true;
      } else {
        isPlayable = false;
      }

      Serial.flush();
      gif.close();
    }
  } else {
    if (gif.open(name, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      GIFINFO pInfo;
      gif.getInfo(&pInfo);

      Serial.printf("[INFO] W: %d | H: %d | Frames: %d | Duration: %d ms\n", gif.getCanvasWidth(), gif.getCanvasHeight(), pInfo.iFrameCount, pInfo.iDuration);
      int i = 0;

      do {
        i++;
        Serial.printf("[INFO] PLAYING FRAME: %d / %d\n", i, pInfo.iFrameCount);
      } while (allowPlaying && gif.playFrame(true, NULL));

      if (!allowPlaying) {
        Serial.printf("[OK] PLAYING ABORTED!\n");
        isPlayable = true;
        allowPlaying = true;
      } else {
        isPlayable = false;
      }

      Serial.flush();
      gif.close();
    }
  }
}

void drawXbm565(int x, int y, int width, int height, const char *xbm, uint16_t color = 0xffff) {
  if (width % 8 != 0) {
    width = ((width / 8) + 1) * 8;
  }
  for (int i = 0; i < width * height; i++) {
    unsigned char charColumn = pgm_read_byte(xbm + i);
    int swap = 0;
    for (int j = 7; j >= 0; j--) {
      int targetX = (i * 8 + swap) % width + x;
      int targetY = (8 * i / (width)) + y;
      if (swap >= 7) {
        swap = 0;
      } else {
        swap++;
      }
      if (bitRead(charColumn, j)) {
        matrix_display->drawPixel(targetX, targetY, color);
      }
    }
  }
}

void showStatus() {
  String hexcolor = accentcolor;
  hexcolor.replace("#", "");
  char charbuf[8];
  hexcolor.toCharArray(charbuf, 8);
  long int rgb = strtol(charbuf, 0, 16);
  byte r = (byte)(rgb >> 16);
  byte g = (byte)(rgb >> 8);
  byte b = (byte)(rgb);
  String display_string = "Wifi: " + wifi_mode + "\n" + my_ip.toString() + "\nrgbmatrix.local\nSD: " + sd_status;
  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);
  matrix_display->setTextColor(matrix_display->color565(r, g, b));
  matrix_display->setCursor(0, 0);
  matrix_display->print(display_string);
  drawXbm565((totalWidth - ICON_WIFI_WIDTH), (totalHeight - ICON_WIFI_HEIGHT), ICON_WIFI_HEIGHT, ICON_WIFI_WIDTH, icon_wifi_bmp, matrix_display->color565(r, g, b));
}

void plasmaScreenSaver() {
  for (int x = 0; x < (totalWidth); x++) {
    for (int y = 0; y < (totalHeight); y++) {
      int16_t v = 0;
      uint8_t wibble = sin8(time_counter);
      v += sin16(x * wibble * 3 + time_counter);
      v += cos16(y * (128 - wibble) + time_counter);
      v += sin16(y * x * cos8(-time_counter) / 8);

      currentColor = ColorFromPalette(currentPalette, (v >> 8) + 127);
      matrix_display->drawPixelRGB888(x, y, currentColor.r, currentColor.g, currentColor.b);
    }
  }

  ++time_counter;
  ++cycles;

  if (cycles >= 1024) {
    time_counter = 0;
    cycles = 0;
    currentPalette = palettes[random(0, sizeof(palettes) / sizeof(palettes[0]))];
  }
}

void starfieldScreenSaver() {
  String hexcolor = accentcolor;
  hexcolor.replace("#", "");
  char charbuf[8];
  hexcolor.toCharArray(charbuf, 8);
  long int rgb = strtol(charbuf, 0, 16);
  byte r = (byte)(rgb >> 16);
  byte g = (byte)(rgb >> 8);
  byte b = (byte)(rgb);

  bufferClear(matrix_buffer);

  int origin_x = (totalWidth) / 2;
  int origin_y = (totalHeight) / 2;

  for (int i = 0; i < starCount; ++i) {
    stars[i][2] -= 0.19;
    if (stars[i][2] <= 0) {
      stars[i][0] = getRandom(-25, 25);
      stars[i][1] = getRandom(-25, 25);
      stars[i][2] = maxDepth;
    }

    double k = (totalWidth) / stars[i][2];
    int x = static_cast<int>(stars[i][0] * k + origin_x);
    int y = static_cast<int>(stars[i][1] * k + origin_y);

    if ((0 <= x and x < (totalWidth))
        and (0 <= y and y < (totalHeight))) {
      int size = (1 - stars[i][2] / maxDepth) * 4;

      for (int xplus = 0; xplus < size; xplus++) {
        for (int yplus = 0; yplus < size; yplus++) {
          if ((((y + yplus) * (totalWidth) + (x + xplus)) < (totalWidth) * (totalHeight))) {
            matrix_buffer[(y + yplus) * (totalWidth) + (x + xplus)].r = r;
            matrix_buffer[(y + yplus) * (totalWidth) + (x + xplus)].g = g;
            matrix_buffer[(y + yplus) * (totalWidth) + (x + xplus)].b = b;
          }
        }
      }
      matrixFill(matrix_buffer);
    }
  }
}

void clockScreenSaver() {
  unsigned long now = millis();
  if (now > oneSecondLoopDue) {
    setMatrixTime();
    showColon = !showColon;

    if (finishedAnimating) {
      handleColonAfterAnimation();
    }
    oneSecondLoopDue = now + 1000;
  }
  now = millis();
  if (now > animationDue) {
    animationHandler();
    animationDue = now + animationDelay;
  }
}

void toasterScreenSaver() {
  String hexcolor = accentcolor;
  hexcolor.replace("#", "");
  char charbuf[8];
  hexcolor.toCharArray(charbuf, 8);
  long int rgb = strtol(charbuf, 0, 16);
  byte r = (byte)(rgb >> 16);
  byte g = (byte)(rgb >> 8);
  byte b = (byte)(rgb);

  uint8_t i, f;
  int16_t x, y;
  boolean resort = false;

  matrix_display->clearScreen();
  matrix_display->setBrightness8(matrix_brightness);

  for (i = 0; i < N_FLYERS; i++) {
    f = (flyer[i].frame == 255) ? 4 : (flyer[i].frame++ & 3);
    x = flyer[i].x / 16;
    y = flyer[i].y / 16;
    drawXbm565(x, y, 32, 32, (const char *)mask[f], 0x0000);
    drawXbm565(x, y, 32, 32, (const char *)img[f], matrix_display->color565(r, g, b));

    flyer[i].x -= flyer[i].depth * 2;
    flyer[i].y += flyer[i].depth;
    if ((flyer[i].y >= (64 * 16)) || (flyer[i].x <= (-32 * 16))) {
      if (random(7) < 5) {
        flyer[i].x = random(160) * 16;
        flyer[i].y = -32 * 16;
      } else {
        flyer[i].x = 128 * 16;
        flyer[i].y = random(64) * 16;
      }
      flyer[i].frame = random(3) ? random(4) : 255;
      flyer[i].depth = 10 + random(16);
      resort = true;
    }
  }
  if (resort) {
    qsort(flyer, N_FLYERS, sizeof(struct Flyer), compare);
  }
  delay(50);
}

static int compare(const void *a, const void *b) {
  return ((struct Flyer *)a)->depth - ((struct Flyer *)b)->depth;
}

void animationHandler() {
  if (!finishedAnimating) {
    matrix_display->fillScreen(0);
    if (twelvehour) {
      bool tetris1Done = false;
      bool tetris2Done = false;
      bool tetris3Done = false;

      tetris1Done = tetris.drawNumbers(-6 + tetrisXOffset, 10 + tetrisYOffset, showColon);
      tetris2Done = tetris2.drawText(56 + tetrisXOffset, 9 + tetrisYOffset);

      if (tetris2Done) {
        tetris3Done = tetris3.drawText(56 + tetrisXOffset, -1 + tetrisYOffset);
      }

      finishedAnimating = tetris1Done && tetris2Done && tetris3Done;
    } else {
      finishedAnimating = tetris.drawNumbers(2 + tetrisXOffset, 10 + tetrisYOffset, showColon);
    }
    matrix_display->flipDMABuffer();
  }
}

void setMatrixTime() {
  String timeString = "";
  String AmPmString = "";
  if (twelvehour) {
    timeString = myTZ.dateTime("g:i");

    if (timeString.length() == 4) {
      timeString = " " + timeString;
    }

    AmPmString = myTZ.dateTime("A");

    if (lastDisplayedAmPm != AmPmString) {
      lastDisplayedAmPm = AmPmString;
      tetris2.setText("M", forceRefresh);
      tetris3.setText(AmPmString.substring(0, 1), forceRefresh);
    }
  } else {
    timeString = myTZ.dateTime("H:i");
  }

  if (lastDisplayedTime != timeString) {
    lastDisplayedTime = timeString;
    tetris.setTime(timeString, forceRefresh);
    finishedAnimating = false;
  }
}

void handleColonAfterAnimation() {
  uint16_t colour = showColon ? tetris.tetrisWHITE : tetris.tetrisBLACK;
  int x = twelvehour ? -6 : 2;
  x = x + tetrisXOffset;
  int y = 10 + tetrisYOffset - (TETRIS_Y_DROP_DEFAULT * tetris.scale);
  tetris.drawColon(x, y, colour);
  matrix_display->flipDMABuffer();
}

int getRandom(int lower, int upper) {
  return lower + static_cast<int>(rand() % (upper - lower + 1));
}

void bufferClear(CRGB *buf) {
  memset(buf, 0x00, ((totalWidth) * (totalHeight)) * sizeof(CRGB));
}

void matrixFill(CRGB *leds) {
  uint16_t y = (totalHeight);

  do {
    --y;
    uint16_t x = (totalWidth);
    do {
      --x;
      uint16_t _pixel = y * (totalWidth) + x;
      matrix_display->drawPixelRGB888(x, y, leds[_pixel].r, leds[_pixel].g, leds[_pixel].b);
    } while (x);
  } while (y);
}
