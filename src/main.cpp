/**
 * @file main.cpp
 * @author Thomas Schöpf / Hanimat
 * @brief Firmware für die HANIMAT Verkaufsmaschine basierend auf der ESP32 Plattform.
 * @version 1.2.5-noec
 * @date 2025-12-01
 *
 * © Copyright Thomas Schöpf
 *
 * Der HANIMAT steht unter der Creative Commons Namensnennung-NichtKommerziell 4.0 International (CC BY-NC 4.0) Lizenz.
 * Urheber des Projekts ist Thomas Schöpf (Hanimat-Projekt).
 * Weitere Informationen finden Sie unter: www.hanimat.at
 *
 * Was bedeutet die CC BY-NC 4.0 Lizenz für dich und den HANIMAT?
 * Du darfst die HANIMAT-Software und -Pläne frei nutzen, kopieren, weiterentwickeln und anpassen.
 * Bei jeder Weitergabe oder Veröffentlichung deiner HANIMAT Anpassungen musst du Thomas Schöpf (Hanimat-Projekt)
 * als Urheber nennen und einen Link zur Lizenz (CC BY-NC 4.0) beifügen.
 *
 * Der Verkauf von Produkten, die du mit einem selbstgebauten HANIMAT Automaten anbietest (z.B. Honig, Eier etc.),
 * ist ausdrücklich erlaubt und erwünscht! Das ist der Sinn des HANIMAT Projekts.
 *
 * Du darfst die HANIMAT-Software, die Pläne oder deine direkten Anpassungen daran nicht verkaufen oder als kommerzielles Produkt anbieten.
 * Die Bereitstellung der HANIMAT Software und Pläne selbst muss kostenfrei und unter denselben Lizenzbedingungen erfolgen.
 *
 * Diese Firmware verwaltet alle Operationen der HANIMAT Verkaufsmaschine, einschließlich:
 * - TFT Display Benutzeroberfläche
 * - Keypad Eingabe für die Slot-Auswahl
 * - Münz- und Banknotenprüfer für die Zahlungsabwicklung
 * - I2C Relaissteuerung zur Produktausgabe
 * - WiFi Konnektivität und ein webbasiertes Administrationspanel
 * - OTA (Over-the-Air) Firmware-Updates
 * - Telegram Benachrichtigungen für Verkäufe und Lagerbestandsalarme
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // Required for secure HTTPS connections to Telegram
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <Update.h>
#include <UniversalTelegramBot.h> // Telegram Bot Library
#include <ArduinoJson.h>

// --- Custom Fonts ---
#include "fonts/Poppins_Black_14.h"
#include "fonts/Poppins_Regular_10.h"
#include "fonts/Poppins_Regular_7.h"


// =================================================================
//                      FIRMWARE VERSION
// =================================================================
const String FIRMWARE_VERSION = "V1.2.5-noec";

// =================================================================
//                      CONFIGURATION CONSTANTS
// =================================================================

// --- Vending Machine Configuration ---
const int DEFAULT_MAX_SLOTS = 16;
const int MAX_SLOTS = 16;

// --- Timing and Timeout Values (in milliseconds) ---
unsigned long COIN_PROCESSING_DELAY = 150;
unsigned long BILL_ISR_DEBOUNCE_MS = 75;
unsigned long BILL_GROUP_PROCESSING_TIMEOUT_MS = 1500;
unsigned long DISPENSE_RELAY_ON_TIME = 5000;
unsigned long KEYPAD_INPUT_TIMEOUT = 3000;
unsigned long WEB_TIMEOUT = 600000;
unsigned long SLOT_SELECTION_TIMEOUT = 10000;
unsigned long DISPLAY_TIMEOUT = 20000;
const unsigned long STARTUP_IGNORE_BILL_TIME = 5000; // Ignore bill pulses briefly on startup
const long TELEGRAM_CHECK_INTERVAL = 5000; // Not currently used, but can be for polling

// --- Hardware Pin Definitions ---
#define TFT_CS    26
#define TFT_DC    4
#define TFT_RST   16
#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_MISO  -1 // MISO not used

#define COIN_ACCEPTOR_PIN 5
#define BILL_ACCEPTOR_PIN 32
#define BILL_INHIBIT_PIN 33
#define WIFI_RESET_BUTTON 34
#define RELAY_I2C_ADDRESS 0x20
#define BUZZER_PIN 25
#define OFFLINE_MODE_PIN 27

// --- Payment Mapping ---
// Maps the number of pulses to a cent value for coins. Index is the pulse count.
const int pulseValues[] = {0, 0, 10, 20, 50, 100, 200}; // 0, 1, 2, 3, 4, 5, 6 pulses

// Maps the number of pulses to a Euro value for bills. Index is the pulse count.
const int billValues[]  = {
//Pulses: 0, 1, 2, 3, 4, 5, 6, 7, 8,  9, 10, 11, 12, 13, 14, 15, 16
          0, 0, 0, 0, 5, 0, 0, 0, 10, 0, 0,  0,  0,  0,  0,  0,  20
};

// --- Security ---
const String DEFAULT_PASSWORD = "admin"; // Default password for the web interface

// --- System State ---
enum class CurrentSystemState {
  IDLE,                  // Default state, waiting for user interaction
  USER_INTERACTION,      // User is interacting via keypad or payment
  ERROR_DISPLAY,         // An error message is being shown
  OTA_UPDATE             // OTA update is in progress
};
CurrentSystemState currentSystemState = CurrentSystemState::IDLE;

// =================================================================
//                      GLOBAL VARIABLES
// =================================================================

// --- Timing & State Tracking ---
unsigned long slotSelectedTime = 0;
unsigned long bootTime = 0;
unsigned long lastRelayChangeTime = 0;
unsigned long lastUserInteractionTime = 0;

// --- Web Server & Storage ---
WebServer server(80);
Preferences preferences;

// --- Relay Control ---
#define RELAYS_PER_EXPANDER 16
#define NUM_EXPANDERS 1
static uint16_t expanderOutputStates[NUM_EXPANDERS]; // Bitmask for relay states

// --- Keypad Configuration ---
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 3;
char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[KEYPAD_ROWS] = {15, 14, 12, 17}; // ESP32 GPIO pins for keypad rows
byte colPins[KEYPAD_COLS] = {2, 19, 13};  // ESP32 GPIO pins for keypad columns

// --- Keypad State ---
char lastPhysicallyPressedKey = 0;
char lastReturnedKey = 0;
unsigned long lastKeyPressTime = 0;
const unsigned long KEYPAD_DEBOUNCE_PERIOD = 50; // Debounce time for keypad

// --- Display ---
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// --- Slot Data ---
float slotPrices[MAX_SLOTS];
bool slotAvailable[MAX_SLOTS];
bool slotLocked[MAX_SLOTS];
int activeSlots = DEFAULT_MAX_SLOTS;

// --- Telegram Notification Configuration ---
int almostEmptyThreshold = 5;
bool almostEmptyNotificationSent = false;
bool emptyNotificationSent = false;

bool telegramEnabled = false;
bool telegramNotifyOnSale = false;
bool telegramNotifyAlmostEmpty = true;
bool telegramNotifyEmpty = true;

// --- Payment & Credit ---
float credit = 0.0;
volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;

volatile unsigned long billAcceptorPulseCount = 0;
volatile unsigned long lastBillPulseEdgeTime = 0;
volatile unsigned long lastBillDebounceEdgeTime = 0;


// --- Display Customization ---
const int SLOGAN_MAX_LENGTH = 24; // Zeichenlimit für den Slogan
String displaySlogan = "";
String displayFooter = "www.hanimat.at";

// --- Logging ---
#define MAX_LOG_LINES 50
String logBuffer[MAX_LOG_LINES];
int logIndex = 0;

// --- User Input State ---
String keypadInputBuffer = "";
unsigned long lastKeypadInputTime = 0;
int selectedSlot = -1;

// --- Authentication ---
String savedPassword = DEFAULT_PASSWORD;
bool isAuthenticated = false;
unsigned long lastActivityTimeWeb = 0;

bool displayNeedsUpdate = true;

// --- Dispense Job ---
struct DispenseJob {
  bool active;
  int slot;
  unsigned long startTime;
  bool relayActivated;
};
DispenseJob dispenseJob = { false, -1, 0, false };

// --- OTA Update ---
String otaStatusMessage = "";
bool otaUpdateInProgress = false;

// --- Telegram Bot ---
WiFiClientSecure secured_client;
String telegramBotToken = ""; // Placeholder for Telegram Bot Token
String telegramChatId = "";   // Placeholder for Telegram Chat ID
UniversalTelegramBot bot(telegramBotToken, secured_client);


// =================================================================
//                      FUNCTION PROTOTYPES
// =================================================================
void setupWebServer();
void updateDisplayScreen();
char manualGetKeyState();
void processKeypad();
void processKeypadSelection();
void scheduleDispense(int slot);
void processDispenseJob();
bool controlSlotRelay(int slot, bool activate);
void processBillAcceptorPulses();
void resetDisplayToDefault();
void processAcceptedCoin();
void handleLogDataRequest();
void displayOTAMessageTFT(String line1, String line2 = "", String line3 = "", uint16_t color = ILI9341_ORANGE);
void checkOverallStockLevel();

// Web Server Handlers
void handleRoot();
void handleLogin();
void handleChangePasswordWeb();
void handleUpdatePriceWeb();
void handleRefillWeb();
void handleAddCreditWeb();
void handleResetCreditWeb();
void handleRefillAllWeb();
void handleTriggerRelayWeb();
void handleTriggerAllRelaysWeb();
void handleSetStaticIPWeb();
void handleUpdateSlotsWeb();
void handleToggleSlotLockWeb();
void handleOTAUpdatePage();
void handleOTAUpdateUpload();
void handleOTAFileUpload();
void handleTimingConfigPage();
void handleSaveTimingConfig();
void handleTelegramConfigPage();
void handleSaveTelegramConfig();
void handleSendTestTelegram();
void handleDisplayConfigPage();
void handleSaveDisplayConfig();

// HTML Page Generators
void showLoginPage();
void showDashboard();

// Utility Functions
int countAvailableSlots();
int countEmptySlots();
void displayErrorMessage(const String &line1, const String &line2 = "");
void playThankYouMelody();
void playErrorSound();
void playKeyPressBeep();
void logMessage(const String& msg);
bool checkRelayBoardOnline();
void sendTelegramMessage(String message);

// Interrupt Service Routines
void IRAM_ATTR coinAcceptorISR();
void IRAM_ATTR billAcceptorISR();

// =================================================================
//                      HELPER FUNCTIONS
// =================================================================

/**
 * @brief Logs a message to the Serial port and a circular buffer for the web UI.
 * @param msg The message string to log.
 */
void logMessage(const String& msg) {
  Serial.println(msg);
  logBuffer[logIndex] = "[" + String(millis() / 1000) + "s] " + msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

/**
 * @brief Plays a "Thank You" melody on the buzzer.
 */
void playThankYouMelody() {
  int melody[] = { 2093, 2349, 2637, 2349, 2093, 1975, 2093 }; // Notes (C7, D7, E7, ...)
  int noteDurations[] = { 150, 150, 300, 150, 150, 300, 400 };
  for (int i = 0; i < sizeof(melody)/sizeof(melody[0]); i++) {
    ledcWriteTone(0, melody[i]);
    delay(noteDurations[i]);
    ledcWriteTone(0, 0); // Stop tone
    delay(50);
  }
}

/**
 * @brief Plays a descending two-tone error sound on the buzzer.
 */
void playErrorSound() {
  ledcWriteTone(0, 2500);
  delay(150);
  ledcWriteTone(0, 2000);
  delay(250);
  ledcWriteTone(0, 0); // Stop tone
}

/**
 * @brief Plays a short beep sound for keypad presses.
 */
void playKeyPressBeep() {
  ledcWriteTone(0, 2800);
  delay(50);
  ledcWriteTone(0, 0); // Stop tone
}

/**
 * @brief Checks if the I2C relay expander board is connected and responsive.
 * @return True if the board acknowledges its address, false otherwise.
 */
bool checkRelayBoardOnline() {
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  byte error = Wire.endTransmission();
  if (error != 0) {
    logMessage("ERROR: Relay board I2C not reachable (Addr: 0x" + String(RELAY_I2C_ADDRESS, HEX) + ", Code: " + String(error) + ")");
  }
  return (error == 0);
}

/**
 * @brief Sends a message via Telegram if enabled and configured.
 * @param message The message string to send.
 */
void sendTelegramMessage(String message) {
  if (!telegramEnabled) {
    logMessage("Telegram: Notifications are disabled.");
    return;
  }
  bool offlineMode = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  if (offlineMode || WiFi.status() != WL_CONNECTED) {
    logMessage("Telegram: Offline, message not sent.");
    return;
  }
  if (telegramBotToken.length() > 0 && telegramChatId.length() > 0) {
    logMessage("Sending Telegram message: " + message);
    if(bot.sendMessage(telegramChatId, message, "")) { // Empty parse mode for emojis
      logMessage("Telegram message sent successfully.");
    } else {
      logMessage("ERROR: Failed to send Telegram message.");
    }
  } else {
    logMessage("WARNING: Telegram Bot Token or Chat ID not configured. Cannot send message.");
  }
}

/**
 * @brief Displays a multi-line message on the TFT, typically for OTA updates.
 * @param line1 First line of the message.
 * @param line2 Second line (optional).
 * @param line3 Third line (optional).
 * @param color Color for the first line.
 */
void displayOTAMessageTFT(String line1, String line2, String line3, uint16_t color) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(true);
  int16_t x1, y1;
  uint16_t w, h;

  // Title
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  tft.getTextBounds("HANIMAT", 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 40);
  tft.println("HANIMAT");

  // Message Lines
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(color);
  tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 90);
  tft.println(line1);

  if (line2.length() > 0) {
    tft.setTextColor(ILI9341_WHITE);
    tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 120);
    tft.println(line2);
  }
  
  if (line3.length() > 0) {
    tft.setTextColor(ILI9341_WHITE);
    tft.getTextBounds(line3, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 150);
    tft.println(line3);
  }
}

// =================================================================
//                      INTERRUPT SERVICE ROUTINES
// =================================================================

/**
 * @brief ISR for the coin acceptor. Increments a pulse counter.
 */
void IRAM_ATTR coinAcceptorISR() {
  coinPulseCount++;
  lastCoinPulseTime = millis();
}

/**
 * @brief ISR for the bill acceptor. Increments a pulse counter with debouncing.
 */
void IRAM_ATTR billAcceptorISR() {
  unsigned long currentMillis = millis();
  if (currentMillis < STARTUP_IGNORE_BILL_TIME) return; // Ignore pulses at startup

  if (currentMillis - lastBillDebounceEdgeTime > BILL_ISR_DEBOUNCE_MS) {
    billAcceptorPulseCount++;
    lastBillPulseEdgeTime = currentMillis;
    lastBillDebounceEdgeTime = currentMillis;
  }
}

// =================================================================
//                            SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  logMessage("System starting: HANIMAT " + FIRMWARE_VERSION);
  bootTime = millis();

  // --- Initialize I2C ---
  Wire.begin();
  Wire.setClock(50000L); // Set I2C clock to 50kHz for stability
  logMessage("I2C clock set to 50kHz.");
  delay(100); // Allow I2C bus to stabilize

  // --- Initialize Relay Expander Board (EARLY to prevent race condition) ---
  // This is done immediately after I2C initialization to ensure relay
  // pins are in a defined state (OFF) as quickly as possible.
  expanderOutputStates[0] = 0x0000;
  logMessage("Configuring Relay Board Bank 0 (Slots 0-7) as outputs...");
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x06); // IODIRB Register
  Wire.write(0x00); // Set all pins of Port B to output
  Wire.endTransmission();
   
  logMessage("Configuring Relay Board Bank 1 (Slots 8-15) as outputs...");
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x07); // IODIRA Register
  Wire.write(0x00); // Set all pins of Port A to output
  Wire.endTransmission();

  logMessage("Setting all relays to OFF state...");
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x02); // GPIOB Register
  Wire.write(0x00); // Set all pins low
  Wire.endTransmission();
   
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x03); // GPIOA Register
  Wire.write(0x00); // Set all pins low
  Wire.endTransmission();
  logMessage("Relay board initialized.");

  // --- Initialize Telegram Client ---
  secured_client.setInsecure(); // Allow connections without certificate validation
  logMessage("Telegram client set to 'insecure' mode.");

  // --- Initialize Buzzer ---
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  ledcSetup(0, 2000, 8); // Setup LEDC channel 0
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWriteTone(0, 0); // Ensure buzzer is off

  // --- Initialize GPIO Pins ---
  pinMode(WIFI_RESET_BUTTON, INPUT_PULLUP);
  pinMode(OFFLINE_MODE_PIN, INPUT_PULLUP);
  pinMode(BILL_INHIBIT_PIN, OUTPUT);
  digitalWrite(BILL_INHIBIT_PIN, HIGH); // Inhibit bill acceptor by default


  // --- Initialize Keypad Pins (Manual Scan Mode) ---
  for (int i = 0; i < KEYPAD_ROWS; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], LOW);
  }
  for (int i = 0; i < KEYPAD_COLS; i++) {
    pinMode(colPins[i], INPUT); // Assumes external pull-down resistors
  }
  logMessage("Keypad pins configured for manual scan with external pull-downs.");

  // --- Load Settings from Preferences ---
  preferences.begin("hanimat", false);
  logMessage("Loading settings from Preferences...");
  COIN_PROCESSING_DELAY = preferences.getULong("coinDelay", 150);
  BILL_ISR_DEBOUNCE_MS = preferences.getULong("billIsrDeb", 75);
  BILL_GROUP_PROCESSING_TIMEOUT_MS = preferences.getULong("billGrpTout", 1500);
  DISPENSE_RELAY_ON_TIME = preferences.getULong("dispTime", 5000);
  KEYPAD_INPUT_TIMEOUT = preferences.getULong("keypadTime", 3000);
  SLOT_SELECTION_TIMEOUT = preferences.getULong("slotSelTime", 10000);
  DISPLAY_TIMEOUT = preferences.getULong("dispTimeout", 20000);
  
  telegramEnabled = preferences.getBool("tgEnabled", false);
  telegramBotToken = preferences.getString("tgToken", "");
  telegramChatId = preferences.getString("tgChatId", "");
  telegramNotifyOnSale = preferences.getBool("tgNotifySale", false);
  telegramNotifyAlmostEmpty = preferences.getBool("tgNotifyAlmost", true);
  telegramNotifyEmpty = preferences.getBool("tgNotifyEmpty", true);
  almostEmptyThreshold = preferences.getInt("tgAlmostThres", 5);
  bot.updateToken(telegramBotToken);

  // Display-Texte laden
  displaySlogan = preferences.getString("dispSlogan", "");
  displayFooter = preferences.getString("dispFooter", "www.hanimat.at");

  activeSlots = preferences.getInt("activeSlots", DEFAULT_MAX_SLOTS);
  if (activeSlots <= 0 || activeSlots > MAX_SLOTS) activeSlots = DEFAULT_MAX_SLOTS;

  for (int i = 0; i < MAX_SLOTS; i++) {
    slotPrices[i]    = preferences.getFloat(("price" + String(i)).c_str(), 5.0f + (i * 0.1f));
    slotAvailable[i] = preferences.getBool(("avail" + String(i)).c_str(), true);
    slotLocked[i]    = preferences.getBool(("locked" + String(i)).c_str(), false);
  }
  credit = preferences.getFloat("credit", 0.0f);
  savedPassword = preferences.getString("password", DEFAULT_PASSWORD);
  preferences.end();
  logMessage("Settings loaded.");

  // --- Initialize TFT Display ---
  tft.begin();
  tft.setRotation(1); // Landscape mode
  tft.fillScreen(ILI9341_BLACK);
   
  // Use custom font for startup screen
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds("HANIMAT", 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) - h);
  tft.println("HANIMAT");

  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(ILI9341_WHITE);
  String subtitle = "startet...";
  tft.getTextBounds(subtitle, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) + 25);
  tft.println(subtitle);
  delay(2500);

  // --- Initialize WiFi ---
  bool offlineMode = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  if (offlineMode) {
    logMessage("Operating Mode: OFFLINE (GPIO " + String(OFFLINE_MODE_PIN) + " is LOW)");
    WiFi.softAP("HANIMAT-Offline", "Honig1234");
    logMessage("Offline AP started. SSID: HANIMAT-Offline, IP: " + WiFi.softAPIP().toString());
    tft.fillScreen(ILI9341_BLACK);
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(ILI9341_ORANGE);
    tft.setCursor(10,40); tft.println("OFFLINE MODUS");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(10,70); tft.println("AP: HANIMAT-Offline");
    tft.setCursor(10,100); tft.println("IP: " + WiFi.softAPIP().toString());
    tft.setCursor(10,130); tft.println("PW: Honig1234");
    delay(5000);

  } else {
    logMessage("Operating Mode: ONLINE (GPIO " + String(OFFLINE_MODE_PIN) + " is HIGH)");
    preferences.begin("hanimat", false);
    if (preferences.isKey("static_ip")) {
        IPAddress staticIP, gateway, subnet, dns1, dns2;
        staticIP.fromString(preferences.getString("static_ip", ""));
        gateway.fromString(preferences.getString("gateway", ""));
        subnet.fromString(preferences.getString("subnet", ""));
        dns1.fromString(preferences.getString("dns1", "8.8.8.8"));
        dns2.fromString(preferences.getString("dns2", "8.8.4.4"));
        if(staticIP[0] != 0) {
            logMessage("Attempting to connect with static IP: " + staticIP.toString());
            wm.setSTAStaticIPConfig(staticIP, gateway, subnet, dns1);
        }
    }
    preferences.end();

    if (!wm.autoConnect("HANIMAT-Setup", "Honig1234")) {
      logMessage("WiFi connection failed. Starting Config Portal: HANIMAT-Setup");
      tft.fillScreen(ILI9341_BLACK);
      tft.setFont(&Poppins_Black14pt7b);
      tft.setTextColor(ILI9341_RED);
      String errorMsg = "WLAN Fehler!";
      tft.getTextBounds(errorMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 40);
      tft.println(errorMsg);

      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(ILI9341_WHITE);
       
      String line1 = "Verbinde mit WLAN:";
      tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 70);
      tft.println(line1);
       
      String line2 = "SSID: HANIMAT-Setup";
      tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 90);
      tft.println(line2);

      String line3 = "PW: Honig1234";
      tft.getTextBounds(line3, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 110);
      tft.println(line3);

      String line4 = "Dann 192.168.4.1";
      tft.getTextBounds(line4, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 130);
      tft.println(line4);
    } else {
      logMessage("WiFi connected! IP: " + WiFi.localIP().toString());
      tft.fillScreen(ILI9341_BLACK);

      tft.setFont(&Poppins_Black14pt7b);
      tft.setTextColor(ILI9341_GREEN);
      String connectedMsg = "WLAN Verbunden!";
      tft.getTextBounds(connectedMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 80);
      tft.println(connectedMsg);

      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(ILI9341_WHITE);
      String ipMsg = "IP: " + WiFi.localIP().toString();
      tft.getTextBounds(ipMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 110);
      tft.println(ipMsg);

      String versionMsg = "Version: " + FIRMWARE_VERSION;
      tft.getTextBounds(versionMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 130);
      tft.println(versionMsg);

      delay(3000);
    }
  }

  // --- Initialize Web Server ---
  setupWebServer();

  // --- Initialize Payment Acceptors ---
  pinMode(COIN_ACCEPTOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(COIN_ACCEPTOR_PIN), coinAcceptorISR, RISING);
  
  pinMode(BILL_ACCEPTOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BILL_ACCEPTOR_PIN), billAcceptorISR, RISING);

  // --- Finalize Setup ---
  logMessage("Setup complete. System is ready.");
  digitalWrite(BILL_INHIBIT_PIN, LOW); // Enable bill acceptor
  displayNeedsUpdate = true;
  lastUserInteractionTime = millis();
  currentSystemState = CurrentSystemState::IDLE;
}

// =================================================================
//                            MAIN LOOP
// =================================================================
void loop() {
  // Always handle web server clients
  server.handleClient();

  // Check for factory reset button press (hold for 7 seconds)
  if (digitalRead(WIFI_RESET_BUTTON) == LOW) {
    unsigned long pressStart = millis();
    while (digitalRead(WIFI_RESET_BUTTON) == LOW) {
      if (millis() - pressStart >= 7000) break;
      delay(10);
    }
    if (millis() - pressStart >= 7000) {
      logMessage("FACTORY RESET initiated...");
      tft.fillScreen(ILI9341_BLACK);
      tft.setTextColor(ILI9341_RED); tft.setTextSize(3);
      tft.setCursor(10, 80); tft.println("WERKSRESET");
      tft.setTextSize(2); tft.setCursor(10, 130); tft.println("Daten werden geloescht...");
      delay(3000);

      // Clear all saved settings
      preferences.begin("hanimat", true); preferences.clear(); preferences.end();
      WiFiManager wm; wm.resetSettings();
      
      logMessage("Factory reset complete. Restarting...");
      ESP.restart();
    }
  }

  // --- Main state machine ---
  if (currentSystemState != CurrentSystemState::OTA_UPDATE) {
    // Timeout for user inactivity, resetting the screen to default
    if (millis() - lastUserInteractionTime > DISPLAY_TIMEOUT) {
      if (currentSystemState != CurrentSystemState::IDLE) {
        logMessage("Display timeout. Reverting to idle screen.");
        resetDisplayToDefault();
      }
    }
    
    // Timeout for slot selection
    if (selectedSlot != -1 && (millis() - slotSelectedTime > SLOT_SELECTION_TIMEOUT)) {
        logMessage("Slot selection timed out. Resetting selection.");
        resetDisplayToDefault();
    }

    // Process all inputs and jobs
    processKeypad();
    processAcceptedCoin();
    processBillAcceptorPulses();
    processDispenseJob();
  }

  // Auto-logout from web interface after timeout
  if (isAuthenticated && (millis() - lastActivityTimeWeb > WEB_TIMEOUT)) {
    isAuthenticated = false;
    logMessage("Web interface auto-logout due to inactivity.");
  }

  // Update display only when needed
  if (displayNeedsUpdate && currentSystemState != CurrentSystemState::OTA_UPDATE) {
    updateDisplayScreen();
    displayNeedsUpdate = false;
  }

  // Periodically check WiFi connection and attempt to reconnect if lost
  static unsigned long lastWiFiCheckTime = 0;
  bool offlineMode = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  if (!offlineMode && (millis() - lastWiFiCheckTime > 30000)) {
      lastWiFiCheckTime = millis();
      if (WiFi.status() != WL_CONNECTED) {
          logMessage("WiFi connection lost. Attempting to reconnect...");
          WiFi.reconnect();
      }
  }
  delay(10); // Small delay to prevent watchdog timer issues
}

// =================================================================
//                      CORE LOGIC IMPLEMENTATION
// =================================================================

char manualGetKeyState() {
  char currentPhysicalKey = 0;

  // Iterate through rows
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    digitalWrite(rowPins[r], HIGH); // Activate one row
    // Check all columns in that row
    for (int c = 0; c < KEYPAD_COLS; c++) {
      if (digitalRead(colPins[c]) == HIGH) {
        currentPhysicalKey = keys[r][c];
        break;
      }
    }
    digitalWrite(rowPins[r], LOW); // Deactivate the row
    if (currentPhysicalKey != 0) {
      break;
    }
  }

  unsigned long now = millis();

  // Debounce logic
  if (currentPhysicalKey != lastPhysicallyPressedKey) {
    lastKeyPressTime = now;
    lastPhysicallyPressedKey = currentPhysicalKey;
    if (currentPhysicalKey == 0) {
        lastReturnedKey = 0; // Reset returned key when released
    }
    return 0; // Return nothing on initial press/release
  }

  // If key is held down longer than the debounce period, return it once
  if (currentPhysicalKey != 0 && (now - lastKeyPressTime > KEYPAD_DEBOUNCE_PERIOD)) {
    if (currentPhysicalKey != lastReturnedKey) {
      lastReturnedKey = currentPhysicalKey;
      return currentPhysicalKey;
    }
  }
  
  return 0; // No valid key press
}

/**
 * @brief Sets up all web server endpoints (routes).
 */
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/changepassword", HTTP_POST, handleChangePasswordWeb);
  server.on("/updateprice", HTTP_POST, handleUpdatePriceWeb);
  server.on("/refill", HTTP_POST, handleRefillWeb);
  server.on("/addcredit", HTTP_POST, handleAddCreditWeb);
  server.on("/resetcredit", HTTP_POST, handleResetCreditWeb);
  server.on("/refillall", HTTP_POST, handleRefillAllWeb);
  server.on("/triggerrelay", HTTP_POST, handleTriggerRelayWeb);
  server.on("/triggerallrelays", HTTP_POST, handleTriggerAllRelaysWeb);
  server.on("/setstaticip", HTTP_POST, handleSetStaticIPWeb);
  server.on("/updateslots", HTTP_POST, handleUpdateSlotsWeb);
  server.on("/toggleslotlock", HTTP_POST, handleToggleSlotLockWeb);
  server.on("/logdata", HTTP_GET, handleLogDataRequest);
  server.on("/otaupdate", HTTP_GET, handleOTAUpdatePage);
  server.on("/timingconfig", HTTP_GET, handleTimingConfigPage);
  server.on("/savetimingconfig", HTTP_POST, handleSaveTimingConfig);
  server.on("/telegramconfig", HTTP_GET, handleTelegramConfigPage);
  server.on("/savetelegramconfig", HTTP_POST, handleSaveTelegramConfig);
  server.on("/sendtesttelegram", HTTP_POST, handleSendTestTelegram);
  server.on("/displayconfig", HTTP_GET, handleDisplayConfigPage);
  server.on("/savedisplayconfig", HTTP_POST, handleSaveDisplayConfig);
   
  // OTA Upload Handler
  server.on("/ota-upload", HTTP_POST, []() {
    otaStatusMessage = "Upload successful. Starting update...";
    server.sendHeader("Location", "/otaupdate", true);
    server.send(302, "text/plain", "");
  }, handleOTAFileUpload);

  // 404 Not Found Handler
  server.onNotFound([]() {
    server.send(404, "text/plain", "Page not found.");
    logMessage("HTTP 404: " + server.uri());
  });

  server.begin();
  logMessage("Web server started.");
}

/**
 * @brief Resets the system state and display to the default idle screen.
 */
void resetDisplayToDefault() {
  selectedSlot = -1;
  keypadInputBuffer = "";
  currentSystemState = CurrentSystemState::IDLE; 
  displayNeedsUpdate = true;
  lastUserInteractionTime = millis();
}

/**
 * @brief Updates the TFT display based on the current system state.
 */
void updateDisplayScreen() {
  tft.fillScreen(ILI9341_BLACK);
  char buffer[50]; // Buffer for formatting strings
  int16_t x1, y1; uint16_t w, h;

  // --- Static Header ---
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  tft.getTextBounds("HONIGAUTOMAT", 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 40);
  tft.println("HONIGAUTOMAT");

  // --- Credit Display ---
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(ILI9341_GREEN);
  sprintf(buffer, "Guthaben: %.2f EUR", credit);
  tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(10, 80);
  tft.println(buffer);

  // --- WiFi Status Indicator ---
  bool offlineModeActive = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  if (!offlineModeActive) {
    int wifiX = tft.width() - 20;
    int wifiY = 20;
    int wifiRadius = 6;
    tft.fillCircle(wifiX, wifiY, wifiRadius, (WiFi.status() == WL_CONNECTED) ? ILI9341_GREEN : ILI9341_RED);
  }

  // --- Dynamic Content Area ---
  int y_dynamic_start = 110;
  int line_spacing = 25;
  tft.setFont(&Poppins_Regular10pt7b);

  switch(currentSystemState) {
    case CurrentSystemState::ERROR_DISPLAY:
      // This case is handled by displayErrorMessage(), no action needed here.
      break;

    default: // Covers IDLE and USER_INTERACTION
      if (dispenseJob.active) {
        tft.setTextColor(ILI9341_CYAN);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Fach "); tft.print(dispenseJob.slot + 1);
        tft.setCursor(10, y_dynamic_start + line_spacing);
        tft.print("wird geoeffnet...");
      } else if (selectedSlot != -1) {
        tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Fach: "); tft.println(selectedSlot + 1);
        tft.setCursor(10, y_dynamic_start + line_spacing);
        if (slotLocked[selectedSlot]) {
          tft.setTextColor(ILI9341_RED); tft.println("Gesperrt");
        } else if (!slotAvailable[selectedSlot]) {
          tft.setTextColor(ILI9341_RED); tft.println("Leer");
        } else {
          tft.setTextColor(ILI9341_WHITE);
          sprintf(buffer, "Preis: %.2f EUR", slotPrices[selectedSlot]);
          tft.println(buffer);
          tft.setCursor(10, y_dynamic_start + (line_spacing * 2));
          if (credit >= slotPrices[selectedSlot]) {
            tft.setTextColor(ILI9341_GREEN); tft.println("# Kaufen");
          } else {
            tft.setTextColor(ILI9341_RED); tft.println("Guthaben?");
          }
        }
      } else if (keypadInputBuffer.length() > 0) {
        tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Eingabe: "); tft.println(keypadInputBuffer);
      } else { // Idle screen
        tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(10, y_dynamic_start);
        tft.println("Waehle Fach (1-" + String(activeSlots) + ")");
        tft.setCursor(10, y_dynamic_start + line_spacing);
        tft.println("oder Geld einwerfen.");
      }
      break;
  }
  

// --- Slogan zeichnen (größere Schrift) ---
if (displaySlogan.length() > 0) {
  tft.setFont(&Poppins_Regular10pt7b); // Größere Schrift für den Slogan
  tft.setTextColor(ILI9341_WHITE);
  tft.getTextBounds(displaySlogan, 0, 0, &x1, &y1, &w, &h);
  // Positionierung angepasst für größere Schrift
  tft.setCursor((tft.width() - w) / 2, tft.height() - h - 25); 
  tft.println(displaySlogan);
}

// --- Footer-Text zeichnen (kleinere Schrift) ---
tft.setFont(&Poppins_Regular7pt7b); // Kleinere Schrift für den Footer
tft.setTextColor(ILI9341_YELLOW);
tft.getTextBounds(displayFooter, 0, 0, &x1, &y1, &w, &h);
tft.setCursor((tft.width() - w) / 2, tft.height() - h - 5);
tft.print(displayFooter);
}

/**
 * @brief Processes keypad input, updates buffer, and handles '#' and '*' keys.
 */
void processKeypad() {
  char key = manualGetKeyState();
  if (key == 0) return; // No new key press

  playKeyPressBeep();
  logMessage(String("Keypad: Processed Key: '") + key + "'");
  lastUserInteractionTime = millis();
  currentSystemState = CurrentSystemState::USER_INTERACTION;

  if (isdigit(key)) {
    lastKeypadInputTime = millis();

    // Logic to handle 1 or 2-digit slot numbers
    if (keypadInputBuffer.length() >= 2) {
      keypadInputBuffer = ""; // Reset buffer if it's already full
    }
    keypadInputBuffer += key;
    logMessage("Keypad: Buffer updated to: " + keypadInputBuffer);
    processKeypadSelection();

  } else if (key == '#') { // Confirm selection or purchase
    if (keypadInputBuffer.length() > 0) {
      logMessage("Keypad: '#' pressed. Finalizing selection from buffer: " + keypadInputBuffer);
      processKeypadSelection();
    }
     
    if (selectedSlot != -1) {
      if (slotLocked[selectedSlot]) {
        displayErrorMessage("Fach " + String(selectedSlot + 1), "gesperrt!");
      } else if (!slotAvailable[selectedSlot]) {
        displayErrorMessage("Fach " + String(selectedSlot + 1), "ist leer!");
      } else if (credit >= slotPrices[selectedSlot]) {
        logMessage("Purchase attempt: Slot " + String(selectedSlot + 1) + ", Credit: " + String(credit,2) + " EUR, Price: " + String(slotPrices[selectedSlot],2) + " EUR.");
        scheduleDispense(selectedSlot);
      } else {
        displayErrorMessage("Guthaben", "zu gering!");
      }
    } else {
      displayErrorMessage("Kein Fach", "gewaehlt!");
    }
    keypadInputBuffer = ""; // Clear buffer after '#'
   
} else if (key == '*') { // Cancel/reset
    logMessage("Keypad: '*' pressed. Resetting selection.");
    resetDisplayToDefault();
}
  displayNeedsUpdate = true;
}

/**
 * @brief Processes the current keypad input buffer to select a slot.
 */
void processKeypadSelection() {
  if (keypadInputBuffer.isEmpty()) return;

  int slotNum = keypadInputBuffer.toInt();
  logMessage("processKeypadSelection: Buffer '" + keypadInputBuffer + "', toInt: " + String(slotNum));

  if (slotNum >= 1 && slotNum <= activeSlots) {
    selectedSlot = slotNum - 1;
    logMessage("Keypad: Slot " + String(selectedSlot + 1) + " selected from buffer.");
    slotSelectedTime = millis();
    currentSystemState = CurrentSystemState::USER_INTERACTION;

    // Check if the selection can be considered final (e.g., for single-digit slots or after 2 digits)
    bool isFinal = (keypadInputBuffer.length() == 2) || (activeSlots < 10);
    if (keypadInputBuffer.length() == 1 && activeSlots >= 10) {
        // If first digit is too high for a valid 2-digit number, it's final
        if (keypadInputBuffer.toInt() > activeSlots / 10) {
            isFinal = true;
        }
    }
     
    if (isFinal) {
      logMessage("Keypad: Selection '" + keypadInputBuffer + "' is final. Clearing buffer.");
      keypadInputBuffer = "";
    } else {
      logMessage("Keypad: Waiting for second digit or '#' to confirm.");
    }

  } else {
    // If the input is 2 digits long and still invalid, show an error.
    if (keypadInputBuffer.length() == 2) {
      displayErrorMessage("Fach " + keypadInputBuffer, "ungueltig!");
      selectedSlot = -1;
      keypadInputBuffer = "";
    }
  }
  displayNeedsUpdate = true;
}

/**
 * @brief Activates or deactivates a relay for a specific slot via I2C.
 * @param slot The slot index (0-15).
 * @param activate True to activate the relay, false to deactivate.
 * @return True on success, false on I2C communication failure.
 */
bool controlSlotRelay(int slot, bool activate) {
  if (slot < 0 || slot >= MAX_SLOTS) {
    logMessage("ERROR: Invalid slot index for relay: " + String(slot));
    return false;
  }

  // Update the bitmask for the relay states
  if (activate) {
    expanderOutputStates[0] |= (1 << slot);
  } else {
    expanderOutputStates[0] &= ~(1 << slot);
  }
  
  // Determine which register (Port A or B) and data byte to send
  uint8_t relayCommand = (slot < 8) ? 0x02 : 0x03; // GPIOB or GPIOA
  uint8_t dataByte = (slot < 8) ? (uint8_t)(expanderOutputStates[0] & 0xFF) : (uint8_t)(expanderOutputStates[0] >> 8);
  
  // Send the command over I2C
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(relayCommand);
  Wire.write(dataByte);
  byte error = Wire.endTransmission();

  if (error == 0) {
    logMessage("Relay for slot " + String(slot + 1) + (activate ? " ON" : " OFF") + " command sent successfully.");
    lastRelayChangeTime = millis();
    return true;
  } else {
    logMessage("ERROR: I2C failed for slot " + String(slot + 1) + ". Code: " + String(error));
    return false;
  }
}

/**
 * @brief Initializes a dispense job for a given slot.
 * @param slotToDispense The slot index to be dispensed.
 */
void scheduleDispense(int slotToDispense) {
  logMessage("scheduleDispense: Called for slot " + String(slotToDispense + 1));
  if (dispenseJob.active) {
    logMessage("scheduleDispense: WARNING: Dispense job already active. New request ignored.");
    return;
  }
  if (!checkRelayBoardOnline()) {
    displayErrorMessage("Relais Fehler", "Board offline");
    return;
  }

  // Set up the dispense job
  dispenseJob.active = true;
  dispenseJob.slot = slotToDispense;
  dispenseJob.startTime = millis();
  dispenseJob.relayActivated = false;
  logMessage("Dispense job scheduled for slot " + String(slotToDispense + 1));
  currentSystemState = CurrentSystemState::USER_INTERACTION;
  
  // Display message to user
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(10, 100);
  tft.print("Fach "); tft.print(dispenseJob.slot + 1);
  tft.setCursor(10, 130);
  tft.print("wird vorbereitet...");
  displayNeedsUpdate = true;
}

/**
 * @brief Manages the active dispense job, from activating the relay to deactivating it after a timeout.
 */
void processDispenseJob() {
  if (!dispenseJob.active) return;

  unsigned long currentTime = millis();
  currentSystemState = CurrentSystemState::USER_INTERACTION;

  // --- Step 1: Activate Relay and Process Payment ---
  if (!dispenseJob.relayActivated) {
    digitalWrite(BILL_INHIBIT_PIN, HIGH); // Inhibit bill acceptor during dispense

    if (!controlSlotRelay(dispenseJob.slot, true)) {
      logMessage("processDispenseJob: ERROR activating relay for slot " + String(dispenseJob.slot + 1));
      displayErrorMessage("Relais Fehler", "Kauf abgebrochen");
      dispenseJob.active = false;
      digitalWrite(BILL_INHIBIT_PIN, LOW);
      resetDisplayToDefault();
      return;
    }
     
    // Deduct credit and update slot availability
    credit -= slotPrices[dispenseJob.slot];
    if (credit < 0) credit = 0;
    slotAvailable[dispenseJob.slot] = false;
    logMessage("Purchase complete for slot " + String(dispenseJob.slot + 1) + ". New credit: " + String(credit, 2));

    // Persist changes to Preferences
    preferences.begin("hanimat", false);
    preferences.putFloat("credit", credit);
    preferences.putBool(("avail" + String(dispenseJob.slot)).c_str(), slotAvailable[dispenseJob.slot]);
    preferences.end();
    
    // Send notifications
    if (telegramNotifyOnSale) {
        String saleMessage = "🍯 VERKAUF: Fach #" + String(dispenseJob.slot + 1) + " wurde verkauft und ist jetzt leer.";
        sendTelegramMessage(saleMessage);
    }
    checkOverallStockLevel();

    // Play sound and update display
    playThankYouMelody();
    tft.fillScreen(ILI9341_BLACK);
    tft.setFont(&Poppins_Black14pt7b);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(10, 100);
    tft.println("Danke!");
     
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setCursor(10, 140);
    tft.print("Fach "); tft.print(dispenseJob.slot + 1); tft.print(" offen.");

    // Mark step 1 as complete
    dispenseJob.relayActivated = true;
    dispenseJob.startTime = currentTime; // Reset timer for dispense duration
    displayNeedsUpdate = true;
  }

  // --- Step 2: Deactivate Relay after Timeout ---
  if (dispenseJob.relayActivated && (currentTime - dispenseJob.startTime >= DISPENSE_RELAY_ON_TIME)) {
    logMessage("Dispense time elapsed. Deactivating relay for slot " + String(dispenseJob.slot + 1));
    controlSlotRelay(dispenseJob.slot, false);

    // Finalize job
    dispenseJob.active = false;
    digitalWrite(BILL_INHIBIT_PIN, LOW); // Re-enable bill acceptor
    resetDisplayToDefault();
  }
}

/**
 * @brief Processes coin pulses after a delay to group them into a single coin event.
 */
void processAcceptedCoin() {
  if (coinPulseCount > 0 && (millis() - lastCoinPulseTime > COIN_PROCESSING_DELAY)) {
    int pulsesToProcess;
    noInterrupts();
    pulsesToProcess = coinPulseCount;
    coinPulseCount = 0;
    interrupts();

    logMessage("Coin: Processing " + String(pulsesToProcess) + " pulses.");

    if (pulsesToProcess > 0 && pulsesToProcess < (sizeof(pulseValues) / sizeof(pulseValues[0]))) {
      int coinValueCents = pulseValues[pulsesToProcess];
      if (coinValueCents > 0) {
        credit += (float)coinValueCents / 100.0;
        logMessage("Coin accepted: " + String(pulsesToProcess) + " pulses -> " + String((float)coinValueCents / 100.0, 2) + " EUR. New credit: " + String(credit, 2) + " EUR");
        
        preferences.begin("hanimat", false);
        preferences.putFloat("credit", credit);
        preferences.end();

        displayNeedsUpdate = true;
        lastUserInteractionTime = millis();
        currentSystemState = CurrentSystemState::USER_INTERACTION;
        ledcWriteTone(0, 1200); delay(100); ledcWriteTone(0,0);
      } else {
        logMessage("Coin: " + String(pulsesToProcess) + " pulses has a value of 0 (invalid pulse count).");
      }
    } else {
      logMessage("Coin: Invalid pulse count rejected: " + String(pulsesToProcess));
    }
  }
}

/**
 * @brief Processes bill pulses after a timeout to group them into a single bill event.
 */
void processBillAcceptorPulses() {
  // Ignore pulses immediately after a relay change to prevent electrical noise
  if (millis() - lastRelayChangeTime < 1000) {
    if (billAcceptorPulseCount > 0) {
      logMessage("Bill: Pulses ignored (noise after relay action). Count: " + String(billAcceptorPulseCount));
      noInterrupts(); billAcceptorPulseCount = 0; interrupts();
    }
    return;
  }

  if (billAcceptorPulseCount > 0 && (millis() - lastBillPulseEdgeTime > BILL_GROUP_PROCESSING_TIMEOUT_MS)) {
    int pulsesToProcess;
    noInterrupts();
    pulsesToProcess = billAcceptorPulseCount;
    billAcceptorPulseCount = 0;
    interrupts();

    logMessage("Bill: Processing " + String(pulsesToProcess) + " pulses.");

    if (pulsesToProcess > 0 && pulsesToProcess < (sizeof(billValues) / sizeof(billValues[0]))) {
      int billValueEuros = billValues[pulsesToProcess];
      if (billValueEuros > 0) {
        credit += billValueEuros;
        logMessage("Bill accepted: " + String(pulsesToProcess) + " pulses -> " + String(billValueEuros) + " EUR. New credit: " + String(credit, 2) + " EUR");
        
        preferences.begin("hanimat", false);
        preferences.putFloat("credit", credit);
        preferences.end();

        displayNeedsUpdate = true;
        lastUserInteractionTime = millis();
        currentSystemState = CurrentSystemState::USER_INTERACTION;
        ledcWriteTone(0, 1000); delay(150); ledcWriteTone(0,0);
      } else {
        logMessage("Bill: " + String(pulsesToProcess) + " pulses has a value of 0.");
      }
    } else {
      logMessage("Bill: Invalid pulse count rejected: " + String(pulsesToProcess));
    }
  }
   
  // Inhibit the bill acceptor while pulses are being received, re-enable when done.
  digitalWrite(BILL_INHIBIT_PIN, (billAcceptorPulseCount > 0) ? HIGH : LOW);
}

/**
 * @brief Displays a centered, two-line error message on the TFT for a short duration.
 * @param line1 The first (main) line of the error message.
 * @param line2 The second (optional) line of the error message.
 */
void displayErrorMessage(const String &line1, const String &line2) {
    logMessage("Display Error: " + line1 + (line2.length() > 0 ? " | " + line2 : ""));
    currentSystemState = CurrentSystemState::ERROR_DISPLAY;
    tft.fillScreen(ILI9341_BLACK);

    int16_t x1, y1;
    uint16_t w, h;

    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(ILI9341_RED);

    // Line 1
    tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 100);
    tft.println(line1);

    // Line 2 (if present)
    if (line2.length() > 0) {
        tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor((tft.width() - w) / 2, 130);
        tft.println(line2);
    }

    playErrorSound();
    displayNeedsUpdate = false;
    unsigned long errorTime = millis();
    while(millis() - errorTime < 3000) { // Show error for 3 seconds
        server.handleClient();
        delay(10);
    }
    resetDisplayToDefault();
}


// =================================================================
//                      WEB SERVER HANDLERS
// =================================================================

/**
 * @brief Handles requests to the root URL ("/"). Shows login or dashboard.
 */
void handleRoot() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) {
    showLoginPage();
  } else {
    showDashboard();
  }
}

/**
 * @brief Handles the login form submission.
 */
void handleLogin() {
  lastActivityTimeWeb = millis();
  if (server.hasArg("password") && server.arg("password") == savedPassword) {
    isAuthenticated = true;
    logMessage("Web: Login successful.");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  } else {
    logMessage("Web: Login failed.");
    showLoginPage();
  }
}

/**
 * @brief Handles the change password form submission.
 */
void handleChangePasswordWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("newPassword")) {
    String newPass = server.arg("newPassword");
    if (newPass.length() >= 4) {
        savedPassword = newPass;
        preferences.begin("hanimat", false);
        preferences.putString("password", savedPassword);
        preferences.end();
        logMessage("Web: Admin password changed.");
        server.send(200, "text/html", "Passwort geändert. <meta http-equiv='refresh' content='2;url=/' />");
    } else {
        server.send(400, "text/html", "Passwort zu kurz (min. 4 Zeichen). <meta http-equiv='refresh' content='2;url=/' />");
    }
  } else { server.send(400, "text/plain", "New password missing."); }
}

/**
 * @brief Handles updating the price of a slot.
 */
void handleUpdatePriceWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot") && server.hasArg("price")) {
    int slot = server.arg("slot").toInt();
    float price = server.arg("price").toFloat();
    if (slot >= 0 && slot < activeSlots && price >= 0) {
      slotPrices[slot] = price;
      preferences.begin("hanimat", false);
      preferences.putFloat(("price" + String(slot)).c_str(), price);
      preferences.end();
      logMessage("Web: Price for slot " + String(slot + 1) + " changed to " + String(price, 2) + " EUR.");
      server.send(200, "text/html", "Preis aktualisiert. <meta http-equiv='refresh' content='1;url=/' />");
      displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", "Invalid input."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Handles refilling a single slot.
 */
void handleRefillWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      if (!slotLocked[slot]) {
          slotAvailable[slot] = true;
          preferences.begin("hanimat", false);
          preferences.putBool(("avail" + String(slot)).c_str(), true);
          preferences.end();
          logMessage("Web: Slot " + String(slot + 1) + " refilled.");
          checkOverallStockLevel();
          server.send(200, "text/html", "Fach aufgefuellt. <meta http-equiv='refresh' content='1;url=/' />");
          displayNeedsUpdate = true;
      } else { server.send(400, "text/html", String("Fach ") + (slot+1) + " ist gesperrt. <meta http-equiv='refresh' content='2;url=/' />");}
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Handles manually adding or removing credit.
 */
void handleAddCreditWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("amount")) {
    float amount = server.arg("amount").toFloat();
    if (amount != 0) {
        credit += amount;
        preferences.begin("hanimat", false);
        preferences.putFloat("credit", credit);
        preferences.end();
        logMessage("Web: Credit adjusted by " + String(amount, 2) + " EUR. New credit: " + String(credit, 2) + " EUR.");
        server.send(200, "text/html", "Guthaben angepasst. <meta http-equiv='refresh' content='1;url=/' />");
        displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", "Amount is 0."); }
  } else { server.send(400, "text/plain", "Amount missing."); }
}

/**
 * @brief Handles resetting the credit to zero.
 */
void handleResetCreditWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  credit = 0.0;
  preferences.begin("hanimat", false);
  preferences.putFloat("credit", credit);
  preferences.end();
  logMessage("Web: Credit reset to 0.");
  server.send(200, "text/html", "Guthaben zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/' />");
  displayNeedsUpdate = true;
}

/**
 * @brief Handles refilling all available (and not locked) slots.
 */
void handleRefillAllWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  preferences.begin("hanimat", false);
  for (int i = 0; i < activeSlots; i++) {
    if (!slotLocked[i]) {
        slotAvailable[i] = true;
        preferences.putBool(("avail" + String(i)).c_str(), true);
    }
  }
  preferences.end();
  logMessage("Web: All unlocked slots have been refilled.");
  checkOverallStockLevel();
  server.send(200, "text/html", "Alle Faecher aufgefuellt. <meta http-equiv='refresh' content='1;url=/' />");
  displayNeedsUpdate = true;
}

/**
 * @brief Triggers a single relay for testing purposes.
 */
void handleTriggerRelayWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      logMessage("Web: Testing relay for slot " + String(slot + 1));
      controlSlotRelay(slot, true); delay(1000); controlSlotRelay(slot, false);
      server.send(200, "text/html", String("Relais Fach ") + (slot+1) + " ausgeloest. <meta http-equiv='refresh' content='1;url=/' />");
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Triggers all relays in sequence for testing.
 */
void handleTriggerAllRelaysWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  logMessage("Web: Testing all relays...");
  for (int i = 0; i < activeSlots; i++) {
    controlSlotRelay(i, true); delay(300); controlSlotRelay(i, false); delay(100);
  }
  server.send(200, "text/html", "Alle Relais ausgeloest. <meta http-equiv='refresh' content='1;url=/' />");
}

/**
 * @brief Sets static IP configuration and reboots.
 */
void handleSetStaticIPWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("static_ip") && server.hasArg("gateway") && server.hasArg("subnet")) {
    preferences.begin("hanimat", false);
    preferences.putString("static_ip", server.arg("static_ip"));
    preferences.putString("gateway", server.arg("gateway"));
    preferences.putString("subnet", server.arg("subnet"));
    if (server.hasArg("dns1")) preferences.putString("dns1", server.arg("dns1")); else preferences.remove("dns1");
    preferences.end();
    logMessage("Web: Static IP settings saved. Restart required.");
    server.send(200, "text/html", "Netzwerkeinstellungen gespeichert. Neustart in 5 Sek... <meta http-equiv='refresh' content='5;url=/' />");
    delay(5000); ESP.restart();
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Updates the total number of active slots.
 */
void handleUpdateSlotsWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("maxSlots")) {
    int newNumSlots = server.arg("maxSlots").toInt();
    if (newNumSlots > 0 && newNumSlots <= MAX_SLOTS) {
      activeSlots = newNumSlots;
      preferences.begin("hanimat", false);
      preferences.putInt("activeSlots", activeSlots);
      // Initialize new slots if they don't exist in preferences
      for(int i = 0; i < activeSlots; i++) {
          if(!preferences.isKey(("avail" + String(i)).c_str())) {
              slotAvailable[i] = true;
              preferences.putBool(("avail" + String(i)).c_str(), true);
          }
            if(!preferences.isKey(("price" + String(i)).c_str())) {
              slotPrices[i] = 5.0f;
              preferences.putFloat(("price" + String(i)).c_str(), 5.0f);
          }
      }
      preferences.end();
      logMessage("Web: Number of active slots set to " + String(activeSlots));
      server.send(200, "text/html", "Anzahl Faecher aktualisiert. Neustart empfohlen. <meta http-equiv='refresh' content='2;url=/' />");
      displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", String("Invalid slot count (1-") + MAX_SLOTS + ")."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Toggles the locked state of a slot.
 */
void handleToggleSlotLockWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      slotLocked[slot] = !slotLocked[slot];
      preferences.begin("hanimat", false);
      preferences.putBool(("locked" + String(slot)).c_str(), slotLocked[slot]);
      preferences.end();
      logMessage("Web: Slot " + String(slot + 1) + (slotLocked[slot] ? " locked." : " unlocked."));
      server.send(200, "text/html", "Fachstatus geaendert. <meta http-equiv='refresh' content='1;url=/' />");
      displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Provides log data as plain text for the web UI.
 */
void handleLogDataRequest() {
  lastActivityTimeWeb = millis();
  if (!isAuthenticated) {
    server.send(401, "text/plain", "Not authorized.");
    return;
  }

  String logContent = "";
  int startIdx = logIndex;
  for (int i = 0; i < MAX_LOG_LINES; i++) {
    int currentReadPos = (startIdx + i) % MAX_LOG_LINES;
    if (logBuffer[currentReadPos].length() > 0) {
      logContent += logBuffer[currentReadPos] + "\n";
    }
  }
  server.send(200, "text/plain", logContent);
}


// --- OTA Update Handlers ---

/**
 * @brief Displays the OTA update page.
 */
void handleOTAUpdatePage() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  lastActivityTimeWeb = millis();
  // The actual HTML is generated by showDashboard() JS, this just redirects.
  server.sendHeader("Location", "/#ota-update-section", true);
  server.send(302, "text/plain", "");
}

/**
 * @brief Handles the binary file upload for OTA updates.
 */
void handleOTAFileUpload() {
  if (!isAuthenticated) { return; }
  lastActivityTimeWeb = millis();
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUpdateInProgress = true;
    otaStatusMessage = "Upload started... Writing firmware.";
    logMessage("OTA: Upload started: " + upload.filename);
    displayOTAMessageTFT("Update gestartet", "Nicht ausschalten!", "", ILI9341_ORANGE);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      logMessage("OTA ERROR: Update.begin() failed. Error: " + String(Update.getError()));
      otaStatusMessage = "ERROR: Could not start update (Error: " + String(Update.getError()) + ")";
      displayOTAMessageTFT("Update Fehler!", "Start fehlgeschlagen", "Details im Log", ILI9341_RED);
      otaUpdateInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      logMessage("OTA ERROR: Update.write() failed. Error: " + String(Update.getError()));
      otaStatusMessage = "ERROR: Failed to write firmware (Error: " + String(Update.getError()) + ")";
      displayOTAMessageTFT("Update Fehler!", "Schreibfehler", "Details im Log", ILI9341_RED);
      otaUpdateInProgress = false;
      Update.end(false);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUpdateInProgress) {
        if (Update.end(true)) {
            otaStatusMessage = "Update successful! ESP32 is restarting...";
            logMessage("OTA: Update finished successfully. Restarting ESP32.");
            displayOTAMessageTFT("Update fertig.", "Automat startet neu", "", ILI9341_GREEN);
            server.sendHeader("Location", "/otaupdate", true);
            server.send(302, "text/plain", "Update successful, restarting...");
            delay(3000);
            ESP.restart();
        } else {
            Update.printError(Serial);
            logMessage("OTA ERROR: Update.end() failed. Error: " + String(Update.getError()));
            otaStatusMessage = "ERROR: Update failed (Error: " + String(Update.getError()) + ")";
            displayOTAMessageTFT("Update Fehler!", "Abschluss fehlgeschl.", "Details im Log", ILI9341_RED);
        }
    }
    otaUpdateInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
      logMessage("OTA: Upload aborted by client.");
      if(otaUpdateInProgress) Update.end(false);
      otaUpdateInProgress = false;
  }
}

/**
 * @brief Handles timing configuration form submission.
 */
void handleSaveTimingConfig() {
    if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();

    preferences.begin("hanimat", false);
    preferences.putULong("coinDelay", server.arg("coin_delay").toInt());
    preferences.putULong("billIsrDeb", server.arg("bill_isr_debounce").toInt());
    preferences.putULong("billGrpTout", server.arg("bill_group_timeout").toInt());
    preferences.putULong("dispTime", server.arg("disp_time").toInt());
    preferences.putULong("keypadTime", server.arg("keypad_time").toInt());
    preferences.putULong("slotSelTime", server.arg("slot_sel_time").toInt());
    preferences.putULong("dispTimeout", server.arg("disp_timeout").toInt());
    preferences.end();
    
    logMessage("Web: Timing settings saved. A restart is recommended.");
    otaStatusMessage = "Zeiteinstellungen gespeichert! Neustart empfohlen.";
    server.sendHeader("Location", "/#timing-config", true);
    server.send(302, "text/plain", "");
}

/**
 * @brief Handles Telegram and stock notification form submission.
 */
void handleSaveTelegramConfig() {
    if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();

    telegramEnabled = server.hasArg("tg_enabled");
    telegramNotifyOnSale = server.hasArg("notify_sale");
    telegramNotifyAlmostEmpty = server.hasArg("notify_almost_empty");
    telegramNotifyEmpty = server.hasArg("notify_empty");
    telegramBotToken = server.arg("tg_token");
    telegramChatId = server.arg("tg_chat_id");
    almostEmptyThreshold = server.arg("almost_empty_threshold").toInt();

    preferences.begin("hanimat", false);
    preferences.putBool("tgEnabled", telegramEnabled);
    preferences.putString("tgToken", telegramBotToken);
    preferences.putString("tgChatId", telegramChatId);
    preferences.putInt("tgAlmostThres", almostEmptyThreshold);
    preferences.putBool("tgNotifySale", telegramNotifyOnSale);
    preferences.putBool("tgNotifyAlmost", telegramNotifyAlmostEmpty);
    preferences.putBool("tgNotifyEmpty", telegramNotifyEmpty);
    preferences.end();

    bot.updateToken(telegramBotToken);

    logMessage("Web: Telegram & notification settings saved.");
    otaStatusMessage = "Einstellungen gespeichert!";
    server.sendHeader("Location", "/#telegram-config", true);
    server.send(302, "text/plain", "");
}

/**
 * @brief Sends a test message to the configured Telegram chat.
 */
void handleSendTestTelegram() {
    if (!isAuthenticated) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();
    String message = "👋 Hallo vom HANIMAT! Dies ist eine Testnachricht. Alles scheint zu funktionieren. Version: " + FIRMWARE_VERSION;
    sendTelegramMessage(message);
    otaStatusMessage = "Testnachricht gesendet! Überprüfen Sie Ihren Telegram-Chat.";
    server.sendHeader("Location", "/#telegram-config", true);
    server.send(302, "text/plain", "");
}

void handleDisplayConfigPage() {
  server.sendHeader("Location", "/#display-config", true);
  server.send(302);
}

void handleSaveDisplayConfig() {
  if (!isAuthenticated) { server.send(401); return; }

  // Slogan-Text holen und auf maximale Länge kürzen
  String newSlogan = server.arg("slogan");
  if (newSlogan.length() > SLOGAN_MAX_LENGTH) {
    newSlogan = newSlogan.substring(0, SLOGAN_MAX_LENGTH);
  }

  // Footer-Text holen und ebenfalls kürzen (sicherheitshalber)
  String newFooter = server.arg("footer");
  if (newFooter.length() > 30) { // Limit für Footer
    newFooter = newFooter.substring(0, 30);
  }

  // Globale Variablen aktualisieren
  displaySlogan = newSlogan;
  displayFooter = newFooter;

  // Im Speicher sichern
  preferences.begin("hanimat", false);
  preferences.putString("dispSlogan", displaySlogan);
  preferences.putString("dispFooter", displayFooter);
  preferences.end();
  displayNeedsUpdate = true; 
   
  logMessage("Web: Display texts updated.");
  otaStatusMessage = "Display-Texte gespeichert!";
  server.sendHeader("Location", "/#display-config", true);
  server.send(302);
}

// Redirects for settings pages (content is loaded via JS)
void handleTimingConfigPage() { server.sendHeader("Location", "/#timing-config", true); server.send(302); }
void handleTelegramConfigPage() { server.sendHeader("Location", "/#telegram-config", true); server.send(302); }


// =================================================================
//                      HTML PAGE GENERATORS
// =================================================================

/**
 * @brief Generates and sends the HTML for the login page.
 */
void showLoginPage() {
  String html = R"HTML(
<!DOCTYPE html><html><head><title>Login | HANIMAT</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>
:root { --primary: #FFA500; --primary-hover: #FF8C00; --background: #1E1E1E; --text: #E0E0E0; --card-bg: #2D2D2D; }
body { min-height: 100vh; display: grid; place-items: center; font-family: 'Inter', system-ui, sans-serif; background: var(--background); color: var(--text); }
.login-container { width: 90%; max-width: 400px; padding: 2rem; background: var(--card-bg); border-radius: 1.5rem; box-shadow: 0 8px 32px rgba(0,0,0,0.3); display: flex; flex-direction: column; align-items: center; }
.logo { margin-bottom: 1rem; font-size: 2rem; font-weight: 700; color: var(--primary); text-align: center; }
h1 { color: var(--primary); font-size: 1.875rem; margin-bottom: 1.5rem; text-align: center; }
form { width: 100%; display: flex; flex-direction: column; align-items: center; }
input { width: 100%; padding: 0.875rem; border: 2px solid #444; border-radius: 0.75rem; font-size: 1rem; background: #1E1E1E; color: var(--text); transition: border-color 0.2s; box-sizing: border-box; }
input:focus { outline: none; border-color: var(--primary); }
button { width: 100%; padding: 1rem; background: var(--primary); color: white; border: none; border-radius: 0.75rem; font-size: 1rem; font-weight: 600; cursor: pointer; transition: background 0.2s; margin-top: 1.5rem; box-sizing: border-box;}
button:hover { background: var(--primary-hover); }
</style></head><body>
<div class='login-container'>
  <div class='logo'>HANIMAT</div>
  <h1>Admin Login</h1>
  <form action='/login' method='post'>
    <input type='password' id='password' name='password' placeholder='Passwort' required>
    <button type='submit'>Anmelden</button>
  </form>
</div>
</body></html>
)HTML";
  server.send(200, "text/html", html);
}

/**
 * @brief Generates and sends the main dashboard HTML page (Single Page Application).
 */
void showDashboard() {
  String html = R"HTML(
<!DOCTYPE html><html lang='de'><head><title>Admin Panel | HANIMAT</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>
:root { --primary: #FFA500; --primary-hover: #FF8C00; --background: #121212; --text: #E0E0E0; --card-bg: #1E1E1E; --sidebar-bg: #1A1A1A; --sidebar-width: 260px; --border-color: #333; --input-bg: #2C2C2C; --success: #4CAF50; --error: #F44336; --info: #2196F3;}
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: 'Inter', system-ui, sans-serif; background: var(--background); color: var(--text); display: flex; min-height: 100vh; font-size: 14px; }
.sidebar { width: var(--sidebar-width); background: var(--sidebar-bg); padding: 1.5rem 1rem; border-right: 1px solid var(--border-color); position: fixed; height: 100vh; overflow-y: auto; transition: transform 0.3s ease; z-index: 1000;}
.main-content { flex: 1; margin-left: var(--sidebar-width); padding: 1.5rem; transition: margin-left 0.3s ease; }
.logo { font-size: 1.8rem; font-weight: 700; color: var(--primary); margin-bottom: 2rem; text-align: center; }
.nav-menu { list-style: none; }
.nav-item { margin-bottom: 0.5rem; }
.nav-link { display: flex; align-items: center; gap: 0.8rem; padding: 0.8rem 1rem; border-radius: 0.5rem; color: #ccc; text-decoration: none; transition: all 0.2s ease; font-weight: 500; }
.nav-link:hover, .nav-link.active { background: var(--primary); color: var(--background); }
.card { background: var(--card-bg); border-radius: 1rem; padding: 1.5rem; box-shadow: 0 6px 12px rgba(0,0,0,0.3); margin-bottom: 1.5rem; }
h1, h2 { color: var(--primary); margin-bottom: 1rem; font-weight: 600; } h1 { font-size: 1.8rem; } h2 { font-size: 1.5rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 1.5rem; }
.stat-card { background: var(--input-bg); padding: 1.5rem; border-radius: 0.75rem; text-align: center; }
.stat-label { font-size: 0.9rem; color: #aaa; margin-bottom: 0.5rem; }
.stat-value { font-size: 2rem; font-weight: 700; color: var(--primary); }
table { width: 100%; border-collapse: collapse; background: var(--card-bg); border-radius: 0.75rem; overflow: hidden; }
th, td { padding: 0.9rem 1rem; text-align: left; border-bottom: 1px solid var(--border-color); }
th { background: var(--input-bg); font-weight: 600; }
tr:hover { background: #252525; }
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 0.5rem; padding: 0.7rem 1.2rem; border-radius: 0.5rem; text-decoration: none; transition: all 0.2s ease; border: none; cursor: pointer; font-weight: 500; white-space: nowrap;}
.btn-primary { background: var(--primary); color: var(--background); } .btn-primary:hover { background: var(--primary-hover); }
.btn-secondary { background: var(--input-bg); color: var(--text); border: 1px solid var(--border-color); } .btn-secondary:hover { background: #383838; }
.btn-danger { background: var(--error); color: white; } .btn-danger:hover { background: #D32F2F; }
.btn-icon { padding: 0.6rem; background: transparent; border: 1px solid var(--border-color); color: #ccc; } .btn-icon:hover { background: var(--input-bg); color: var(--primary); }
.badge { padding: 0.3rem 0.6rem; border-radius: 0.3rem; font-size: 0.8rem; font-weight: 500; }
.locked-badge { background: var(--error); color: white; } .available-badge { background: #64B5F6; color: white; } .empty-badge { background: #757575; color: white; } .success-badge { background: var(--success); color: white; }
input[type='text'], input[type='number'], input[type='password'], select { width: 100%; padding: 0.8rem; border: 1px solid var(--border-color); border-radius: 0.5rem; font-size: 0.9rem; background: var(--input-bg); color: var(--text); }
.form-group { margin-bottom: 1.2rem; } .form-group label { display: block; margin-bottom: 0.4rem; font-weight: 500; }
.form-inline { display: flex; gap: 0.8rem; align-items: flex-end; } .form-inline input, .form-inline select { flex: 1; }
#log-console { background: #000; color: #0F0; padding: 1rem; height: 250px; overflow-y: auto; border-radius: 0.5rem; font-family: 'Courier New', monospace; white-space: pre-wrap; }
.mobile-header { display: none; }
.sidebar-footer { margin-top: auto; padding-top: 1rem; border-top: 1px solid var(--border-color); font-size: 0.8rem; color: #888; text-align: center;}
.status-message { padding: 1rem; border-radius: 0.5rem; margin-top: 1rem; text-align: center; font-weight: 500;}
.status-success { background-color: var(--success); color: white;} .status-error { background-color: var(--error); color: white;} .status-info { background-color: var(--info); color: white;}
.checkbox-label { display: flex; align-items: center; gap: 0.5rem; cursor: pointer; } .checkbox-label input { width: auto; }
@media (max-width: 768px) {
  .sidebar { transform: translateX(-100%); width: 80%; max-width: 300px; }
  .sidebar.active { transform: translateX(0); }
  .main-content { margin-left: 0; padding-top: 5rem; }
  .mobile-header { display: flex; justify-content: space-between; align-items: center; position: fixed; top: 0; left: 0; width: 100%; background: var(--sidebar-bg); padding: 0.8rem 1rem; z-index: 1002; }
  .menu-toggle { background: none; border: none; color: var(--primary); font-size: 1.8rem; cursor: pointer; }
  .form-inline { flex-direction: column; align-items: stretch; }
  table { display: block; overflow-x: auto; white-space: nowrap; }
}
</style></head><body>
<div class='mobile-header'><div class='logo'>HANIMAT</div><button class='menu-toggle' onclick='toggleSidebar()'>&#9776;</button></div>
<aside class='sidebar'>
  <div class='logo'>HANIMAT</div>
  <ul class='nav-menu'>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link active' onclick='showSection("dashboard")'>Dashboard</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("slots-config")'>Slotkonfiguration</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("display-config")'>Anzeige</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("timing-config")'>Zeiteinstellungen</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("telegram-config")'>Benachrichtigungen</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("network-config")'>Netzwerk</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("password-config")'>Passwort</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("logs")'>Logs</a></li>
    <li class='nav-item'><a href='javascript:void(0)' class='nav-link' onclick='showSection("ota-update-section")'>System Update</a></li>
  </ul>
  <div class='sidebar-footer'>Version: 
)HTML";
  html += FIRMWARE_VERSION;
  html += R"HTML(<br><a href='http://www.hanimat.at' target='_blank'>www.hanimat.at</a></div></aside>
<main class='main-content'>
  <!-- Dashboard Section -->
  <section id='dashboard' class='content-section'><h1>Dashboard</h1><div class='grid'>
    <div class='stat-card'><div class='stat-label'>Verfügbare Fächer</div><div class='stat-value'>)HTML";
  html += String(countAvailableSlots()) + "/" + String(activeSlots) + "</div></div>";
  html += "<div class='stat-card'><div class='stat-label'>Aktuelles Guthaben</div><div class='stat-value'>" + String(credit, 2) + " &euro;</div></div>";
  html += "<div class='stat-card'><div class='stat-label'>System Uptime</div><div class='stat-value'>" + String(millis()/60000) + " min</div></div></div>";
  html += R"HTML(
    <div class='card' style='margin-top: 1.5rem;'><h2>Schnellaktionen</h2>
      <div class='form-inline' style='margin-bottom: 1.5rem;'>
        <form action='/addcredit' method='post' class='form-inline' style='flex-grow: 1;'><div class='form-group' style='margin-bottom:0; flex-grow: 1;'><label for='addAmount'>Guthaben +/-</label><input type='number' step='0.01' id='addAmount' name='amount' placeholder='Betrag' required></div><button type='submit' class='btn btn-primary'>OK</button></form>
        <form action='/resetcredit' method='post'><button type='submit' class='btn btn-danger'>Guthaben Reset</button></form>
      </div>
      <div class='form-inline' style='gap:1rem;'><form action='/refillall' method='post' style='flex:1;'><button type='submit' class='btn btn-secondary' style='width:100%;'>Alle Fächer auffüllen</button></form><form action='/triggerallrelays' method='post' style='flex:1;'><button type='submit' class='btn btn-secondary' style='width:100%;'>Alle Relais testen</button></form></div>
    </div>
    <h2>Fachübersicht</h2><table><thead><tr><th>Fach</th><th>Status</th><th>Preis (&euro;)</th><th>Aktionen</th></tr></thead><tbody>
)HTML";
  for (int i = 0; i < activeSlots; i++) {
    String statusText, statusClass;
    if (slotLocked[i]) { statusText = "Gesperrt"; statusClass = "locked-badge"; }
    else if (!slotAvailable[i]) { statusText = "Leer"; statusClass = "empty-badge"; }
    else { statusText = "Verfügbar"; statusClass = "success-badge"; }
    html += "<tr><td>#" + String(i+1) + "</td><td><span class='badge " + statusClass + "'>" + statusText + "</span></td><td>" + String(slotPrices[i], 2) + "</td><td><div class='form-inline' style='gap:0.3rem;'>";
    html += "<form action='/toggleslotlock' method='post'><input type='hidden' name='slot' value='" + String(i) + "'><button type='submit' class='btn btn-icon' title='" + (slotLocked[i] ? "Entsperren" : "Sperren") + "'>" + (slotLocked[i] ? "&#128274;" : "&#128275;") + "</button></form>";
    html += "<form action='/triggerrelay' method='post'><input type='hidden' name='slot' value='" + String(i) + "'><button type='submit' class='btn btn-icon' title='Test Relais'>&#9889;</button></form>";
    html += "<form action='/refill' method='post'><input type='hidden' name='slot' value='" + String(i) + "'><button type='submit' class='btn btn-icon' title='Auffüllen'>&#128260;</button></form></div></td></tr>";
  }
  html += "</tbody></table></section>";

  html += R"HTML(
  <!-- Slots Config Section -->
  <section id='slots-config' class='content-section' style='display:none;'><h1>Slotkonfiguration</h1>
    <div class='card'><form action='/updateslots' method='post'><div class='form-group'><label for='maxSlotsInput'>Anzahl aktiver Fächer (1-)HTML";
  html += String(MAX_SLOTS) + R"HTML(:</label><input type='number' id='maxSlotsInput' name='maxSlots' value=')HTML" + String(activeSlots) + R"HTML(' min='1' max=')HTML" + String(MAX_SLOTS) + R"HTML(' required></div><button type='submit' class='btn btn-primary'>Speichern</button></form></div>
    <h2>Preise anpassen</h2><div class='grid'>
)HTML";
  for (int i = 0; i < activeSlots; i++) {
    html += "<div class='card'><form action='/updateprice' method='post'><div class='form-group'><label for='price" + String(i) + "'>Fach #" + String(i+1) + " Preis (&euro;)</label><input type='hidden' name='slot' value='" + String(i) + "'><input type='number' step='0.01' id='price" + String(i) + "' name='price' value='" + String(slotPrices[i],2) + "' required></div><button type='submit' class='btn btn-primary'>Preis Speichern</button></form></div>";
  }
  html += "</div></section>";

// Display Config Section
  html += R"HTML(<section id='display-config' class='content-section' style='display:none;'><h1>Anzeige anpassen</h1><div class='card'>
    <h2>Footer-Texte</h2>
    <form action='/savedisplayconfig' method='post'>
      <div class='form-group'>
        <label for='slogan_input'>Slogan (über dem Footer, max. )HTML" + String(SLOGAN_MAX_LENGTH) + R"HTML( Zeichen):</label>
        <input type='text' id='slogan_input' name='slogan' value=')HTML" + displaySlogan + R"HTML(' maxlength=')HTML" + String(SLOGAN_MAX_LENGTH) + R"HTML('>
      </div>
      <div class='form-group'>
        <label for='footer_input'>Footer-Text (unterste Zeile, max. 30 Zeichen):</label>
        <input type='text' id='footer_input' name='footer' value=')HTML" + displayFooter + R"HTML(' maxlength='30' required>
      </div>
      <button type='submit' class='btn btn-primary'>Speichern</button>
    </form>
  </div></section>)HTML";

  // Timing Config Section
  html += R"HTML(<section id='timing-config' class='content-section' style='display:none;'><h1>Zeiteinstellungen</h1><div class='card'><form action='/savetimingconfig' method='post'>)HTML";
  html += "<div class='form-group'><label for='coin_delay'>Münzverarbeitung Verzoegerung (ms):</label><input type='number' id='coin_delay' name='coin_delay' value='" + String(COIN_PROCESSING_DELAY) + "' required></div>";
  html += "<div class='form-group'><label for='bill_isr_debounce'>Schein ISR Entprellzeit (ms):</label><input type='number' id='bill_isr_debounce' name='bill_isr_debounce' value='" + String(BILL_ISR_DEBOUNCE_MS) + "' required></div>";
  html += "<div class='form-group'><label for='bill_group_timeout'>Schein Gruppen Timeout (ms):</label><input type='number' id='bill_group_timeout' name='bill_group_timeout' value='" + String(BILL_GROUP_PROCESSING_TIMEOUT_MS) + "' required></div>";
  html += "<div class='form-group'><label for='disp_time'>Fach Oeffnungszeit (ms):</label><input type='number' id='disp_time' name='disp_time' value='" + String(DISPENSE_RELAY_ON_TIME) + "' required></div>";
  html += "<div class='form-group'><label for='keypad_time'>Keypad Eingabe Timeout (ms):</label><input type='number' id='keypad_time' name='keypad_time' value='" + String(KEYPAD_INPUT_TIMEOUT) + "' required></div>";
  html += "<div class='form-group'><label for='slot_sel_time'>Fachauswahl Anzeige Timeout (ms):</label><input type='number' id='slot_sel_time' name='slot_sel_time' value='" + String(SLOT_SELECTION_TIMEOUT) + "' required></div>";
  html += "<div class='form-group'><label for='disp_timeout'>Display Timeout (ms):</label><input type='number' id='disp_timeout' name='disp_timeout' value='" + String(DISPLAY_TIMEOUT) + "' required></div>";
  html += R"HTML(<button type='submit' class='btn btn-primary'>Zeiten Speichern</button></form></div></section>)HTML";

  // Telegram Config Section
  html += R"HTML(<section id='telegram-config' class='content-section' style='display:none;'><h1>Benachrichtigungen</h1><div class='card'><form action='/savetelegramconfig' method='post'>)HTML";
  html += "<h2>Telegram Konfiguration</h2>";
  html += String("<div class='form-group'><label class='checkbox-label'><input type='checkbox' name='tg_enabled' ") + (telegramEnabled ? "checked" : "") + "> <b>Telegram-Benachrichtigungen aktivieren</b></label></div>";
  html += "<div class='form-group'><label for='tg_token'>Bot Token:</label><input type='password' id='tg_token' name='tg_token' value='" + telegramBotToken + "'></div>";
  html += "<div class='form-group'><label for='tg_chat_id'>Chat ID:</label><input type='text' id='tg_chat_id' name='tg_chat_id' value='" + telegramChatId + "'></div>";
  html += "<h2>Benachrichtigungs-Optionen</h2>";
  html += String("<div class='form-group'><label class='checkbox-label'><input type='checkbox' name='notify_sale' ") + (telegramNotifyOnSale ? "checked" : "") + "> Bei jedem Verkauf benachrichtigen</label></div>";
  html += String("<div class='form-group'><label class='checkbox-label'><input type='checkbox' name='notify_almost_empty' ") + (telegramNotifyAlmostEmpty ? "checked" : "") + "> Benachrichtigen, wenn Automat fast leer ist</label></div>";
  html += "<div class='form-group'><label for='almost_empty_threshold'>\"Fast leer\" Schwelle (Anzahl Fächer):</label><input type='number' id='almost_empty_threshold' name='almost_empty_threshold' value='" + String(almostEmptyThreshold) + "' required></div>";
  html += String("<div class='form-group'><label class='checkbox-label'><input type='checkbox' name='notify_empty' ") + (telegramNotifyEmpty ? "checked" : "") + "> Benachrichtigen, wenn Automat komplett leer ist</label></div>";
  html += R"HTML(<button type='submit' class='btn btn-primary'>Speichern</button></form><form action='/sendtesttelegram' method='post' style='margin-top: 1rem;'><button type='submit' class='btn btn-secondary'>Testnachricht senden</button></form></div></section>)HTML";

  // Network Config Section
  preferences.begin("hanimat", false);
  String staticIP_val = preferences.getString("static_ip", "");
  String gateway_val = preferences.getString("gateway", "");
  String subnet_val = preferences.getString("subnet", "");
  String dns1_val = preferences.getString("dns1", "8.8.8.8");
  preferences.end();
  html += R"HTML(<section id='network-config' class='content-section' style='display:none;'><h1>Netzwerkeinstellungen</h1><div class='card'>)HTML";
  html += "<p>Aktuelle IP: " + WiFi.localIP().toString() + "</p>";
  html += String("<p>Modus: ") + (staticIP_val.length() > 0 ? "Statische IP" : "DHCP") + "</p>";
  html += R"HTML(<form action='/setstaticip' method='post'>)HTML";
  html += "<div class='form-group'><label for='static_ip_input'>Statische IP (leer für DHCP):</label><input type='text' id='static_ip_input' name='static_ip' value='" + staticIP_val + "'></div>";
  html += "<div class='form-group'><label for='gateway_input'>Gateway:</label><input type='text' id='gateway_input' name='gateway' value='" + gateway_val + "'></div>";
  html += "<div class='form-group'><label for='subnet_input'>Subnetzmaske:</label><input type='text' id='subnet_input' name='subnet' value='" + subnet_val + "'></div>";
  html += "<div class='form-group'><label for='dns1_input'>DNS 1 (optional):</label><input type='text' id='dns1_input' name='dns1' value='" + dns1_val + "'></div>";
  html += R"HTML(<button type='submit' class='btn btn-primary'>Speichern & Neustart</button></form></div></section>)HTML";
   
  // Password Config Section
  html += R"HTML(<section id='password-config' class='content-section' style='display:none;'><h1>Passwort ändern</h1><div class='card'><form action='/changepassword' method='post'><div class='form-group'><label for='newPasswordInput'>Neues Passwort (min. 4 Zeichen):</label><input type='password' id='newPasswordInput' name='newPassword' required></div><button type='submit' class='btn btn-primary'>Passwort Speichern</button></form></div></section>)HTML";

  // Logs Section
  html += R"HTML(<section id='logs' class='content-section' style='display:none;'><h1>Live Logs</h1><div class='card'><div id='log-console'>Lade Logs...</div></div></section>)HTML";

  // OTA Update Section
  html += R"HTML(<section id='ota-update-section' class='content-section' style='display:none;'><h1>System Update (OTA)</h1><div class='card'><h2>Firmware hochladen (.bin Datei)</h2><form method='POST' action='/ota-upload' enctype='multipart/form-data'><input type='file' name='update' accept='.bin' required><br><br><button type='submit' class='btn btn-primary'>Update starten</button></form></div></section>)HTML";

  html += R"HTML(
</main>
<script>
function toggleSidebar() { document.querySelector('.sidebar').classList.toggle('active'); }
function showSection(sectionId) {
  document.querySelectorAll('.content-section').forEach(s => s.style.display = 'none');
  const targetSection = document.getElementById(sectionId);
  if (targetSection) { targetSection.style.display = 'block'; }
  document.querySelectorAll('.nav-link').forEach(l => l.classList.remove('active'));
  let activeLink = document.querySelector(`.nav-link[onclick*='showSection("${sectionId}")']`);
  if(activeLink) activeLink.classList.add('active');
  if (window.innerWidth <= 768 && document.querySelector('.sidebar').classList.contains('active')) { toggleSidebar(); }
  if (sectionId === 'logs') { fetchLogs(); }
}
function fetchLogs(){
  const logConsole = document.getElementById('log-console');
  if (!logConsole) return;
  fetch('/logdata').then(r => r.text()).then(t => { logConsole.textContent = t; logConsole.scrollTop = logConsole.scrollHeight; });
}
document.addEventListener('DOMContentLoaded', () => {
  const hash = window.location.hash.substring(1);
  if (hash && document.getElementById(hash)) { showSection(hash); } else { showSection('dashboard'); }
  if(document.getElementById('log-console')) { setInterval(fetchLogs, 3000); }
});
</script></body></html>
)HTML";
  server.send(200, "text/html; charset=UTF-8", html);
}


// =================================================================
//                      UTILITY FUNCTIONS
// =================================================================

/**
 * @brief Counts the number of slots that are currently available and not locked.
 * @return The count of available slots.
 */
int countAvailableSlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

/**
 * @brief Counts the number of slots that are empty and not locked.
 * @return The count of empty slots.
 */
int countEmptySlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (!slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

/**
 * @brief Checks the overall stock level and sends Telegram notifications if thresholds are met.
 */
void checkOverallStockLevel() {
    int totalAvailable = countAvailableSlots();

    // Check for "almost empty"
    if (telegramNotifyAlmostEmpty && totalAvailable > 0 && totalAvailable <= almostEmptyThreshold && !almostEmptyNotificationSent) {
        String message = "⚠️ INFO: Der HANIMAT ist fast leer!\nVerfügbare Fächer: " + String(totalAvailable);
        sendTelegramMessage(message);
        almostEmptyNotificationSent = true;
        emptyNotificationSent = false; // Reset this flag in case it was set
    }

    // Check for "completely empty"
    else if (telegramNotifyEmpty && totalAvailable == 0 && !emptyNotificationSent) {
        String message = "🚨 ALARM: Der HANIMAT ist komplett ausverkauft! Bitte auffüllen! 😭";
        sendTelegramMessage(message);
        emptyNotificationSent = true;
        almostEmptyNotificationSent = true; // Also considered almost empty
    }

    // Reset flags if stock is high again
    else if (totalAvailable > almostEmptyThreshold) {
        if(almostEmptyNotificationSent || emptyNotificationSent) {
            logMessage("Stock level is high again. Resetting notification flags.");
        }
        almostEmptyNotificationSent = false;
        emptyNotificationSent = false;
    }
}