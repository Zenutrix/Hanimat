/**
 * @file main.cpp
 * @author Thomas Schöpf / Hanimat
 * @brief Firmware für die HANIMAT Verkaufsmaschine basierend auf der ESP32 Plattform.
 * @version 1.4.1-ec
 * @date 02-02-2026
 *
 * © Copyright Thomas Schöpf
 *
 * Der HANIMAT steht unter der **Creative Commons Namensnennung-NichtKommerziell-Weitergabe unter gleichen Bedingungen 4.0 International (CC BY-NC-SA 4.0)** Lizenz.
 * Urheber des Projekts ist Thomas Schöpf (Hanimat-Projekt).
 * Weitere Informationen finden Sie unter: www.hanimat.at
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
#include <HTTPUpdate.h> // Für Online-Updates
#include <UniversalTelegramBot.h> // Telegram Bot Library
#include <ArduinoJson.h>
#include <HTTPClient.h>

// =================================================================
//  LOGGING SYSTEM
// =================================================================
#define MAX_LOG_LINES 50
String logBuffer[MAX_LOG_LINES]; 
int logIndex = 0;

/**
 * @brief Zentrale Log-Funktion mit Datum und Uhrzeit
 */
void logMessage(const String& msg) {
  struct tm timeinfo;
  char timeString[20];
  
  // Versuche die aktuelle Uhrzeit zu holen
  if (getLocalTime(&timeinfo)) {
    // Format: Tag.Monat. Stunde:Minute:Sekunde (z.B. 13.02. 16:21:05)
    strftime(timeString, sizeof(timeString), "%d.%m. %H:%M:%S", &timeinfo);
  } else {
    // Fallback falls NTP (Internetzeit) noch nicht da ist
    snprintf(timeString, sizeof(timeString), "%lus", millis() / 1000);
  }

  String fullEntry = "[" + String(timeString) + "] " + msg;
  
  Serial.println(fullEntry);
  logBuffer[logIndex] = fullEntry;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}


#include "SumUpController.h" // SumUp Klasse muss im selben Ordner liegen

// --- Custom Fonts ---
#include "fonts/Poppins_Black_14.h"
#include "fonts/Poppins_Regular_10.h"
#include "fonts/Poppins_Regular_7.h"


// =================================================================
//                      FIRMWARE VERSION
// =================================================================
const String FIRMWARE_VERSION = "V1.4.1-ec";

// =================================================================
//                      CONFIGURATION CONSTANTS
// =================================================================

// --- Online Update Configuration ---
const char* UPDATE_VERSION_URL = "https://www.hanimat.at/update/version.json";
const char* UPDATE_FIRMWARE_URL = "https://www.hanimat.at/update/firmware.bin";

// --- Vending Machine Configuration ---
const int DEFAULT_MAX_SLOTS = 16;
const int MAX_SLOTS = 16;

// --- Timing and Timeout Values (in milliseconds) ---
unsigned long COIN_PROCESSING_DELAY = 120;
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
#define SUMUP_BUTTON_PIN 35 // ACHTUNG: GPIO 35 hat keinen internen Pullup! Externer Widerstand nötig.

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
  OTA_UPDATE,             // OTA update is in progress
  SUMUP_PENDING          // NEW: Waiting for SumUp payment
};
CurrentSystemState currentSystemState = CurrentSystemState::IDLE;

// =================================================================
//                      GLOBAL VARIABLES
// =================================================================

// --- SumUp Konfiguration ---
String sumupApiKey = "";
String sumupMerchantId = "";
String sumupReaderId = "";
bool sumupEnabled = false;
unsigned long sumupTimeout = 60000; // Millisekunden (Default 60s)

// Controller Instanz
SumUpController sumUp("", "", "");

// --- SumUp Asynchrone Status Variablen ---
bool isSumUpTransactionActive = false;
String currentSumUpTxId = "";
float pendingSumUpAmount = 0.0;
unsigned long sumUpStartTime = 0;
unsigned long lastSumUpCheckTime = 0;


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

// --- Colors ---
#define HANIMAT_BG        ILI9341_BLACK
#define HANIMAT_HEADER    ILI9341_YELLOW
#define HANIMAT_TEXT      ILI9341_WHITE
#define HANIMAT_ACCENT    ILI9341_ORANGE
#define HANIMAT_SUCCESS   ILI9341_GREEN
#define HANIMAT_ERROR     ILI9341_RED
#define HANIMAT_INFO      ILI9341_CYAN
#define HANIMAT_DIVIDER   0x7BEF // Light Grey

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

// --- Hanimat Status Network ---
bool statusEnabled = true; // Variable zum Deaktivieren/Aktivieren
const char* statusServerUrl = "https://status.hanimat.at/api.php";
const char* statusApiKey = "HanimatKeyStatus";
unsigned long lastStatusPing = 0;
const unsigned long statusInterval = 3600000; // Alle 60 Minuten (in ms)

// Flag zur Erkennung eines offenen Pins
bool resetPinIsFloating = false; 

// --- Payment & Credit ---
float credit = 0.0;
volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;

volatile unsigned long billAcceptorPulseCount = 0;
volatile unsigned long lastBillPulseEdgeTime = 0;
volatile unsigned long lastBillDebounceEdgeTime = 0;

float lastCreditSaved = 0.0;
unsigned long lastCreditChangeTime = 0;
const unsigned long NVS_SAVE_DELAY = 10000; // 10 Sekunden warten nach letztem Einwurf

// --- Display Customization ---
const int SLOGAN_MAX_LENGTH = 24; // Zeichenlimit für den Slogan
String displaySlogan = "";
String displayFooter = "www.hanimat.at";

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
void displayOTAMessageTFT(String line1, String line2 = "", String line3 = "", uint16_t color = HANIMAT_ACCENT);
void checkOverallStockLevel();
void sendHanimatStatusPing();
void handleSumUpPaymentInitiation(); 
void drawPageHeader(String title, uint16_t color = HANIMAT_HEADER);
void saveCreditToNVS(bool force = false);
// Neue Funktionen für Online Update
void handleCheckOnlineUpdate();
void handleStartOnlineUpdate();

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
bool checkRelayBoardOnline();
void sendTelegramMessage(String message);

// Interrupt Service Routines
void IRAM_ATTR coinAcceptorISR();
void IRAM_ATTR billAcceptorISR();

// =================================================================
//                      HELPER FUNCTIONS
// =================================================================

/**
 * @brief Standard UI Helper: Draws the Header Bar with Wifi Status
 */
void drawPageHeader(String title, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  
  // 1. Draw Title
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(color);
  tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 35); // Standard Header Height
  tft.println(title);

  // 2. Draw Divider Line
  tft.drawFastHLine(10, 48, tft.width() - 20, HANIMAT_DIVIDER);

  // 3. Draw WiFi Icon (Top Right)
  bool offlineModeActive = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  if (!offlineModeActive) {
    int wifiX = tft.width() - 15;
    int wifiY = 15;
    int wifiRadius = 4;
    tft.fillCircle(wifiX, wifiY, wifiRadius, (WiFi.status() == WL_CONNECTED) ? HANIMAT_SUCCESS : HANIMAT_ERROR);
  }
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
  tft.fillScreen(HANIMAT_BG);
  
  // Use consistent Header
  drawPageHeader("SYSTEM UPDATE", HANIMAT_HEADER);

  // Message Lines - Centered nicely
  tft.setFont(&Poppins_Regular10pt7b);
  int16_t x1, y1; uint16_t w, h;
  int yPos = 100;

  tft.setTextColor(color);
  tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, yPos);
  tft.println(line1);
  yPos += 30;

  if (line2.length() > 0) {
    tft.setTextColor(HANIMAT_TEXT);
    tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, yPos);
    tft.println(line2);
    yPos += 30;
  }
   
  if (line3.length() > 0) {
    tft.setTextColor(HANIMAT_TEXT);
    tft.getTextBounds(line3, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, yPos);
    tft.println(line3);
  }
}

/**
 * @brief Sichert das aktuelle Guthaben im NVS, falls nötig.
 */
void saveCreditToNVS(bool force) {
    // Nur speichern, wenn sich der Wert geändert hat
    if (credit != lastCreditSaved || force) {
        preferences.begin("hanimat", false);
        preferences.putFloat("credit", credit);
        preferences.end();
        
        lastCreditSaved = credit;
        logMessage("NVS: Guthaben gesichert: " + String(credit, 2) + " EUR");
    }
}

/**
 * @brief Wird vom WiFiManager aufgerufen, wenn er in den AP-Modus (Setup-Portal) wechselt.
 */
void configModeCallback(WiFiManager *myWiFiManager) {
  logMessage("Kein WLAN gefunden. Setup-Portal gestartet.");
  
  tft.fillScreen(HANIMAT_BG);
  
  // Header: HANIMAT Setup
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(HANIMAT_HEADER);
  int16_t x1, y1; uint16_t w, h;
  String title = "HANIMAT Setup";
  tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 40);
  tft.println(title);

  // Info-Text
  tft.setFont(&Poppins_Regular10pt7b);
  
  tft.setTextColor(HANIMAT_ACCENT);
  String line1 = "WLAN nicht konfiguriert!";
  tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 75);
  tft.println(line1);

  tft.setTextColor(HANIMAT_TEXT);
  String line2 = "WLAN SSID: " + myWiFiManager->getConfigPortalSSID();
  tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 110);
  tft.println(line2);

  String line3 = "PW: Honig1234";
  tft.getTextBounds(line3, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 135);
  tft.println(line3);

  String line4 = "IP: 192.168.4.1";
  tft.getTextBounds(line4, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 160);
  tft.println(line4);
  
  // Piepton zur Signalisierung, dass er im Setup-Modus ist
  ledcWriteTone(0, 1500);
  delay(200);
  ledcWriteTone(0, 0);
}

// =================================================================
//                      INTERRUPT SERVICE ROUTINES
// =================================================================

/**
 * @brief ISR for the coin acceptor. Increments a pulse counter.
 */
void IRAM_ATTR coinAcceptorISR() {
  unsigned long now = millis();
  // Nur zählen, wenn der letzte Puls mindestens 20ms her ist
  // Das filtert "Prellen" bei FAST-Einstellung effektiv raus
  if (now - lastCoinPulseTime > 20) { 
    coinPulseCount++;
    lastCoinPulseTime = now;
  }
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
  expanderOutputStates[0] = 0x0000;

  logMessage("Setting relay output latches to OFF (pre-config)...");
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x02); // Start bei Register 0x02 (Output Port 0)
  Wire.write(0x00); // Port 0 auf LOW
  Wire.write(0x00); // Port 1 auf LOW (Chip inkrementiert automatisch zu Reg 0x03)
  Wire.endTransmission();

  logMessage("Configuring Relay Board pins as OUTPUT...");
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x06); // Start bei Register 0x06 (Configuration Port 0)
  Wire.write(0x00); // Port 0 auf OUTPUT
  Wire.write(0x00); // Port 1 auf OUTPUT (Chip inkrementiert automatisch zu Reg 0x07)
  Wire.endTransmission();

  logMessage("Relay board initialized (Fast Mode).");

  // --- Pin-Modus festlegen ---
  pinMode(WIFI_RESET_BUTTON, INPUT);  
  delay(100); // Dem Pin Zeit geben, sich zu stabilisieren

  // --- Floating Pin Check ---
  int lowCount = 0; 
  int sampleCount = 500; 
  for (int i = 0; i < sampleCount; i++) {
    if (digitalRead(WIFI_RESET_BUTTON) == LOW) {
      lowCount++;
    }
    delayMicroseconds(100);
  }

  logMessage("Reset-Pin Check: " + String(lowCount) + " von " + String(sampleCount) + " Samples waren LOW.");

  if (lowCount > 5) {
    resetPinIsFloating = true;
    logMessage("WARNUNG: Reset-Pin floating! Button wird SOFTWARESEITIG DEAKTIVIERT.");
  } else {
    resetPinIsFloating = false;
    logMessage("STATUS: Reset-Pin stabil erkannt.");
  }

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
  pinMode(WIFI_RESET_BUTTON, INPUT);
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

  char kBuf[16]; // Statischer Buffer für NVS Keys (verhindert Heap-Fragmentierung)

  // --- NEU: Einmalige Initialisierung, falls das System "leer" ist ---
  if (!preferences.isKey("initialized")) {
      logMessage("ERSTSTART: Erzeuge Standardwerte im NVS...");
      
      for (int i = 0; i < MAX_SLOTS; i++) {
          // Preise initialisieren
          snprintf(kBuf, sizeof(kBuf), "price%d", i);
          preferences.putFloat(kBuf, 5.0f + (i * 0.1f));
          
          // Verfügbarkeit initialisieren
          snprintf(kBuf, sizeof(kBuf), "avail%d", i);
          preferences.putBool(kBuf, true);
          
          // Sperre initialisieren
          snprintf(kBuf, sizeof(kBuf), "locked%d", i);
          preferences.putBool(kBuf, false);
      }
      
      preferences.putString("password", DEFAULT_PASSWORD);
      preferences.putBool("initialized", true);
      logMessage("Initialisierung abgeschlossen.");
  }

  logMessage("Loading settings from Preferences...");
  
  // Zeit- und Systemwerte laden
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
  
  sumupEnabled = preferences.getBool("suEnabled", false);
  sumupApiKey = preferences.getString("suApiKey", "");
  sumupMerchantId = preferences.getString("suMid", "");
  sumupReaderId = preferences.getString("suRid", "");
  sumupTimeout = preferences.getULong("suTimeout", 60000);

  sumUp = SumUpController(sumupApiKey, sumupMerchantId, sumupReaderId);
  pinMode(SUMUP_BUTTON_PIN, INPUT); 

  displaySlogan = preferences.getString("dispSlogan", "");
  displayFooter = preferences.getString("dispFooter", "www.hanimat.at");

  activeSlots = preferences.getInt("activeSlots", DEFAULT_MAX_SLOTS);
  if (activeSlots <= 0 || activeSlots > MAX_SLOTS) activeSlots = DEFAULT_MAX_SLOTS;

  // Fächer laden mit statischen Key-Namen und Fallback-Werten
  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(kBuf, sizeof(kBuf), "price%d", i);
    slotPrices[i]    = preferences.getFloat(kBuf, 5.0f + (i * 0.1f));
    
    snprintf(kBuf, sizeof(kBuf), "avail%d", i);
    slotAvailable[i] = preferences.getBool(kBuf, true);
    
    snprintf(kBuf, sizeof(kBuf), "locked%d", i);
    slotLocked[i]    = preferences.getBool(kBuf, false);
  }
  
  credit = preferences.getFloat("credit", 0.0f);
  savedPassword = preferences.getString("password", DEFAULT_PASSWORD);
  
  preferences.end();
  logMessage("Settings loaded.");

  // --- Initialize TFT Display ---
  tft.begin();
  tft.setRotation(1); // Landscape mode
  tft.fillScreen(HANIMAT_BG);
   
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(HANIMAT_HEADER);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds("HANIMAT", 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) - h);
  tft.println("HANIMAT");

  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(HANIMAT_TEXT);
  String subtitle = "startet...";
  tft.getTextBounds(subtitle, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) + 25);
  tft.println(subtitle);
  delay(2500);

  // --- Initialize WiFi ---
  bool offlineMode = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback(configModeCallback);

  if (offlineMode) {
    logMessage("Operating Mode: OFFLINE (GPIO " + String(OFFLINE_MODE_PIN) + " is LOW)");
    WiFi.softAP("HANIMAT-Offline", "Honig1234");
    logMessage("Offline AP started. SSID: HANIMAT-Offline, IP: " + WiFi.softAPIP().toString());
    tft.fillScreen(HANIMAT_BG);
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_ACCENT);
    tft.setCursor(10,40); tft.println("OFFLINE MODUS");
    tft.setTextColor(HANIMAT_TEXT);
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
      tft.fillScreen(HANIMAT_BG);
      tft.setFont(&Poppins_Black14pt7b);
      tft.setTextColor(HANIMAT_ERROR);
      String errorMsg = "WLAN Fehler!";
      tft.getTextBounds(errorMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 40);
      tft.println(errorMsg);

      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
        
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
      configTime(0, 0, "pool.ntp.org");
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); 
      logMessage("NTP Zeit-Synchronisierung gestartet...");
      tzset();
      tft.fillScreen(HANIMAT_BG);

      tft.setFont(&Poppins_Black14pt7b);
      tft.setTextColor(HANIMAT_SUCCESS);
      String connectedMsg = "WLAN Verbunden!";
      tft.getTextBounds(connectedMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 80);
      tft.println(connectedMsg);

      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
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

  // --- Start Hanimat Status Network ---
  sendHanimatStatusPing();

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


// --- Reset-Logik mit Keypad-Bestätigung ---
  if (!resetPinIsFloating && digitalRead(WIFI_RESET_BUTTON) == LOW) {
    
    // Sicherheitsprüfung: Muss 500ms STABIL gedrückt sein (Filtert Rauschen)
    unsigned long stabilityTimer = millis();
    bool isInterference = false;
    
    while (millis() - stabilityTimer < 2000) {
      if (digitalRead(WIFI_RESET_BUTTON) == HIGH) { 
        isInterference = true;
        break;
      }
      delay(5); 
    }

    // Nur wenn das Signal 500ms perfekt stabil war, öffnen wir den Dialog
    if (!isInterference) {
      logMessage("Reset-Knopf stabil gedrueckt. Warte auf # am Keypad...");
      
      // Anzeige auf dem TFT
      tft.fillScreen(HANIMAT_BG);
      drawPageHeader("SYSTEM RESET", HANIMAT_ACCENT);
      
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
      tft.setCursor(10, 110); tft.println("Bestaetigen mit #");
      tft.setCursor(10, 140); tft.println("Abbruch nach 5 Sek.");
      
      playKeyPressBeep();

      unsigned long waitStart = millis();
      bool confirmed = false;

      // 5 Sekunden Zeitfenster für die Raute-Taste
      while (millis() - waitStart < 5000) {
        char key = manualGetKeyState(); 
        if (key == '#') {
          confirmed = true;
          break;
        }
        delay(10);
      }

      if (confirmed) {
        logMessage("RESET BESTAETIGT!");
        tft.fillScreen(HANIMAT_BG);
        tft.setTextColor(HANIMAT_ERROR);
        tft.setFont(&Poppins_Black14pt7b);
        tft.setCursor(10, 80); tft.println("WERKSRESET...");
        playErrorSound();
        delay(2000);

        // Daten löschen
        preferences.begin("hanimat", false);
        preferences.clear();
        preferences.end();
        
        WiFiManager wm;
        wm.resetSettings();
        
        logMessage("Factory reset complete. Restarting...");
        ESP.restart();
      } else {
        logMessage("Reset abgebrochen.");
        resetDisplayToDefault(); 
      }
    }
  }

  // Guthaben verzögert speichern (Wear Leveling)
  if (credit != lastCreditSaved && (millis() - lastCreditChangeTime > NVS_SAVE_DELAY)) {
        saveCreditToNVS();
    }

  // --- Main state machine ---
  if (currentSystemState != CurrentSystemState::OTA_UPDATE && !isSumUpTransactionActive) {
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

  // --- SUMUP Button Check ---
  // Nur prüfen wenn SumUp aktiviert ist UND keine Transaktion läuft
  if (sumupEnabled && !isSumUpTransactionActive && digitalRead(SUMUP_BUTTON_PIN) == LOW) { 
    delay(100); // Entprellen
    if (digitalRead(SUMUP_BUTTON_PIN) == LOW) {
      handleSumUpPaymentInitiation();
    }
  }

  // --- SUMUP STATUS CHECK (läuft nur, wenn eine Zahlung aktiv ist) ---
  if (isSumUpTransactionActive) {
    unsigned long now = millis();
    lastUserInteractionTime = now; // Prevent display sleep during payment

    // --- NEU: Abbruch sofort prüfen (Responsive) ---
    char key = manualGetKeyState();
    if (key == '*') {
        logMessage("SumUp: Abbruch durch Benutzer (* Taste)");
        sumUp.cancel(); // API Call zum Terminal
        
        isSumUpTransactionActive = false; 
        currentSumUpTxId = "";
        
        displayErrorMessage("ZAHLUNG", "ABGEBROCHEN");
        // Reset state ensures we go back to IDLE
        return; // Loop iteration beenden
    }
    // -----------------------------------------------

    // 1. TIMEOUT PRÜFEN
    if (now - sumUpStartTime > sumupTimeout) {
        logMessage("SumUp: Zeit abgelaufen (Timeout)!");
        sumUp.cancel(); // Sendet Abbruch-Befehl an Terminal
        isSumUpTransactionActive = false;
        currentSumUpTxId = "";
        displayErrorMessage("ZEIT ABGELAUFEN", "Bitte erneut versuchen.");
    }

    // 2. STATUS PRÜFEN (Alle 2 Sekunden)
    else if (now - lastSumUpCheckTime > 2000) {
        lastSumUpCheckTime = now;
        
        // Status über die History-Suche abfragen
        String status = sumUp.checkStatus(currentSumUpTxId);
        
        // --- LOGGING ---
        // Zeigt Status im Log, wenn er NICHT "PENDING" ist (vermeidet Spam)
        if (status != "PENDING") {
            logMessage("SumUp Status Check: " + status); 
        }

if (status == "SUCCESSFUL") {
            logMessage(">>> SUMUP ZAHLUNG ERFOLGREICH! <<<");
            
            // Logik für korrekte Verrechnung:
            // Wir setzen das Guthaben auf den vollen Preis des Faches.
            // Grund: scheduleDispense() zieht den Preis später wieder ab.
            credit = slotPrices[selectedSlot]; 
            
            logMessage("SumUp: Zahlung abgeschlossen. Internes Guthaben angepasst auf: " + String(credit, 2) + " EUR");

            // Guthaben im Flash-Speicher sichern
            preferences.begin("hanimat", false);
            preferences.putFloat("credit", credit);
            preferences.end();

            // SumUp Status zurücksetzen
            isSumUpTransactionActive = false; // Stoppt das Polling
            currentSumUpTxId = "";
            currentSystemState = CurrentSystemState::IDLE; // Zurück in den Standardmodus
            
            // Warenausgabe starten
            logMessage("Starte Warenausgabe fuer Fach " + String(selectedSlot + 1));
            scheduleDispense(selectedSlot);
            
        }
        else if (status == "FAILED" || status == "CANCELLED") {
            logMessage("SumUp: Zahlung fehlgeschlagen oder abgebrochen (" + status + ").");
            isSumUpTransactionActive = false; // Polling beenden
            currentSumUpTxId = "";
            displayErrorMessage("ZAHLUNG", "abgebrochen");
        }
        // Bei "PENDING" passiert nichts, der Loop läuft einfach weiter
    }
  }

  // Auto-logout from web interface after timeout
  if (isAuthenticated && (millis() - lastActivityTimeWeb > WEB_TIMEOUT)) {
    isAuthenticated = false;
    logMessage("Web interface auto-logout due to inactivity.");
  }

  // Update display only when needed
  // Blockiert Updates während SumUp aktiv ist (da SumUp eigene Anzeige hat), außer Status Updates
  if (displayNeedsUpdate && currentSystemState != CurrentSystemState::OTA_UPDATE && !isSumUpTransactionActive) {
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
  // --- HANIMAT STATUS HEARTBEAT ---
  // Prüfen, ob die Zeit (statusInterval = 60 min) abgelaufen ist
  if (millis() - lastStatusPing > statusInterval) {
     sendHanimatStatusPing();
  }
  delay(10); // Small delay to prevent watchdog timer issues
}

// =================================================================
//                      CORE LOGIC IMPLEMENTATION
// =================================================================

/**
 * @brief Initiates the SumUp Payment process. This is now NON-BLOCKING.
 * It sets the state and returns to the main loop for polling.
 */
void handleSumUpPaymentInitiation() {
  // 1. Sicherheits-Checks
  if (!sumupEnabled) {
    displayErrorMessage("SUMUP", "Deaktiviert");
    return;
  }
  if (selectedSlot == -1) {
    displayErrorMessage("KEIN FACH", "Bitte wählen!");
    return;
  }

  // --- NEUER CHECK: Fachstatus prüfen ---
  if (slotLocked[selectedSlot]) {
    displayErrorMessage("FACH " + String(selectedSlot + 1), "gesperrt!");
    return;
  }
  if (!slotAvailable[selectedSlot]) {
    displayErrorMessage("FACH " + String(selectedSlot + 1), "ist leer!");
    return;
  }
  // --------------------------------------

  // 2. System-Status setzen (verhindert Timeout/Schlafmodus des Displays)
  currentSystemState = CurrentSystemState::SUMUP_PENDING;
  lastUserInteractionTime = millis();

  float price = slotPrices[selectedSlot];
  
  // LOG START
  logMessage("SumUp: Prozess gestartet fuer Fach " + String(selectedSlot + 1));

  // --- LOGIK FÜR MISCHZAHLUNG ---
  float remainingAmount = price - credit;
  
  logMessage("SumUp: Preis: " + String(price, 2) + " EUR, Guthaben: " + String(credit, 2) + " EUR -> Zu zahlen: " + String(remainingAmount, 2) + " EUR");

  if (remainingAmount <= 0) {
    // Falls das Guthaben bereits reicht, brauchen wir kein SumUp.
    // Wir triggern direkt den Kaufprozess.
    logMessage("SumUp: Abbruch, Guthaben deckt bereits den Preis.");
    scheduleDispense(selectedSlot);
    return;
  }

  // --- NEUER CHECK: MINDESTBETRAG 1.00 EUR (SumUp Limit) ---
  if (remainingAmount < 1.00) {
    logMessage("SumUp: Restbetrag " + String(remainingAmount, 2) + " EUR ist zu gering. Minimum 1.00 EUR.");
    displayErrorMessage("MIN. KARTE", "ab 1.00 EUR");
    return;
  }
  // ---------------------------------------------------------

  // 3. Anzeige auf TFT vorbereiten
  tft.fillScreen(HANIMAT_BG);
  drawPageHeader("KARTENZAHLUNG", HANIMAT_HEADER);
  
  tft.setFont(&Poppins_Regular10pt7b);
  int yPos = 80;
  int lineSpacing = 25;

  tft.setCursor(10, yPos);
  tft.setTextColor(HANIMAT_TEXT);
  tft.print("Preis:");
  tft.setCursor(150, yPos);
  tft.print(price, 2); tft.println(" EUR");
  
  if (credit > 0) {
    yPos += lineSpacing;
    tft.setCursor(10, yPos);
    tft.setTextColor(HANIMAT_SUCCESS);
    tft.print("- Guthaben:");
    tft.setCursor(150, yPos);
    tft.print(credit, 2); tft.println(" EUR");
  }

  // Divider Line for Total
  yPos += 15;
  tft.drawFastHLine(10, yPos, 220, HANIMAT_DIVIDER);
  yPos += 25;

  tft.setCursor(10, yPos);
  tft.setTextColor(HANIMAT_ACCENT);
  tft.print("Zu zahlen:");
  tft.setCursor(150, yPos);
  tft.print(remainingAmount, 2); tft.println(" EUR");
  
  // Status Bereich initialisieren
  tft.setTextColor(HANIMAT_TEXT);
  tft.setCursor(10, 200);
  tft.print("Bitte am Terminal folgen...");

  // 4. Zahlung bei SumUp-Server anmelden (NUR DEN RESTBETRAG)
  String trackingId;
  if (sumUp.startPayment(remainingAmount, trackingId)) {
    // 5. ASYNCHRONEN ZUSTAND SETZEN
    isSumUpTransactionActive = true;
    currentSumUpTxId = trackingId;
    pendingSumUpAmount = remainingAmount;
    sumUpStartTime = millis();
    lastSumUpCheckTime = 0; // Sofort prüfen

    logMessage("SumUp: Checkout API OK. Tracking-ID: " + trackingId + ". Warte im Loop auf Terminal...");
    
    // Wir verlassen die Funktion hier. Der Loop kümmert sich um den Rest.
  } else {
    logMessage("SumUp: Fehler beim Starten des Checkouts (API-Aufruf fehlgeschlagen).");
    displayErrorMessage("SUMUP FEHLER", "API nicht erreichbar");
    currentSystemState = CurrentSystemState::IDLE;
  }
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
  tft.fillScreen(HANIMAT_BG);
  char buffer[50]; // Buffer for formatting strings
  int16_t x1, y1; uint16_t w, h;

  // --- STATIC HEADER ---
  drawPageHeader("HANIMAT", HANIMAT_HEADER);

  // --- Credit Display (Top Left below header) ---
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(HANIMAT_SUCCESS);
  sprintf(buffer, "Guthaben: %.2f EUR", credit);
  tft.setCursor(10, 80);
  tft.println(buffer);

  // --- Dynamic Content Area ---
  int y_dynamic_start = 120;
  int line_spacing = 28;
  tft.setFont(&Poppins_Regular10pt7b);

  switch(currentSystemState) {
    case CurrentSystemState::ERROR_DISPLAY:
      // This case is handled by displayErrorMessage(), no action needed here.
      break;

    default: // Covers IDLE and USER_INTERACTION
      if (dispenseJob.active) {
        tft.setTextColor(HANIMAT_INFO);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Fach "); tft.print(dispenseJob.slot + 1);
        tft.setCursor(10, y_dynamic_start + line_spacing);
        tft.print("wird geoeffnet...");
      } else if (selectedSlot != -1) {
        tft.setTextColor(HANIMAT_TEXT);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Fach: "); tft.println(selectedSlot + 1);
        tft.setCursor(10, y_dynamic_start + line_spacing);
        if (slotLocked[selectedSlot]) {
          tft.setTextColor(HANIMAT_ERROR); tft.println("Gesperrt");
        } else if (!slotAvailable[selectedSlot]) {
          tft.setTextColor(HANIMAT_ERROR); tft.println("Leer");
        } else {
          tft.setTextColor(HANIMAT_TEXT);
          sprintf(buffer, "Preis: %.2f EUR", slotPrices[selectedSlot]);
          tft.println(buffer);
          tft.setCursor(10, y_dynamic_start + (line_spacing * 2));
          if (credit >= slotPrices[selectedSlot]) {
            tft.setTextColor(HANIMAT_SUCCESS); tft.println("# Kaufen");
          } else {
            tft.setTextColor(HANIMAT_ACCENT); tft.println("Guthaben?");
          }
        }
      } else if (keypadInputBuffer.length() > 0) {
        tft.setTextColor(HANIMAT_TEXT);
        tft.setCursor(10, y_dynamic_start);
        tft.print("Eingabe: "); tft.println(keypadInputBuffer);
      } else { // Idle screen
        tft.setTextColor(HANIMAT_TEXT);
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
  tft.setTextColor(HANIMAT_TEXT);
  tft.getTextBounds(displaySlogan, 0, 0, &x1, &y1, &w, &h);
  // Positionierung angepasst für größere Schrift
  tft.setCursor((tft.width() - w) / 2, tft.height() - h - 25); 
  tft.println(displaySlogan);
}

// --- Footer-Text zeichnen (kleinere Schrift) ---
tft.setFont(&Poppins_Regular7pt7b); // Kleinere Schrift für den Footer
tft.setTextColor(HANIMAT_HEADER);
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
        displayErrorMessage("FACH " + String(selectedSlot + 1), "gesperrt!");
      } else if (!slotAvailable[selectedSlot]) {
        displayErrorMessage("FACH " + String(selectedSlot + 1), "ist leer!");
      } else if (credit >= slotPrices[selectedSlot]) {
        logMessage("Purchase attempt: Slot " + String(selectedSlot + 1) + ", Credit: " + String(credit,2) + " EUR, Price: " + String(slotPrices[selectedSlot],2) + " EUR.");
        scheduleDispense(selectedSlot);
      } else {
        displayErrorMessage("GUTHABEN", "zu gering!");
      }
    } else {
      displayErrorMessage("KEIN FACH", "gewaehlt!");
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
      displayErrorMessage("FACH " + keypadInputBuffer, "ungueltig!");
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
    displayErrorMessage("RELAIS FEHLER", "Board offline");
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
  tft.fillScreen(HANIMAT_BG);
  drawPageHeader("PRODUKTAUSGABE", HANIMAT_INFO);
  
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(HANIMAT_TEXT);
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
      displayErrorMessage("RELAIS FEHLER", "Kauf abgebrochen");
      dispenseJob.active = false;
      digitalWrite(BILL_INHIBIT_PIN, LOW);
      resetDisplayToDefault();
      return;
    }
      
    // 1. Guthaben abziehen
    credit -= slotPrices[dispenseJob.slot];
    if (credit < 0) credit = 0;
    
    // 2. Slot als leer markieren
    slotAvailable[dispenseJob.slot] = false;
    logMessage("Purchase complete for slot " + String(dispenseJob.slot + 1) + ". New credit: " + String(credit, 2));

    // 3. Änderungen permanent im Flash speichern
    // Wir nutzen einen statischen Buffer für den Key-Namen (z.B. "avail5")
    char availKey[12];
    snprintf(availKey, sizeof(availKey), "avail%d", dispenseJob.slot);

    preferences.begin("hanimat", false);
    preferences.putBool(availKey, false); // NEU: Statischer Key statt dynamischem String
    preferences.end();
    
    saveCreditToNVS(true); // Sofortiges Speichern des Guthabens erzwingen (Wear-Leveling Logik)
    
    // Send notifications
    if (telegramNotifyOnSale) {
        String saleMessage = "🍯 VERKAUF: Fach #" + String(dispenseJob.slot + 1) + " wurde verkauft und ist jetzt leer.";
        sendTelegramMessage(saleMessage);
    }
    checkOverallStockLevel();

    // Play sound and update display
    playThankYouMelody();
    tft.fillScreen(HANIMAT_BG);
    drawPageHeader("VIELEN DANK", HANIMAT_SUCCESS);
    
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_TEXT);
    tft.setCursor(10, 100);
    tft.println("Bitte Produkt");
    tft.setCursor(10, 130);
    tft.print("entnehmen.");
      
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
  // Überprüfen, ob Pulse vorhanden sind und die Lücke zwischen den Münzen groß genug ist
  if (coinPulseCount > 0 && (millis() - lastCoinPulseTime > COIN_PROCESSING_DELAY)) {
    
    int pulsesToProcess;
    
    // 1. KRITISCHE SEKTION: Wert sichern und Zähler SOFORT nullen
    // Damit verpassen wir keine Pulse der nächsten Münze während der Logik unten.
    noInterrupts();
    pulsesToProcess = coinPulseCount;
    coinPulseCount = 0; 
    interrupts();

    logMessage("Münzprüfer: " + String(pulsesToProcess) + " Pulse erkannt.");

    // 2. MAPPING PRÜFEN (pulseValues Array)
    if (pulsesToProcess > 0 && pulsesToProcess < (sizeof(pulseValues) / sizeof(pulseValues[0]))) {
      int coinValueCents = pulseValues[pulsesToProcess];
      
      if (coinValueCents > 0) {
        // Guthaben im RAM erhöhen
        credit += (float)coinValueCents / 100.0;
        
        // Timer für die verzögerte Flash-Speicherung (Wear-Leveling)
        lastCreditChangeTime = millis(); 

        logMessage("Guthaben aktualisiert: +" + String((float)coinValueCents / 100.0, 2) + " EUR");
        
        // Display-Update anfordern und System-Status setzen
        displayNeedsUpdate = true;
        lastUserInteractionTime = millis();
        currentSystemState = CurrentSystemState::USER_INTERACTION;
        
        // Akustisches Feedback (Piep auf 40ms verkürzt, um Zeit zu sparen)
        ledcWriteTone(0, 1200); 
        delay(40); 
        ledcWriteTone(0, 0);
      } else {
        logMessage("Münz-Fehler: Wert für " + String(pulsesToProcess) + " Pulse ist 0.");
      }
    } else {
      // Hilft beim Debugging: Zeigt an, wie viele Pulse bei Fehlern wirklich ankamen
      logMessage("Coin Fehler: " + String(pulsesToProcess) + " Pulse passen zu keinem Mapping.");
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
        
        lastCreditChangeTime = millis();

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
    tft.fillScreen(HANIMAT_BG);

    // Draw Consistent Header
    drawPageHeader("HINWEIS", HANIMAT_ACCENT);

    int16_t x1, y1;
    uint16_t w, h;

    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_ERROR);

    // Line 1 (Bold/Large usually, here regular but colored red)
    tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 100);
    tft.println(line1);

    // Line 2 (if present)
    if (line2.length() > 0) {
        tft.setTextColor(HANIMAT_TEXT);
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

// ------------------------------------------
//         NEUE FUNKTIONEN: ONLINE UPDATE
// ------------------------------------------

void handleCheckOnlineUpdate() {
    if (!isAuthenticated) return;
    
    // Prüfen ob offline
    if (digitalRead(OFFLINE_MODE_PIN) == LOW) {
        server.send(500, "text/plain", "Fehler: Offline Modus aktiv.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure(); // SSL Zertifikat nicht prüfen
    HTTPClient http;
    http.setReuse(false); // Verbindung nach Anfrage schließen
    
    logMessage("Online-Update: Prüfe Version auf " + String(UPDATE_VERSION_URL));
    
    if (http.begin(client, UPDATE_VERSION_URL)) {
        int httpCode = http.GET();
        if (httpCode == 200) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            
            if (!error) {
                String remoteVersion = doc["version"].as<String>();
                if (remoteVersion.length() > 0) {
                    String msg = "Installiert: " + FIRMWARE_VERSION + " | Online: " + remoteVersion;
                    if (remoteVersion != FIRMWARE_VERSION) {
                        msg += " (NEU! Update verfügbar)";
                    } else {
                        msg += " (Aktuell)";
                    }
                    server.send(200, "text/plain", msg);
                } else {
                    server.send(500, "text/plain", "Fehler: Version im JSON leer.");
                }
            } else {
                server.send(500, "text/plain", "Fehler: JSON ungültig.");
            }
        } else {
            server.send(500, "text/plain", "HTTP Fehler: " + String(httpCode));
        }
        http.end();
    } else {
        server.send(500, "text/plain", "Verbindungsfehler zum Server.");
    }
}

void handleStartOnlineUpdate() {
    if (!isAuthenticated) return;
    
    // Prüfen ob offline
    if (digitalRead(OFFLINE_MODE_PIN) == LOW) {
        server.send(500, "text/plain", "Fehler: Offline Modus aktiv.");
        return;
    }

    // Antwort senden bevor der Prozess startet (da er blockiert)
    server.send(200, "text/plain", "Update gestartet! Bitte warten, Gerät startet neu...");
    delay(500); 
    
    logMessage("Online-Update: Starte Download von " + String(UPDATE_FIRMWARE_URL));
    
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setFont(&Poppins_Black14pt7b);
    tft.setCursor(10, 100);
    tft.println("ONLINE UPDATE");
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(10, 130);
    tft.println("Bitte warten...");
    tft.setCursor(10, 160);
    tft.println("Nicht ausschalten!");
    
    WiFiClientSecure client;
    client.setInsecure();
    
    // Globale Flag setzen um Loop zu pausieren
    otaUpdateInProgress = true; 
    
    // Update durchführen (Blockiert bis fertig oder Fehler)
    t_httpUpdate_return ret = httpUpdate.update(client, UPDATE_FIRMWARE_URL);
    
    switch (ret) {
      case HTTP_UPDATE_FAILED:
        logMessage("Online-Update FEHLER: " + httpUpdate.getLastErrorString());
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 190);
        tft.println("Fehler!");
        delay(5000);
        otaUpdateInProgress = false; 
        resetDisplayToDefault();
        break;
        
      case HTTP_UPDATE_NO_UPDATES:
        logMessage("Online-Update: Keine Daten.");
        otaUpdateInProgress = false;
        resetDisplayToDefault();
        break;
        
      case HTTP_UPDATE_OK:
        logMessage("Online-Update: OK. Neustart...");
        // ESP startet automatisch neu
        break;
    }
}

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
  
  // Neue Routen für Online Update
  server.on("/check-online-update", HTTP_GET, handleCheckOnlineUpdate);
  server.on("/start-online-update", HTTP_POST, handleStartOnlineUpdate);
    
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

server.on("/savesumup", HTTP_POST, []() {
  if (!isAuthenticated) return;
  sumupApiKey = server.arg("apiKey");
  sumupMerchantId = server.arg("merchantId");
  sumupReaderId = server.arg("readerId");
  sumupEnabled = server.hasArg("enabled");
  
  // Timeout from seconds to milliseconds
  int timeoutSec = server.arg("timeout").toInt();
  if (timeoutSec < 10) timeoutSec = 10;
  sumupTimeout = (unsigned long)timeoutSec * 1000;

  preferences.begin("hanimat", false);
  preferences.putString("suApiKey", sumupApiKey);
  preferences.putString("suMid", sumupMerchantId);
  preferences.putString("suRid", sumupReaderId);
  preferences.putBool("suEnabled", sumupEnabled);
  preferences.putULong("suTimeout", sumupTimeout);
  preferences.end();
  
  sumUp = SumUpController(sumupApiKey, sumupMerchantId, sumupReaderId);
  logMessage("Web: SumUp Einstellungen gespeichert. Timeout: " + String(timeoutSec) + "s, Enabled: " + String(sumupEnabled));
  server.sendHeader("Location", "/#sumup-config", true);
  server.send(302);
});

server.on("/pairsumup", HTTP_POST, []() {
  if (!isAuthenticated) return;
  String code = server.arg("code");
  logMessage("Web: SumUp Pairing gestartet mit Code " + code);
  String newId = sumUp.pairReader(code);
  if (newId != "") {
    sumupReaderId = newId;
    preferences.begin("hanimat", false);
    preferences.putString("suRid", newId);
    preferences.end();
    logMessage("Web: SumUp Pairing erfolgreich. Reader ID: " + newId);
    server.send(200, "text/html", "Erfolg! ID: " + newId + " <meta http-equiv='refresh' content='2;url=/' />");
  } else {
    logMessage("Web: SumUp Pairing fehlgeschlagen.");
    server.send(200, "text/html", "Fehler! Code prüfen. <meta http-equiv='refresh' content='2;url=/' />");
  }
});

server.on("/disconnectsumup", HTTP_POST, []() {
  if (!isAuthenticated) return;
  
  logMessage("Web: Trenne SumUp Terminal...");
  
  // 1. Versuche API Unpair
  bool apiSuccess = sumUp.unpairReader();
  
  // 2. Lokal speichern (leeren String), egal ob API erfolgreich war oder nicht
  // (damit man bei API-Fehlern nicht "feststeckt")
  sumupReaderId = "";
  preferences.begin("hanimat", false);
  preferences.putString("suRid", "");
  preferences.end();
  
  // Controller ID sicherheitshalber nochmal nullen (macht die Funktion oben zwar auch, aber sicher ist sicher)
  sumUp.setReaderId("");

  if(apiSuccess) {
      logMessage("Web: SumUp Reader erfolgreich entkoppelt.");
      server.send(200, "text/html", "Erfolg! Terminal entkoppelt. <meta http-equiv='refresh' content='2;url=/' />");
  } else {
      logMessage("Web: SumUp Reader lokal entfernt (API Fehler oder offline).");
      server.send(200, "text/html", "Lokal entfernt (API Warnung). <meta http-equiv='refresh' content='2;url=/' />");
  }
});
server.begin();
  logMessage("Web server started.");
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
<!DOCTYPE html><html lang='de'><head><title>Login | HANIMAT</title>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;800&display=swap');
:root { --brand: #FF9F1C; --bg: #0F1115; --panel: #181A20; --text: #FFFFFF; --border: #2A2D35; }
body { height: 100vh; display: flex; flex-direction: column; align-items: center; justify-content: center; font-family: 'Inter', sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; box-sizing: border-box; }
.login-wrap { width: 100%; max-width: 340px; text-align: center; }
.logo-area { margin-bottom: 2.5rem; }
.brand-name { font-size: 2.8rem; font-weight: 800; color: var(--text); letter-spacing: -1px; margin: 0; line-height: 1; }
.brand-name span { color: var(--brand); }
.brand-sub { font-size: 0.9rem; color: #666; font-weight: 500; margin-top: 5px; letter-spacing: 0.5px; }
.card { background: var(--panel); padding: 2.5rem; border-radius: 20px; border: 1px solid var(--border); box-shadow: 0 20px 40px rgba(0,0,0,0.4); }
input { width: 100%; padding: 1.1rem; margin-bottom: 1.5rem; background: #08090B; border: 1px solid var(--border); border-radius: 12px; color: white; font-size: 1.1rem; text-align: center; outline: none; transition: 0.2s; box-sizing: border-box; }
input:focus { border-color: var(--brand); box-shadow: 0 0 0 3px rgba(255, 159, 28, 0.15); }
button { width: 100%; padding: 1.1rem; background: var(--brand); color: #000; border: none; border-radius: 12px; font-size: 1.1rem; font-weight: 700; cursor: pointer; transition: 0.2s; }
button:hover { transform: translateY(-2px); box-shadow: 0 10px 20px rgba(255, 159, 28, 0.2); }
.copy { margin-top: 2rem; font-size: 0.75rem; color: #444; }
.copy a { color: #555; text-decoration: none; }
</style></head><body>
<div class='login-wrap'>
  <div class='logo-area'>
    <h1 class='brand-name'>HANI<span>MAT</span></h1>
    <div class='brand-sub'>Thomas Schöpf</div>
  </div>
  <div class='card'>
    <form action='/login' method='post'>
      <input type='password' name='password' placeholder='Passwort' required autofocus>
      <button type='submit'>Anmelden</button>
    </form>
  </div>
  <div class='copy'>&copy; 2026 Hanimat Systems<br><a href='https://www.hanimat.at'>www.hanimat.at</a></div>
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
<!DOCTYPE html><html lang='de'><head><title>Hanimat Control</title>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap');

/* --- DESIGN SYSTEM --- */
:root { 
    --brand: #FF9F1C; 
    --brand-glow: rgba(255, 159, 28, 0.2);
    --bg-body: #0F1115;
    --bg-sidebar: #15171C;
    --bg-card: #1C1F26;
    --bg-input: #121418;
    --border: #2D3139;
    --text-main: #FFFFFF;
    --text-sec: #9CA3AF;
    --success: #2ECC71;
    --danger: #E74C3C;
    --locked: #F39C12;
}

* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
body { margin: 0; padding: 0; font-family: 'Inter', sans-serif; background: var(--bg-body); color: var(--text-main); display: flex; height: 100vh; overflow: hidden; }

/* --- NAVIGATION --- */
.sidebar { width: 260px; background: var(--bg-sidebar); border-right: 1px solid var(--border); display: flex; flex-direction: column; flex-shrink: 0; transition: transform 0.3s ease; z-index: 100; }
.brand-header { padding: 2rem 1.5rem; }
.logo { font-size: 1.8rem; font-weight: 800; color: white; letter-spacing: -0.5px; margin: 0; }
.logo span { color: var(--brand); }
.logo-sub { font-size: 0.8rem; color: #666; margin-top: 4px; font-weight: 500; }

.nav-list { list-style: none; padding: 0 1rem; margin: 0; overflow-y: auto; flex: 1; }
.nav-btn { display: flex; align-items: center; width: 100%; padding: 0.9rem 1rem; margin-bottom: 0.4rem; background: transparent; border: none; border-radius: 10px; color: var(--text-sec); font-family: inherit; font-size: 0.95rem; font-weight: 500; cursor: pointer; text-align: left; transition: 0.2s; }
.nav-btn:hover { background: rgba(255,255,255,0.03); color: white; }
.nav-btn.active { background: var(--brand); color: #000; font-weight: 600; box-shadow: 0 4px 12px var(--brand-glow); }
.nav-icon { width: 20px; margin-right: 12px; text-align: center; }

.footer-info { padding: 1.5rem; font-size: 0.75rem; color: #444; text-align: center; border-top: 1px solid var(--border); }

/* --- MAIN CONTENT AREA --- */
.main { flex: 1; overflow-y: auto; padding: 2rem; position: relative; }
.top-bar { display: flex; justify-content: space-between; align-items: center; margin-bottom: 2rem; }
h1 { font-size: 1.8rem; font-weight: 700; margin: 0; color: white; }
h2 { font-size: 1.1rem; color: var(--text-sec); font-weight: 600; margin: 2rem 0 1rem 0; text-transform: uppercase; letter-spacing: 0.5px; }

/* --- ACTION GRIDS (DASHBOARD) --- */
.stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; margin-bottom: 2rem; }
.stat-box { background: var(--bg-card); padding: 1.5rem; border-radius: 16px; border: 1px solid var(--border); display: flex; flex-direction: column; }
.stat-val { font-size: 2rem; font-weight: 700; color: white; margin-bottom: 0.3rem; }
.stat-lbl { font-size: 0.85rem; color: var(--text-sec); }
.stat-highlight { color: var(--brand); }

/* --- SLOT TILES (THE NEW DESIGN) --- */
.slots-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 1rem; }
.slot-card { background: var(--bg-card); border-radius: 16px; padding: 1.2rem; border: 1px solid var(--border); position: relative; transition: transform 0.2s; display: flex; flex-direction: column; }
.slot-card:hover { border-color: #444; transform: translateY(-2px); }

.slot-header { display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 1rem; }
.slot-title { font-weight: 700; font-size: 1.1rem; color: white; }
.slot-price { font-size: 0.9rem; color: var(--text-sec); margin-top: 2px; }

.badge { font-size: 0.7rem; font-weight: 700; padding: 4px 8px; border-radius: 6px; text-transform: uppercase; }
.b-ok { background: rgba(46, 204, 113, 0.15); color: var(--success); }
.b-empty { background: rgba(255, 255, 255, 0.1); color: #777; }
.b-lock { background: rgba(231, 76, 60, 0.15); color: var(--danger); }

.slot-controls { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-top: auto; }
.icon-btn { background: #252830; border: none; border-radius: 8px; padding: 10px 0; color: #ccc; cursor: pointer; transition: 0.2s; font-size: 1.1rem; display: flex; align-items: center; justify-content: center; }
.icon-btn:hover { background: #333; color: white; }
.btn-refill:hover { color: var(--brand); background: rgba(255, 159, 28, 0.1); }
.btn-test:hover { color: #3498db; background: rgba(52, 152, 219, 0.1); }

/* --- FORMS --- */
.input-group { margin-bottom: 1.2rem; }
.input-group label { display: block; color: var(--text-sec); font-size: 0.85rem; margin-bottom: 0.5rem; }
input, select { width: 100%; background: var(--bg-input); border: 1px solid var(--border); color: white; padding: 0.9rem; border-radius: 10px; font-size: 1rem; outline: none; transition: 0.2s; box-sizing: border-box; }
input:focus { border-color: var(--brand); }

.btn-main { width: 100%; background: var(--brand); color: #000; border: none; padding: 0.9rem; border-radius: 10px; font-weight: 700; cursor: pointer; margin-top: 0.5rem; }
.btn-main:hover { opacity: 0.9; }
.btn-sec { width: 100%; background: transparent; border: 1px solid var(--border); color: var(--text-sec); padding: 0.9rem; border-radius: 10px; cursor: pointer; font-weight: 600; }
.btn-sec:hover { border-color: #666; color: white; }

.quick-actions { display: flex; gap: 1rem; background: var(--bg-card); padding: 1.5rem; border-radius: 16px; border: 1px solid var(--border); flex-wrap: wrap; margin-bottom: 2rem; align-items: flex-end; }

/* --- MOBILE OPTIMIZATION --- */
.mobile-header { display: none; }
.overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.8); z-index: 90; backdrop-filter: blur(4px); }

@media (max-width: 768px) {
  body { flex-direction: column; overflow: auto; }
  .sidebar { position: fixed; height: 100%; left: -280px; top: 0; }
  .sidebar.open { transform: translateX(280px); }
  .overlay.open { display: block; }
  
  .mobile-header { display: flex; align-items: center; justify-content: space-between; padding: 0 1.5rem; height: 70px; background: rgba(15,17,21,0.95); border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 80; backdrop-filter: blur(10px); }
  .menu-toggle { font-size: 1.5rem; background: none; border: none; color: white; padding: 5px; }
  
  .main { padding: 1.5rem; height: auto; overflow: visible; }
  .top-bar { display: none; } /* Hide desktop title on mobile */
  
  .quick-actions { flex-direction: column; align-items: stretch; }
  .slots-grid { grid-template-columns: 1fr; } /* 1 Column on Mobile */
  .slot-card { display: grid; grid-template-columns: 1fr auto; align-items: center; gap: 1rem; }
  .slot-controls { display: flex; margin-top: 0; grid-column: 1 / -1; margin-top: 1rem; }
  .slot-controls button { flex: 1; }
  .slot-header { margin-bottom: 0; flex-direction: column; }
}

/* Utilities */
.check-row { display: flex; align-items: center; gap: 10px; padding: 10px; background: rgba(255,255,255,0.02); border-radius: 8px; margin-bottom: 0.5rem; cursor: pointer; }
.check-row input { width: auto; }
#log-output{background:#000;color:#0f0;font-family:monospace;padding:1rem;height:350px;overflow-y:auto;border-radius:8px;font-size:0.8rem;border:1px solid #333;white-space:pre-wrap;word-wrap:break-word;}
</style></head><body>

<!-- Mobile UI -->
<div class='overlay' onclick='toggleMenu()'></div>
<div class='mobile-header'>
  <div class='logo' style='font-size:1.4rem'>HANI<span>MAT</span></div>
  <button class='menu-toggle' onclick='toggleMenu()'>&#9776;</button>
</div>

<!-- Sidebar -->
<nav class='sidebar'>
  <div class='brand-header'>
    <div class='logo'>HANI<span>MAT</span></div>
    <div class='logo-sub'>Thomas Schöpf</div>
  </div>
  <ul class='nav-list'>
    <li><button class='nav-btn active' onclick='go("dashboard")'><span class='nav-icon'>&#128202;</span> Dashboard</button></li>
    <li><button class='nav-btn' onclick='go("slots-config")'><span class='nav-icon'>&#9881;</span> Slot Config</button></li>
    <li><button class='nav-btn' onclick='go("display-config")'><span class='nav-icon'>&#128187;</span> Anzeige</button></li>
    <li><button class='nav-btn' onclick='go("timing-config")'><span class='nav-icon'>&#9201;</span> Zeitsteuerung</button></li>
    <li><button class='nav-btn' onclick='go("telegram-config")'><span class='nav-icon'>&#9993;</span> Telegram</button></li>
    <li><button class='nav-btn' onclick='go("sumup-config")'><span class='nav-icon'>&#128179;</span> SumUp</button></li>
    <li><button class='nav-btn' onclick='go("network-config")'><span class='nav-icon'>&#128423;</span> Netzwerk</button></li>
    <li><button class='nav-btn' onclick='go("password-config")'><span class='nav-icon'>&#128274;</span> Sicherheit</button></li>
    <li><button class='nav-btn' onclick='go("logs")'><span class='nav-icon'>&#128466;</span> Logs</button></li>
    <li><button class='nav-btn' onclick='go("ota-update-section")'><span class='nav-icon'>&#128229;</span> Update</button></li>
  </ul>
<div class='footer-info'>
    FW )HTML"; html += FIRMWARE_VERSION; html += R"HTML(<br>
    Hanimat<br>
    <a href='https://www.hanimat.at' target='_blank' style='color:#666; text-decoration:none;'>www.hanimat.at</a>
  </div>
</nav>

<!-- Content -->
<div class='main'>

  <!-- DASHBOARD -->
  <section id='dashboard' class='page'>
    <div class='top-bar'><h1>Dashboard</h1></div>

    <div class='stats-grid'>
      <div class='stat-box'>
        <div class='stat-val stat-highlight'>)HTML"; html += String(credit, 2) + R"HTML( &euro;</div>
        <div class='stat-lbl'>Aktuelles Guthaben</div>
      </div>
      <div class='stat-box'>
        <div class='stat-val'>)HTML"; html += String(countAvailableSlots()) + "/" + String(activeSlots) + R"HTML(</div>
        <div class='stat-lbl'>Verfügbare Fächer</div>
      </div>
      <div class='stat-box'>
        <div class='stat-val'>)HTML"; html += String(millis()/60000) + R"HTML( min</div>
        <div class='stat-lbl'>System Laufzeit</div>
      </div>
    </div>

    <!-- Quick Actions Bar -->
    <div class='quick-actions'>
      <form action='/addcredit' method='post' style='flex:1; min-width:200px;'>
        <label style='font-size:0.8rem; color:#888; margin-bottom:4px; display:block;'>Guthaben simulieren</label>
        <div style='display:flex; gap:10px;'>
          <input type='number' step='0.01' name='amount' placeholder='Betrag' required style='margin:0;'>
          <button type='submit' class='btn-main' style='width:auto; margin:0;'>Go</button>
        </div>
      </form>
      <form action='/resetcredit' method='post'>
        <button type='submit' class='btn-sec' style='border-color:var(--danger); color:var(--danger);'>Reset &euro;</button>
      </form>
      <div style='width:1px; height:40px; background:var(--border); margin:0 10px; display:none;'></div> <!-- Seperator optional -->
      <form action='/refillall' method='post' style='flex:1;'>
         <button type='submit' class='btn-sec'>Alle Auffüllen</button>
      </form>
      <form action='/triggerallrelays' method='post' style='flex:1;'>
         <button type='submit' class='btn-sec'>Relais Test</button>
      </form>
    </div>

    <h2>Fach Status & Steuerung</h2>
    <div class='slots-grid'>
)HTML";
  for (int i = 0; i < activeSlots; i++) {
    String badgeClass = "b-ok";
    String statusText = "Bereit";
    String lockIcon = "&#128275;"; // open lock
    
    if (slotLocked[i]) { 
        statusText = "Gesperrt"; 
        badgeClass = "b-lock"; 
        lockIcon = "&#128274;"; // closed lock
    } else if (!slotAvailable[i]) { 
        statusText = "Leer"; 
        badgeClass = "b-empty"; 
    }

    html += "<div class='slot-card'>";
    // Header Part
    html += "<div class='slot-header'>";
    html += "<div><div class='slot-title'>Fach #" + String(i+1) + "</div><div class='slot-price'>" + String(slotPrices[i], 2) + " &euro;</div></div>";
    html += "<span class='badge " + badgeClass + "'>" + statusText + "</span>";
    html += "</div>"; // end header

    // Controls Part
    html += "<div class='slot-controls'>";
    
    // Lock Button
    html += "<form action='/toggleslotlock' method='post' style='display:contents;'><input type='hidden' name='slot' value='" + String(i) + "'>";
    html += "<button type='submit' class='icon-btn' title='Sperren/Entsperren'>" + lockIcon + "</button></form>";

    // Test Button
    html += "<form action='/triggerrelay' method='post' style='display:contents;'><input type='hidden' name='slot' value='" + String(i) + "'>";
    html += "<button type='submit' class='icon-btn btn-test' title='Relais Test'>&#9889;</button></form>";

    // Refill Button
    html += "<form action='/refill' method='post' style='display:contents;'><input type='hidden' name='slot' value='" + String(i) + "'>";
    html += "<button type='submit' class='icon-btn btn-refill' title='Auffüllen'>&#128260;</button></form>";
    
    html += "</div></div>"; // end card
  }
  html += R"HTML(
    </div>
  </section>

  <!-- CONFIG SLOTS -->
  <section id='slots-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Slot Konfiguration</h1></div>
    <div class='stat-box' style='max-width:500px; margin-bottom:2rem;'>
       <form action='/updateslots' method='post'>
         <div class='input-group'>
           <label>Anzahl aktiver Fächer (Max )HTML"; html += String(MAX_SLOTS) + R"HTML()</label>
           <input type='number' name='maxSlots' value=')HTML" + String(activeSlots) + R"HTML(' min='1' max=')HTML" + String(MAX_SLOTS) + R"HTML(' required>
         </div>
         <button type='submit' class='btn-main'>Anzahl Speichern</button>
       </form>
    </div>
    
    <h2>Preise Einstellen</h2>
    <div class='slots-grid'>
)HTML";
  for (int i = 0; i < activeSlots; i++) {
     html += "<div class='slot-card'><form action='/updateprice' method='post'>";
     html += "<div class='slot-title' style='margin-bottom:10px;'>Fach #" + String(i+1) + "</div>";
     html += "<input type='hidden' name='slot' value='" + String(i) + "'>";
     html += "<div class='input-group'><input type='number' step='0.01' name='price' value='" + String(slotPrices[i],2) + "'></div>";
     html += "<button type='submit' class='btn-sec'>Speichern</button>";
     html += "</form></div>";
  }
  html += R"HTML(
    </div>
  </section>

  <!-- CONFIG DISPLAY -->
  <section id='display-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Display Texte</h1></div>
    <div class='stat-box' style='max-width:600px;'>
      <form action='/savedisplayconfig' method='post'>
        <div class='input-group'>
          <label>Slogan (Zeile 1)</label>
          <input type='text' name='slogan' value=')HTML" + displaySlogan + R"HTML(' maxlength=')HTML" + String(SLOGAN_MAX_LENGTH) + R"HTML('>
        </div>
        <div class='input-group'>
          <label>Footer (Zeile 2)</label>
          <input type='text' name='footer' value=')HTML" + displayFooter + R"HTML(' maxlength='30'>
        </div>
        <button type='submit' class='btn-main'>Texte übernehmen</button>
      </form>
    </div>
  </section>

  <!-- CONFIG TIMING -->
  <section id='timing-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Zeitsteuerung (ms)</h1></div>
    <div class='stat-box'>
      <form action='/savetimingconfig' method='post'>
        <div style='display:grid; grid-template-columns: repeat(auto-fill, minmax(250px, 1fr)); gap:1.5rem;'>
          <div class='input-group'><label>Coin Delay</label><input type='number' name='coin_delay' value=')HTML" + String(COIN_PROCESSING_DELAY) + R"HTML('></div>
          <div class='input-group'><label>Bill Debounce</label><input type='number' name='bill_isr_debounce' value=')HTML" + String(BILL_ISR_DEBOUNCE_MS) + R"HTML('></div>
          <div class='input-group'><label>Bill Timeout</label><input type='number' name='bill_group_timeout' value=')HTML" + String(BILL_GROUP_PROCESSING_TIMEOUT_MS) + R"HTML('></div>
          <div class='input-group'><label>Öffnungszeit</label><input type='number' name='disp_time' value=')HTML" + String(DISPENSE_RELAY_ON_TIME) + R"HTML('></div>
          <div class='input-group'><label>Keypad Timeout</label><input type='number' name='keypad_time' value=')HTML" + String(KEYPAD_INPUT_TIMEOUT) + R"HTML('></div>
          <div class='input-group'><label>Slot Anzeigezeit</label><input type='number' name='slot_sel_time' value=')HTML" + String(SLOT_SELECTION_TIMEOUT) + R"HTML('></div>
          <div class='input-group'><label>Display Timeout</label><input type='number' name='disp_timeout' value=')HTML" + String(DISPLAY_TIMEOUT) + R"HTML('></div>
        </div>
        <button type='submit' class='btn-main' style='margin-top:2rem;'>Zeiten Speichern</button>
      </form>
    </div>
  </section>

  <!-- CONFIG TELEGRAM -->
  <section id='telegram-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Telegram Bot</h1></div>
    <div class='stat-box'>
      <form action='/savetelegramconfig' method='post'>
        <div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:1rem;'>
          <label class='check-row' style='margin-bottom:0; flex-shrink:0;'>
            <input type='checkbox' name='tg_enabled' )HTML" + String(telegramEnabled ? "checked" : "") + R"HTML(>
            <b>Integration Aktivieren</b>
          </label>
          <a href='https://hanimat.at/telegram.html' target='_blank' style='color:var(--brand); text-decoration:none; font-size:0.8rem; background:rgba(255,159,28,0.1); padding:5px 12px; border-radius:8px; border:1px solid rgba(255,159,28,0.3);'>
            &#128214; Setup-Anleitung
          </a>
        </div>
        
        <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap:1.5rem; margin-top:1rem;'>
           <div class='input-group'><label>Bot Token</label><input type='password' name='tg_token' value=')HTML" + telegramBotToken + R"HTML('></div>
           <div class='input-group'><label>Chat ID</label><input type='text' name='tg_chat_id' value=')HTML" + telegramChatId + R"HTML('></div>
        </div>

        <h3 style='color:white; font-size:1rem; margin-top:1.5rem;'>Benachrichtigungen</h3>
        <label class='check-row'><input type='checkbox' name='notify_sale' )HTML" + String(telegramNotifyOnSale ? "checked" : "") + R"HTML(> Bei Verkauf</label>
        <label class='check-row'><input type='checkbox' name='notify_almost_empty' )HTML" + String(telegramNotifyAlmostEmpty ? "checked" : "") + R"HTML(> Wenn fast leer</label>
        <div class='input-group' style='margin-left:30px;'><label>Schwelle (Menge)</label><input type='number' name='almost_empty_threshold' value=')HTML" + String(almostEmptyThreshold) + R"HTML('></div>
        <label class='check-row'><input type='checkbox' name='notify_empty' )HTML" + String(telegramNotifyEmpty ? "checked" : "") + R"HTML(> Wenn komplett leer</label>

        <button type='submit' class='btn-main' style='margin-top:1.5rem;'>Speichern</button>
      </form>
      
      <form action='/sendtesttelegram' method='post' style='margin-top:2rem; border-top:1px solid var(--border); padding-top:1rem;'>
         <button type='submit' class='btn-sec'>Test Nachricht Senden</button>
      </form>
    </div>
  </section>

  <!-- CONFIG NETWORK -->
  )HTML";
  preferences.begin("hanimat", false);
  String staticIP_val = preferences.getString("static_ip", "");
  String gateway_val = preferences.getString("gateway", "");
  String subnet_val = preferences.getString("subnet", "");
  String dns1_val = preferences.getString("dns1", "8.8.8.8");
  preferences.end();
  html += R"HTML(
  <section id='network-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Netzwerk</h1></div>
    <div class='stat-box'>
       <div style='background:rgba(46, 204, 113, 0.1); border:1px solid var(--success); color:var(--success); padding:1rem; border-radius:10px; margin-bottom:2rem;'>
         IP: )HTML" + WiFi.localIP().toString() + R"HTML( | Mode: )HTML" + (staticIP_val.length() > 0 ? "STATIC" : "DHCP") + R"HTML(
       </div>
       <form action='/setstaticip' method='post'>
         <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap:1.5rem;'>
           <div class='input-group'><label>Static IP (leer = DHCP)</label><input type='text' name='static_ip' value=')HTML" + staticIP_val + R"HTML('></div>
           <div class='input-group'><label>Gateway</label><input type='text' name='gateway' value=')HTML" + gateway_val + R"HTML('></div>
           <div class='input-group'><label>Subnet</label><input type='text' name='subnet' value=')HTML" + subnet_val + R"HTML('></div>
           <div class='input-group'><label>DNS</label><input type='text' name='dns1' value=')HTML" + dns1_val + R"HTML('></div>
         </div>
         <button type='submit' class='btn-main' style='margin-top:1.5rem;'>Speichern & Neustart</button>
       </form>
    </div>
  </section>

  <!-- CONFIG PASSWORD -->
  <section id='password-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Sicherheit</h1></div>
    <div class='stat-box' style='max-width:500px;'>
      <form action='/changepassword' method='post'>
         <div class='input-group'>
            <label>Neues Admin Passwort</label>
            <input type='password' name='newPassword' required minlength='4' placeholder='****'>
         </div>
         <button type='submit' class='btn-main'>Passwort ändern</button>
      </form>
    </div>
  </section>

  <!-- LOGS -->
  <section id='logs' class='page' style='display:none;'>
    <div class='top-bar'><h1>System Logs</h1></div>
    <div class='stat-box'>
       <div id='log-output'>Lade Daten...</div>
    </div>
  </section>

  <!-- UPDATE -->
  <section id='ota-update-section' class='page' style='display:none;'>
    <div class='top-bar'><h1>Firmware Update</h1></div>
    <div class='stat-box'>
       <h2>Online Update</h2>
       <div style='background:rgba(255,255,255,0.05); padding:1.5rem; border-radius:10px; margin-bottom:2rem;'>
         <div id='online-update-status' style='margin-bottom:1rem; color:var(--brand);'>Status: Warte auf Prüfung...</div>
         <div style='display:flex; gap:1rem;'>
            <button onclick='checkOnlineUpdate()' class='btn-sec' style='width:auto;'>Version Prüfen</button>
            <form action='/start-online-update' method='post' id='update-form' style='display:none;'>
                <button type='submit' class='btn-main' style='width:auto;'>Update Starten</button>
            </form>
         </div>
       </div>

       <h2>Datei Upload</h2>
       <form method='POST' action='/ota-upload' enctype='multipart/form-data'>
          <div class='input-group'>
             <input type='file' name='update' accept='.bin' required style='padding:1rem;'>
          </div>
          <button type='submit' class='btn-main'>Upload & Flash</button>
       </form>
    </div>
  </section>

  <!-- CONFIG SUMUP -->
  <section id='sumup-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>SumUp Terminal</h1></div>
    <div class='stat-box'>
       <form action='/savesumup' method='post'>
         <label class='check-row'>
            <input type='checkbox' name='enabled' )HTML" + String(sumupEnabled ? "checked" : "") + R"HTML(>
            <b>SumUp Aktiviert</b>
         </label>
         <div class='input-group'><label>API Key</label><input type='password' name='apiKey' value=')HTML" + sumupApiKey + R"HTML('></div>
         <div class='input-group'><label>Merchant ID</label><input type='text' name='merchantId' value=')HTML" + sumupMerchantId + R"HTML('></div>
         <div class='input-group'><label>Reader ID</label><input type='text' name='readerId' value=')HTML" + sumupReaderId + R"HTML('></div>
         <div class='input-group'><label>Timeout (s)</label><input type='number' name='timeout' value=')HTML" + String(sumupTimeout/1000) + R"HTML(' min='10'></div>
         <button type='submit' class='btn-main'>Speichern</button>
       </form>

       <h2 style='margin-top:2rem;'>Pairing</h2>
       <div style='background:rgba(255,255,255,0.05); padding:1.5rem; border-radius:10px;'>
          <form action='/pairsumup' method='post' style='margin-bottom:1rem;'>
            <div style='display:flex; gap:10px;'>
               <input type='text' name='code' placeholder='Code (8-stellig)' style='margin:0;'>
               <button type='submit' class='btn-sec' style='width:auto; margin:0;'>Koppeln</button>
            </div>
          </form>
          <form action='/disconnectsumup' method='post'>
             <button type='submit' class='btn-sec' style='color:var(--danger); border-color:var(--danger);'>Entkoppeln (Reset)</button>
          </form>
       </div>
    </div>
  </section>

</div> <!-- End Main -->

<script>
function toggleMenu() {
  document.querySelector('.sidebar').classList.toggle('open');
  document.querySelector('.overlay').classList.toggle('open');
}

function go(id) {
  // Hide all pages
  document.querySelectorAll('.page').forEach(p => p.style.display = 'none');
  
  // Show target
  const target = document.getElementById(id);
  if(target) target.style.display = 'block';

  // Update nav buttons
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  // Find button that calls this function with this id
  // Simple heuristic: re-select by looking at onclick attribute (simple for embedded)
  const buttons = document.querySelectorAll('.nav-btn');
  buttons.forEach(btn => {
     if(btn.getAttribute('onclick').includes(id)) {
        btn.classList.add('active');
     }
  });

  // Mobile: Close menu after click
  if(window.innerWidth <= 768) toggleMenu();

  // Specific logic
  if(id === 'logs') fetchLogs();
}

function fetchLogs(){
  const console = document.getElementById('log-output');
  if(!console) return;
  fetch('/logdata').then(r=>r.text()).then(t => { 
     console.textContent = t; 
     console.scrollTop = console.scrollHeight;
  });
}

function checkOnlineUpdate() {
   const status = document.getElementById('online-update-status');
   const btn = document.getElementById('update-form');
   status.innerText = 'Verbinde zu hanimat.at...';
   btn.style.display = 'none';
   
   fetch('/check-online-update')
     .then(r => r.text())
     .then(data => {
        status.innerText = data;
        if(data.includes('Update verfügbar')) btn.style.display = 'block';
     })
     .catch(e => status.innerText = 'Fehler: ' + e);
}

// Init
document.addEventListener('DOMContentLoaded', () => {
   const hash = window.location.hash.substring(1);
   go(hash ? hash : 'dashboard');
   if(document.getElementById('log-output')) setInterval(fetchLogs, 3000);
});
</script>
</body></html>
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

    // 1. PRIORITÄT: Komplett ausverkauft (0 Fächer)
    if (telegramNotifyEmpty && totalAvailable == 0) {
        if (!emptyNotificationSent) {
            String message = "🚨 ALARM: Der HANIMAT ist komplett ausverkauft! Bitte auffüllen! 😭";
            sendTelegramMessage(message);
            emptyNotificationSent = true;
            almostEmptyNotificationSent = true; // Verhindert, dass beim Befüllen die Info-Nachricht doppelt kommt
            logMessage("Telegram: Alarm 'Ausverkauft' gesendet.");
        }
    } 
    
    // 2. PRIORITÄT: Fast leer (Über 0, aber unter der Schwelle)
    else if (telegramNotifyAlmostEmpty && totalAvailable > 0 && totalAvailable <= almostEmptyThreshold) {
        if (!almostEmptyNotificationSent) {
            String message = "⚠️ INFO: Der HANIMAT ist fast leer!\nVerfügbare Fächer: " + String(totalAvailable);
            sendTelegramMessage(message);
            almostEmptyNotificationSent = true;
            logMessage("Telegram: Info 'Fast leer' gesendet (" + String(totalAvailable) + " übrig).");
        }
    } 

    // 3. STATUS: Wieder aufgefüllt (Bestand über der Schwelle)
    else if (totalAvailable > almostEmptyThreshold) {
        // Falls vorher eine Warnung aktiv war, loggen wir das Zurücksetzen
        if (almostEmptyNotificationSent || emptyNotificationSent) {
            logMessage("Bestand wieder ok (" + String(totalAvailable) + "). Benachrichtigungs-Flags zurückgesetzt.");
        }
        almostEmptyNotificationSent = false;
        emptyNotificationSent = false;
    }
}
/**
 * @brief Sendet einen Status-Ping an das Hanimat-Netzwerk.
 * Respektiert strikt den Offline-Schalter und nutzt Timeouts.
 */
void sendHanimatStatusPing() {
  // 1. HARDWARE-CHECK: Offline-Schalter prüfen
  if (digitalRead(OFFLINE_MODE_PIN) == LOW) return; 

  // 2. SOFTWARE-CHECK: Status aktiv?
  if (!statusEnabled) return;

  // 3. WLAN-CHECK: Nur wenn verbunden
  if (WiFi.status() != WL_CONNECTED) return;

  // 4. PERFORMANCE-CHECK: Nur pingen, wenn der Automat im Leerlauf (IDLE) ist
  // Wenn gerade ein Verkauf läuft oder das Keypad benutzt wird, verschieben wir den Ping.
  if (dispenseJob.active || currentSystemState != CurrentSystemState::IDLE) {
      // Wir setzen den Timer so, dass er es in 1 Minute (60.000 ms) wieder probiert
      lastStatusPing = millis() - (statusInterval - 60000); 
      return;
  }

  // 5. HEARTBEAT SENDEN
  HTTPClient http;
  WiFiClientSecure client; 
  client.setInsecure();  
  
  // Timeouts extrem verkürzen für flüssige Bedienung (1,5 Sekunden statt 3)
  http.setConnectTimeout(1500); 
  http.setTimeout(1500);

  String chipId = String((uint32_t)ESP.getEfuseMac(), HEX);
  chipId.toUpperCase();
  String url = String(statusServerUrl) + "?id=" + chipId + "&key=" + statusApiKey + "&v=" + FIRMWARE_VERSION;

  logMessage("Status: Sende Heartbeat...");

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
        logMessage("Status: OK (200)");
    } else if (httpCode > 0) {
        logMessage("Status: Server Fehler " + String(httpCode));
    } else {
        logMessage("Status: Timeout/Netzwerkfehler");
    }
    http.end();
  } else {
    logMessage("Status: Verbindung fehlgeschlagen");
  }

  // Zeitstempel aktualisieren
  lastStatusPing = millis();
}