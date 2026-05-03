/**
 * @file main.cpp
 * @author Thomas Schöpf / Hanimat
 * @brief Firmware für die HANIMAT Verkaufsmaschine basierend auf der ESP32 Plattform.
 * @version 1.5.0-ec
 * @date 02-02-2026
 *
 * © Copyright Thomas Schöpf
 *
 * Der HANIMAT steht unter der **Creative Commons Namensnennung-NichtKommerziell-Weitergabe unter gleichen Bedingungen 4.0 International (CC BY-NC-SA 4.0)** Lizenz.
 * Urheber des Projekts ist Thomas Schöpf (Hanimat-Projekt).
 * Weitere Informationen finden Sie unter: www.hanimat.at
 */

#include <Arduino.h>
#include <esp_system.h>  // esp_reset_reason()
#include <stdarg.h>      // va_list für logf()
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
#define MAX_LOG_LINES    50
#define MAX_LOG_LINE_LEN 100  // Zeichen pro Eintrag (inkl. Zeitstempel)
// Fixer char-Array statt String-Array: kein Heap-Overhead, keine Fragmentierung
char logBuffer[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
int  logIndex = 0;

/**
 * @brief Interne Log-Kernfunktion — schreibt Timestamp + Nachricht in den Ring-Buffer.
 *        Wird von logMessage() und logf() genutzt. Kein Heap-Alloc.
 */
static void _logWrite(const char* msg) {
  struct tm timeinfo;
  char timeString[20];
  if (getLocalTime(&timeinfo)) {
    strftime(timeString, sizeof(timeString), "%d.%m. %H:%M:%S", &timeinfo);
  } else {
    snprintf(timeString, sizeof(timeString), "%lus", millis() / 1000);
  }
  snprintf(logBuffer[logIndex], MAX_LOG_LINE_LEN, "[%s] %s", timeString, msg);
  Serial.println(logBuffer[logIndex]);
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

/**
 * @brief Log mit Arduino-String (Kompatibilität für bestehende Aufrufe).
 */
void logMessage(const String& msg) {
  _logWrite(msg.c_str());
}

/**
 * @brief Log mit printf-Formatierung — kein Heap-Alloc, kein String-Objekt.
 *        Bevorzugt verwenden: logf("Slot %d, Preis %d Cent", slot, price);
 */
void logf(const char* fmt, ...) {
  char buf[MAX_LOG_LINE_LEN - 25]; // Platz für Timestamp lassen
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  _logWrite(buf);
}


#include "SumUpController.h" // SumUp Klasse muss im selben Ordner liegen

// --- Custom Fonts ---
#include "fonts/Poppins_Black_14.h"
#include "fonts/Poppins_Regular_10.h"
#include "fonts/Poppins_Regular_7.h"


// =================================================================
//                      FIRMWARE VERSION
// =================================================================
const char* FIRMWARE_VERSION = "V1.5.0";

// =================================================================
//                      CONFIGURATION CONSTANTS
// =================================================================

// --- Online Update Configuration ---
const char* UPDATE_VERSION_URL = "https://www.hanimat.at/update/version.json";
const char* UPDATE_FIRMWARE_URL = "https://www.hanimat.at/update/firmware.bin";

// --- Vending Machine Configuration ---
const int MAX_SLOTS = 16; // Maximale Anzahl Fächer (Hardware-Limit)

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
#define SUMUP_BUTTON_PIN 0

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
  IDLE,             // Default state, waiting for user interaction
  USER_INTERACTION, // User is interacting via keypad or payment
  ERROR_DISPLAY,    // An error message is being shown
  OTA_UPDATE,       // OTA update is in progress
  SUMUP_PENDING,    // Waiting for SumUp payment
  DISPENSING        // Relay aktiv, Ausgabe läuft — Display zeigt VIELEN DANK
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
int pendingSumUpAmountCents = 0; // Zu zahlender Betrag in Cent
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
#define HANIMAT_CARD      0x18C3 // Dunkelgrau (~#191919) für Karten-Hintergründe

// --- Slot Data ---
int slotPriceCents[MAX_SLOTS]; // Preise in Cent (z.B. 500 = 5,00 EUR)
bool slotAvailable[MAX_SLOTS];
bool slotLocked[MAX_SLOTS];
int activeSlots = MAX_SLOTS;

// --- Telegram Notification Configuration ---
int almostEmptyThreshold = 5;
bool almostEmptyNotificationSent = false;
bool emptyNotificationSent = false;

bool telegramEnabled = false;
bool telegramNotifyOnSale = false;
bool telegramNotifyAlmostEmpty = true;
bool telegramNotifyEmpty = true;
bool telegramNotifyCrash = true;
bool telegramNotifyBruteForce = true;

// --- Telegram: Nicht-blockierende Nachrichtenwarteschlange ---
static const int TG_QUEUE_MAX = 5;
static String    tgQueue[TG_QUEUE_MAX];
static int       tgQueueHead         = 0;
static int       tgQueueTail         = 0;
static int       tgQueueCount        = 0;
static unsigned long lastTelegramSend = 0;

// --- Hanimat Status Network ---
bool statusEnabled = true; // Variable zum Deaktivieren/Aktivieren
const char* statusServerUrl = "https://status.hanimat.at/api.php";
const char* statusApiKey = "HanimatKeyStatus";
unsigned long lastStatusPing = 0;
const unsigned long statusInterval = 3600000; // Alle 60 Minuten (in ms)

// --- Absturzprotokoll ---
String  lastResetReason       = "Unbekannt";
bool    wasUnexpectedReset    = false;
int     crashCount            = 0; // Wird aus NVS geladen
bool    pendingCrashTelegram  = false; // Telegram-Meldung in ersten Loop-Tick verschieben

// --- Verkaufsstatistik (persistent in NVS) ---
int totalRevenueCents   = 0;          // Gesamtumsatz in Cent
int slotSalesCount[MAX_SLOTS] = {0};  // Verkaufsanzahl pro Fach

// --- Brute-Force Schutz ---
int           loginFailCount    = 0;
unsigned long loginLockoutUntil = 0;
const int     LOGIN_MAX_FAILS   = 5;
const unsigned long LOGIN_LOCKOUT_MS = 5UL * 60UL * 1000UL; // 5 Minuten

// --- Heap Monitoring ---
unsigned long lastHeapCheckTime = 0;
const unsigned long HEAP_CHECK_INTERVAL = 60000; // 60 Sekunden
const uint32_t HEAP_WARN_THRESHOLD     = 30000;  // Warnung unter 30 KB
bool heapWarningSent = false;                     // Damit nicht jede Minute gewarnt wird

// Flag zur Erkennung eines offenen Pins
bool resetPinIsFloating = false; 

// --- Payment & Credit ---
// Guthaben und Preise werden intern in CENT (Integer) gespeichert,
// um Gleitkomma-Präzisionsfehler bei Geldbeträgen zu vermeiden.
int creditCents = 0;
volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;

volatile unsigned long billAcceptorPulseCount = 0;
volatile unsigned long lastBillPulseEdgeTime = 0;
volatile unsigned long lastBillDebounceEdgeTime = 0;

int lastCreditSavedCents = 0;
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
String savedPassword       = DEFAULT_PASSWORD;
String activeSessionToken  = ""; // Leer = niemand eingeloggt
unsigned long lastActivityTimeWeb = 0;

bool displayNeedsUpdate = true;

// --- Verkaufsstatistik: RAM-only Ringpuffer (max. 50 Einträge) ---
enum class PaymentMethod { CASH, SUMUP };
struct SaleLogEntry {
  char time[20];     // "dd.MM. HH:mm:ss" oder "NNNs" (Fallback)
  int  slot;         // 0-basiert
  int  priceCents;
  PaymentMethod method;
};
static const int SALE_LOG_SIZE = 50;
SaleLogEntry saleLog[SALE_LOG_SIZE];
int saleLogCount = 0;  // Gesamtanzahl (max. SALE_LOG_SIZE für Iteration)
int saleLogNext  = 0;  // Schreibzeiger (modulo)

// --- Dispense Job ---
struct DispenseJob {
  bool active;
  int slot;
  unsigned long startTime;
  bool relayActivated;
  PaymentMethod method;
};
DispenseJob dispenseJob = { false, -1, 0, false, PaymentMethod::CASH };
bool dispensingScreenDrawn = false; // Verhindert Flicker: VIELEN DANK Screen nur einmal vollständig zeichnen

// --- Display-Flicker-Schutz: Zone-basiertes Partial-Redraw ---
// Welcher Screen-Modus ist gerade auf dem Display?
enum class DrawnMode { NONE, NORMAL, DISPENSING };
DrawnMode lastDrawnMode = DrawnMode::NONE;
// Welche Inhalte hat der letzte Zeichendurchlauf hinterlassen?
int  lastDrawnSlot        = -2;   // -2 = "noch nie", -1 = Idle war aktiv
int  lastDrawnCreditCents = -1;
bool lastDrawnSlotAvail   = true;
bool lastDrawnSlotLocked  = false;
String lastDrawnKeypadBuffer = "";

// --- OTA Update ---
String otaStatusMessage = "";
bool otaUpdateInProgress = false;

// --- Non-blocking Error Display ---
bool errorDisplayActive = false;
unsigned long errorDisplayUntil = 0;

// --- Non-blocking Melody Player ---
// Kaufmelodie: aufsteigendes C-Dur Arpeggio — eine Oktave tiefer, voller Klang
//               C4   E4   G4   C5    E5    G5
const int MELODY_NOTES[]     = {  262,  330,  392,  523,  659,  784 };
const int MELODY_DURATIONS[] = {   80,   80,   80,  100,  100,  600 };
const int MELODY_LENGTH      = 6;
const int MELODY_NOTE_GAP    = 25; // ms Pause zwischen zwei Noten
bool          melodyActive       = false;
int           melodyNoteIndex    = 0;
unsigned long melodyNoteStart    = 0;
bool          melodyNotePlaying  = false; // true = Ton läuft, false = Pause

// --- Non-blocking Single-Beep (Münze / Schein / Fehler) ---
struct SingleBeep {
  bool          active       = false;
  unsigned long endTime      = 0;
  // Optionale zweite Stufe (z.B. für Fehlerklang 2500 Hz → 2000 Hz)
  bool          hasNextTone  = false;
  int           nextFreq     = 0;
  unsigned long nextDuration = 0;
};
SingleBeep singleBeep;

// --- Non-blocking Relay-Test Jobs ---
struct RelayTestJob {
  bool active;
  int  slot;
  unsigned long startTime;
};
RelayTestJob singleRelayTest = { false, -1, 0 };

struct RelaySequenceJob {
  bool active;
  int  currentSlot;
  unsigned long phaseStartTime;
  bool relayOn; // true = AN-Phase (300ms), false = AUS-Pause (100ms)
};
RelaySequenceJob allRelaysTest = { false, 0, 0, false };

// --- Non-blocking Reset Button State Machine ---
enum class ResetButtonState { NONE, DETECTING, CONFIRMING };
ResetButtonState resetButtonState = ResetButtonState::NONE;
unsigned long resetDetectStartTime = 0;
unsigned long resetConfirmStartTime = 0;

// --- Telegram Bot ---
WiFiClientSecure secured_client;
String telegramBotToken = ""; // Placeholder for Telegram Bot Token
String telegramChatId = "";   // Placeholder for Telegram Chat ID
// Token ist beim Start noch leer – wird in setup() nach NVS-Laden via bot.updateToken() gesetzt
UniversalTelegramBot bot("", secured_client);


// =================================================================
//                      FUNCTION PROTOTYPES
// =================================================================
void setupWebServer();
void updateDisplayScreen();
char manualGetKeyState();
void processKeypad();
void processKeypadSelection();
void scheduleDispense(int slot, PaymentMethod method);
void processDispenseJob();
void addSaleLogEntry(int slot, int priceCents, PaymentMethod method);
bool controlSlotRelay(int slot, bool activate);
void processBillAcceptorPulses();
void resetDisplayToDefault();
void processAcceptedCoin();
void handleLogDataRequest();
void displayOTAMessageTFT(String line1, String line2 = "", String line3 = "", uint16_t color = HANIMAT_ACCENT);
void checkOverallStockLevel();
void sendHanimatStatusPing();
void checkHeapMonitor();
void handleSumUpPaymentInitiation();
void drawPageHeader(String title, uint16_t color = HANIMAT_HEADER);
void saveCreditToNVS(bool force = false);
void processMelody();
void processSingleBeep();
void startBeep(int freq, int durationMs);
void processRelayTestJobs();
// Neue Funktionen für Online Update
void handleCheckOnlineUpdate();
void handleStartOnlineUpdate();

// Web Server Handlers
void handleRoot();
void handleLogin();
void handleLogout();
void handleResetCrashCount();
void handleResetSalesStats();
void handleSetWifi();
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

void handleOTAFileUpload();
void handleTimingConfigPage();
void handleSaveTimingConfig();
void handleTelegramConfigPage();
void handleSaveTelegramConfig();
void handleSendTestTelegram();
void handleDisplayConfigPage();
void handleSaveDisplayConfig();
void handleSalesLog();

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
void sendTelegramMessage(const String& message);
void processTelegramQueue();

// Interrupt Service Routines
void IRAM_ATTR coinAcceptorISR();
void IRAM_ATTR billAcceptorISR();

// =================================================================
//                      HELPER FUNCTIONS
// =================================================================

/**
 * @brief Wandelt einen Cent-Wert in einen EUR-String um.
 * @param cents  Betrag in Cent (z.B. 510 → "5.10")
 * @return String im Format "X.YY" (ohne EUR-Einheit)
 */
String centsToEurStr(int cents) {
    char buf[12];
    if (cents < 0) {
        snprintf(buf, sizeof(buf), "-%d.%02d", (-cents) / 100, (-cents) % 100);
    } else {
        snprintf(buf, sizeof(buf), "%d.%02d", cents / 100, cents % 100);
    }
    return String(buf);
}

// =============================================================================
// --- WEB SESSION HELPERS ---
// =============================================================================

/**
 * @brief Liest den HANIMAT_SESSION-Cookie aus dem aktuellen Request.
 * @return Token-String oder "" wenn kein Cookie vorhanden.
 */
String getSessionCookie() {
  String cookie = server.header("Cookie");
  int idx = cookie.indexOf("HANIMAT_SESSION=");
  if (idx < 0) return "";
  idx += 16; // Länge von "HANIMAT_SESSION="
  int end = cookie.indexOf(';', idx);
  return (end < 0) ? cookie.substring(idx) : cookie.substring(idx, end);
}

/**
 * @brief Prüft ob der aktuelle Request eine gültige Session hat.
 * @return true wenn eingeloggt, sonst false.
 */
bool isAuth() {
  if (activeSessionToken.length() == 0) return false;
  return getSessionCookie() == activeSessionToken;
}

/**
 * @brief Generiert einen neuen zufälligen Session-Token (16 Hex-Zeichen).
 *        Verwendet den Hardware-RNG des ESP32.
 */
String generateSessionToken() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08X%08X", (unsigned int)esp_random(), (unsigned int)esp_random());
  return String(buf);
}

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
 * @brief Startet die "Danke"-Melodie (non-blocking).
 *        Die eigentliche Wiedergabe übernimmt processMelody() im Loop.
 */
void playThankYouMelody() {
  melodyActive      = true;
  melodyNoteIndex   = 0;
  melodyNotePlaying = true;
  melodyNoteStart   = millis();
  ledcWriteTone(0, MELODY_NOTES[0]); // Erste Note sofort starten
}

/**
 * @brief Verarbeitet Test-Relay-Jobs aus dem Web-Interface (non-blocking).
 *        Muss jeden Loop-Durchlauf aufgerufen werden.
 */
void processRelayTestJobs() {
  unsigned long now = millis();

  // Einzel-Test: Relais nach 1 Sekunde ausschalten
  if (singleRelayTest.active && now - singleRelayTest.startTime >= 1000) {
    controlSlotRelay(singleRelayTest.slot, false);
    singleRelayTest.active = false;
  }

  // Sequenz-Test: Alle Relais nacheinander (300ms AN / 100ms Pause)
  if (allRelaysTest.active) {
    if (allRelaysTest.relayOn) {
      if (now - allRelaysTest.phaseStartTime >= 300) {
        controlSlotRelay(allRelaysTest.currentSlot, false);
        allRelaysTest.relayOn = false;
        allRelaysTest.phaseStartTime = now;
      }
    } else {
      if (now - allRelaysTest.phaseStartTime >= 100) {
        allRelaysTest.currentSlot++;
        if (allRelaysTest.currentSlot >= activeSlots) {
          allRelaysTest.active = false;
          logMessage("Web: Relais-Sequenztest abgeschlossen.");
        } else {
          controlSlotRelay(allRelaysTest.currentSlot, true);
          allRelaysTest.relayOn = true;
          allRelaysTest.phaseStartTime = now;
        }
      }
    }
  }
}

/**
 * @brief Verarbeitet die Melodie-Wiedergabe Note für Note.
 *        Muss jeden Loop-Durchlauf aufgerufen werden.
 */
void processMelody() {
  if (!melodyActive) return;
  unsigned long now = millis();

  if (melodyNotePlaying) {
    // Prüfen ob aktuelle Note lange genug gespielt hat
    if (now - melodyNoteStart >= (unsigned long)MELODY_DURATIONS[melodyNoteIndex]) {
      ledcWriteTone(0, 0);       // Ton stoppen
      melodyNotePlaying = false;
      melodyNoteStart   = now;   // Timer für Pause starten
    }
  } else {
    // In der Pause: prüfen ob Pause vorbei ist
    if (now - melodyNoteStart >= (unsigned long)MELODY_NOTE_GAP) {
      melodyNoteIndex++;
      if (melodyNoteIndex >= MELODY_LENGTH) {
        melodyActive = false;    // Melodie vollständig abgespielt
        melodyNoteIndex = 0;
      } else {
        ledcWriteTone(0, MELODY_NOTES[melodyNoteIndex]);
        melodyNotePlaying = true;
        melodyNoteStart   = now;
      }
    }
  }
}

/**
 * @brief Non-blocking beep helper — stoppt den Ton wenn die Zeit abgelaufen ist.
 *        Muss in loop() aufgerufen werden.
 */
void processSingleBeep() {
  if (!singleBeep.active) return;
  if (millis() >= singleBeep.endTime) {
    if (singleBeep.hasNextTone) {
      // Zweite Stufe abspielen
      ledcWriteTone(0, singleBeep.nextFreq);
      singleBeep.endTime    = millis() + singleBeep.nextDuration;
      singleBeep.hasNextTone = false;
    } else {
      ledcWriteTone(0, 0);
      singleBeep.active = false;
    }
  }
}

/**
 * @brief Startet einen nicht-blockierenden Einzel-Beep.
 * @param freq      Frequenz in Hz
 * @param durationMs Dauer in Millisekunden
 */
void startBeep(int freq, int durationMs) {
  melodyActive           = false; // Laufende Melodie stoppen
  singleBeep.active      = true;
  singleBeep.endTime     = millis() + durationMs;
  singleBeep.hasNextTone = false;  // Keine zweite Stufe
  ledcWriteTone(0, freq);
}

/**
 * @brief Plays a descending two-tone error sound on the buzzer (non-blocking).
 *        Stufe 1: 2500 Hz für 150 ms → Stufe 2: 2000 Hz für 250 ms → Stille.
 *        Wird von processSingleBeep() im Loop abgearbeitet.
 */
void playErrorSound() {
  melodyActive            = false;
  singleBeep.active       = true;
  singleBeep.endTime      = millis() + 150;
  singleBeep.hasNextTone  = true;
  singleBeep.nextFreq     = 2000;
  singleBeep.nextDuration = 250;
  ledcWriteTone(0, 2500);
}

/**
 * @brief Plays a short beep sound for keypad presses.
 */
void playKeyPressBeep() {
  startBeep(2800, 50); // Non-blocking, stoppt automatisch via processSingleBeep()
}

/**
 * @brief Checks if the I2C relay expander board is connected and responsive.
 * @return True if the board acknowledges its address, false otherwise.
 */
bool checkRelayBoardOnline() {
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  byte error = Wire.endTransmission();
  if (error != 0) {
    logf("ERROR: Relay board I2C not reachable (Addr: 0x%X, Code: %d)", RELAY_I2C_ADDRESS, error);
  }
  return (error == 0);
}

/**
 * @brief Sends a message via Telegram if enabled and configured.
 * @param message The message string to send.
 */
/**
 * @brief Reiht eine Telegram-Nachricht in die Queue ein.
 *        Der tatsächliche HTTPS-Aufruf erfolgt nicht-blockierend in processTelegramQueue()
 *        im loop() – nur wenn IDLE und kein Dispense aktiv ist.
 */
void sendTelegramMessage(const String& message) {
  if (!telegramEnabled) return;
  if (telegramBotToken.length() == 0 || telegramChatId.length() == 0) {
    logMessage("Telegram: Nicht konfiguriert, Nachricht verworfen.");
    return;
  }
  if (tgQueueCount >= TG_QUEUE_MAX) {
    logMessage("Telegram: Queue voll, älteste Nachricht überschrieben.");
    // Älteste überschreiben statt neue verwerfen (wichtigere Infos zuerst)
    tgQueue[tgQueueTail] = message;
    tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_MAX;
    tgQueueHead = (tgQueueHead + 1) % TG_QUEUE_MAX; // Head nachrücken
    return;
  }
  tgQueue[tgQueueTail] = message;
  tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_MAX;
  tgQueueCount++;
  logf("Telegram: Nachricht eingereiht (%d/%d).", tgQueueCount, TG_QUEUE_MAX);
}

/**
 * @brief Verarbeitet die Telegram-Queue – wird einmal pro loop()-Iteration aufgerufen.
 *        Sendet maximal eine Nachricht, nur wenn WiFi verbunden, kein Dispense aktiv
 *        und mindestens 3 Sekunden seit dem letzten Send vergangen sind.
 */
void processTelegramQueue() {
  if (tgQueueCount == 0) return;
  if (!telegramEnabled) {
    // Queue leeren falls Telegram deaktiviert wurde
    tgQueueHead = tgQueueTail = tgQueueCount = 0;
    return;
  }
  if (digitalRead(OFFLINE_MODE_PIN) == LOW) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (dispenseJob.active) return;                           // Nicht während Warenausgabe
  if (millis() - lastTelegramSend < 3000) return;          // Rate-Limit: 1 msg / 3 Sek.

  const String& msg = tgQueue[tgQueueHead];
  logf("Telegram: Sende '%s'...", msg.substring(0, 50).c_str());
  if (bot.sendMessage(telegramChatId, msg, "")) {
    logMessage("Telegram: Gesendet.");
  } else {
    logMessage("Telegram: Sendefehler.");
  }
  tgQueue[tgQueueHead] = "";                               // String-Heap freigeben
  tgQueueHead = (tgQueueHead + 1) % TG_QUEUE_MAX;
  tgQueueCount--;
  lastTelegramSend = millis();
}

/**
 * @brief Displays a multi-line message on the TFT, typically for OTA updates.
 * @param line1 First line of the message.
 * @param line2 Second line (optional).
 * @param line3 Third line (optional).
 * @param color Color for the first line.
 */
void displayOTAMessageTFT(String line1, String line2, String line3, uint16_t color) {
  lastDrawnMode = DrawnMode::NONE; // fillScreen kommt gleich — Cache invalidieren
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
    // Nur speichern, wenn sich der Wert geändert hat (Wear Leveling)
    if (creditCents != lastCreditSavedCents || force) {
        preferences.begin("hanimat", false);
        preferences.putInt("creditCts", creditCents);
        preferences.end();

        lastCreditSavedCents = creditCents;
        logf("NVS: Guthaben gesichert: %s EUR", centsToEurStr(creditCents).c_str());
    }
}

/**
 * @brief Wird vom WiFiManager aufgerufen, wenn er in den AP-Modus (Setup-Portal) wechselt.
 */
void configModeCallback(WiFiManager *myWiFiManager) {
  logMessage("Kein WLAN gefunden. Setup-Portal gestartet.");
  lastDrawnMode = DrawnMode::NONE;
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
//                       ABSTURZPROTOKOLL
// =================================================================
/**
 * @brief Liest den Hardware-Reset-Grund aus und klassifiziert ihn.
 *        Muss vor dem NVS-Load aufgerufen werden.
 */
void checkAndLogResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   lastResetReason = "Power-On Reset";          wasUnexpectedReset = false; break;
    case ESP_RST_EXT:       lastResetReason = "Externer Reset-Pin";      wasUnexpectedReset = false; break;
    case ESP_RST_SW:        lastResetReason = "Software Reset (normal)"; wasUnexpectedReset = false; break;
    case ESP_RST_DEEPSLEEP: lastResetReason = "Deep-Sleep Aufwachen";    wasUnexpectedReset = false; break;
    case ESP_RST_BROWNOUT:  lastResetReason = "Brownout (Unterspannung)";wasUnexpectedReset = true;  break;
    case ESP_RST_PANIC:     lastResetReason = "PANIC / Exception";       wasUnexpectedReset = true;  break;
    case ESP_RST_INT_WDT:   lastResetReason = "Interrupt-WDT Timeout";  wasUnexpectedReset = true;  break;
    case ESP_RST_TASK_WDT:  lastResetReason = "Task-WDT Timeout";       wasUnexpectedReset = true;  break;
    case ESP_RST_WDT:       lastResetReason = "Watchdog Reset";          wasUnexpectedReset = true;  break;
    default:                lastResetReason = "Unbekannt (" + String((int)reason) + ")";
                            wasUnexpectedReset = true; break;
  }
  if (wasUnexpectedReset) {
    logf("⚠ ABSTURZ erkannt! Grund: %s", lastResetReason.c_str());
  } else {
    logf("System: Neustart-Grund: %s", lastResetReason.c_str());
  }
}

// =================================================================
//                            SETUP
// =================================================================
/**
 * @brief Zentrale Setup-Routine.
 * Optimiert auf minimalen Boot-Verzug: Display-Init erfolgt sofort, 
 * gefolgt von Hardware-Aktivierung und zeitbegrenztem Netzwerk-Setup.
 */
void setup() {
  // --- 1. SOFORTIGER VISUELLER START (Millisekunden-Bereich) ---
  Serial.begin(115200);
  memset(logBuffer, 0, sizeof(logBuffer)); // Log-Buffer sauber initialisieren
  
  // Display als allererstes starten, um "weißen Bildschirm" zu vermeiden
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(HANIMAT_BG);

  // Hilfsvariablen für die Formatierung
  int16_t x1, y1; uint16_t w, h;
  
  // Hauptlogo zentriert zeichnen
  tft.setFont(&Poppins_Black14pt7b);
  tft.setTextColor(HANIMAT_HEADER);
  tft.getTextBounds("HANIMAT", 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) - h);
  tft.println("HANIMAT");

  // Untertitel "startet..." zentriert zeichnen
  tft.setFont(&Poppins_Regular10pt7b);
  tft.setTextColor(HANIMAT_TEXT);
  String subtitle = "startet...";
  tft.getTextBounds(subtitle, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, (tft.height() / 2) + 25);
  tft.println(subtitle);
  
  // Kurzer Halt für das Auge, während im Hintergrund die Hardware anläuft
  delay(100);

  logf("System Start: HANIMAT %s", FIRMWARE_VERSION);
  bootTime = millis();
  checkAndLogResetReason(); // Reset-Grund sofort sichern (vor allem anderen)

  // --- 2. I2C & RELAIS (ELEKTRISCHE INITIALISIERUNG) ---
  Wire.begin();
  Wire.setClock(100000L);       // Fast-Mode 100kHz
  Wire.setTimeout(3000); // 3ms Timeout bei Bus-Hänger (kein Einfrieren mehr)

  // Alle Relais sofort in definierten AUS-Zustand zwingen
  expanderOutputStates[0] = 0x0000;
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x02); // Register: Output Port 0
  Wire.write(0x00); // Port 0 auf LOW
  Wire.write(0x00); // Port 1 auf LOW (Auto-Inkrement)
  Wire.endTransmission();

  // Expander-Konfiguration als Ausgang
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  Wire.write(0x06); // Register: Configuration Port 0
  Wire.write(0x00); 
  Wire.write(0x00); 
  Wire.endTransmission();
  logMessage("Hardware: I2C Relais-Board bereit.");

  // --- 3. PIN-KONFIGURATION & ZAHLUNGSSYSTEME ---
  pinMode(WIFI_RESET_BUTTON, INPUT);
  pinMode(OFFLINE_MODE_PIN, INPUT_PULLUP);
  pinMode(BILL_INHIBIT_PIN, OUTPUT);
  digitalWrite(BILL_INHIBIT_PIN, HIGH); // Inhibit bis Ende Setup
  pinMode(SUMUP_BUTTON_PIN, INPUT); 
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Buzzer Audio-Setup
  ledcSetup(0, 2000, 8);
  ledcAttachPin(BUZZER_PIN, 0);

  // Keypad Matrix Pins
  for (int i = 0; i < KEYPAD_ROWS; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], LOW);
  }
  for (int i = 0; i < KEYPAD_COLS; i++) {
    pinMode(colPins[i], INPUT);
  }

  // Zahlungssysteme (Interrupts) sofort scharfschalten
  pinMode(COIN_ACCEPTOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(COIN_ACCEPTOR_PIN), coinAcceptorISR, RISING);
  pinMode(BILL_ACCEPTOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BILL_ACCEPTOR_PIN), billAcceptorISR, RISING);
  logMessage("Hardware: Zahlungssysteme aktiv.");

  // --- 4. NVS PREFERENCES (LADEN DER DATEN) ---
  preferences.begin("hanimat", false);
  char kBuf[16];

  if (!preferences.isKey("initialized")) {
    logMessage("NVS: Erst-Initialisierung...");
    for (int i = 0; i < MAX_SLOTS; i++) {
      snprintf(kBuf, sizeof(kBuf), "priceC%d", i); // Cent-Schlüssel
      preferences.putInt(kBuf, 500 + i * 10);      // 5,00 EUR / 5,10 EUR / ...
      snprintf(kBuf, sizeof(kBuf), "avail%d", i);
      preferences.putBool(kBuf, true);
      snprintf(kBuf, sizeof(kBuf), "locked%d", i);
      preferences.putBool(kBuf, false);
    }
    preferences.putString("password", DEFAULT_PASSWORD);
    preferences.putBool("initialized", true);
  }

  // --- Einmalige Migration: float-Preise → int-Cent-Werte ---
  // Für Geräte, die noch die alten float-Schlüssel ("price0" etc.) haben.
  if (!preferences.isKey("migratedV3")) {
    logMessage("NVS: Starte Einmal-Migration float→Cent...");
    for (int i = 0; i < MAX_SLOTS; i++) {
      snprintf(kBuf, sizeof(kBuf), "price%d", i);
      if (preferences.isKey(kBuf)) {
        float oldPrice = preferences.getFloat(kBuf, 5.0f);
        char newKey[12];
        snprintf(newKey, sizeof(newKey), "priceC%d", i);
        preferences.putInt(newKey, (int)(oldPrice * 100.0f + 0.5f));
      }
    }
    if (preferences.isKey("credit")) {
      float oldCredit = preferences.getFloat("credit", 0.0f);
      preferences.putInt("creditCts", (int)(oldCredit * 100.0f + 0.5f));
    }
    preferences.putBool("migratedV3", true);
    logMessage("NVS: Migration abgeschlossen.");
  }

  // System-Parameter laden
  COIN_PROCESSING_DELAY            = preferences.getULong("coinDelay",   150);
  BILL_ISR_DEBOUNCE_MS             = preferences.getULong("billIsrDeb",   75);
  BILL_GROUP_PROCESSING_TIMEOUT_MS = preferences.getULong("billGrpTout",1500);
  DISPENSE_RELAY_ON_TIME           = preferences.getULong("dispTime",   5000);
  KEYPAD_INPUT_TIMEOUT             = preferences.getULong("keypadTime", 3000);
  SLOT_SELECTION_TIMEOUT           = preferences.getULong("slotSelTime",10000);
  DISPLAY_TIMEOUT                  = preferences.getULong("dispTimeout",20000);
  WEB_TIMEOUT                      = preferences.getULong("webTout",  600000);
  
  telegramEnabled           = preferences.getBool("tgEnabled",      false);
  telegramBotToken          = preferences.getString("tgToken",       "");
  telegramChatId            = preferences.getString("tgChatId",      "");
  telegramNotifyOnSale      = preferences.getBool("tgNotifySale",   false);
  telegramNotifyAlmostEmpty = preferences.getBool("tgNotifyAlmost", true);
  telegramNotifyEmpty       = preferences.getBool("tgNotifyEmpty",  true);
  telegramNotifyCrash       = preferences.getBool("tgNotifyCrash",  true);
  telegramNotifyBruteForce  = preferences.getBool("tgNotifyBrute",  true);
  almostEmptyThreshold = preferences.getInt("tgAlmostThres", 5);
  statusEnabled = preferences.getBool("statusEnabled", true);
  bot.updateToken(telegramBotToken);
  
  sumupEnabled = preferences.getBool("suEnabled", false);
  sumupApiKey = preferences.getString("suApiKey", "");
  sumupMerchantId = preferences.getString("suMid", "");
  sumupReaderId = preferences.getString("suRid", "");
  sumupTimeout = preferences.getULong("suTimeout", 60000);
  sumUp = SumUpController(sumupApiKey, sumupMerchantId, sumupReaderId);

  displaySlogan = preferences.getString("dispSlogan", "");
  displayFooter = preferences.getString("dispFooter", "www.hanimat.at");
  activeSlots = preferences.getInt("activeSlots", MAX_SLOTS);

  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(kBuf, sizeof(kBuf), "priceC%d", i);
    slotPriceCents[i] = preferences.getInt(kBuf, 500 + i * 10); // Default 5,00–6,50 EUR
    snprintf(kBuf, sizeof(kBuf), "avail%d", i);
    slotAvailable[i] = preferences.getBool(kBuf, true);
    snprintf(kBuf, sizeof(kBuf), "locked%d", i);
    slotLocked[i] = preferences.getBool(kBuf, false);
  }
  creditCents = preferences.getInt("creditCts", 0);
  savedPassword = preferences.getString("password", DEFAULT_PASSWORD);

  // Verkaufsstatistik laden
  totalRevenueCents = preferences.getInt("totalRev", 0);
  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(kBuf, sizeof(kBuf), "sales%d", i);
    slotSalesCount[i] = preferences.getInt(kBuf, 0);
  }

  // Absturzzähler laden und ggf. inkrementieren
  crashCount = preferences.getInt("crashCount", 0);
  if (wasUnexpectedReset) {
    crashCount++;
    preferences.putInt("crashCount", crashCount);
    logf("NVS: Absturzzähler → %d", crashCount);
  }

  preferences.end();
  logMessage("NVS: Konfiguration geladen.");

  // --- 5. WIFI-RESET-PIN PRÜFUNG ---
  int lowCount = 0; 
  for (int i = 0; i < 500; i++) {
    if (digitalRead(WIFI_RESET_BUTTON) == LOW) lowCount++;
    delayMicroseconds(100);
  }
  resetPinIsFloating = (lowCount > 5);

  // --- 6. NETZWERK INITIALISIERUNG (ZEIT-OPTIMIERT) ---
  bool offlineMode = (digitalRead(OFFLINE_MODE_PIN) == LOW);
  WiFiManager wm;
  
  // WICHTIG: Verbindungsversuch auf 6 Sek begrenzen, um langes Hängen zu vermeiden
  wm.setConnectTimeout(6);
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback(configModeCallback);

  if (offlineMode) {
    logMessage("NETZ: Offline-Modus (Access-Point)");
    WiFi.softAP("HANIMAT-Offline", "Honig1234");
    
    tft.fillScreen(HANIMAT_BG);
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_ACCENT);
    String offMsg = "OFFLINE MODUS";
    tft.getTextBounds(offMsg, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 40); tft.println(offMsg);
    
    tft.setTextColor(HANIMAT_TEXT);
    tft.setCursor(10, 70); tft.println("AP: HANIMAT-Offline");
    tft.setCursor(10, 100); tft.println("IP: " + WiFi.softAPIP().toString());
    delay(300);

  } else {
    logMessage("NETZ: Online-Modus wird gestartet");
    
    // Statische IP falls im Web-Interface konfiguriert
    preferences.begin("hanimat", false);
    if (preferences.isKey("static_ip")) {
        IPAddress staticIP, gateway, subnet, dns1;
        staticIP.fromString(preferences.getString("static_ip", ""));
        gateway.fromString(preferences.getString("gateway", ""));
        subnet.fromString(preferences.getString("subnet", ""));
        dns1.fromString(preferences.getString("dns1", "8.8.8.8"));
        if(staticIP[0] != 0) {
            wm.setSTAStaticIPConfig(staticIP, gateway, subnet, dns1);
        }
    }
    preferences.end();

    // WLAN-Override: falls via Web-UI neue Zugangsdaten gesetzt wurden
    preferences.begin("hanimat", false);
    if (preferences.isKey("wifi_ssid")) {
      String ovSsid = preferences.getString("wifi_ssid", "");
      String ovPass = preferences.getString("wifi_pass", "");
      preferences.remove("wifi_ssid");
      preferences.remove("wifi_pass");
      preferences.end();
      if (ovSsid.length() > 0) {
        logf("NETZ: Neue Zugangsdaten werden gesetzt: '%s'", ovSsid.c_str());
        wm.resetSettings();              // Alte WiFiManager-Credentials löschen
        WiFi.begin(ovSsid.c_str(), ovPass.c_str()); // Neue in Flash schreiben
        delay(300);
      }
    } else {
      preferences.end();
    }

    // autoConnect bricht nun nach 10 Sek ab, wenn der Router nicht reagiert
    if (!wm.autoConnect("HANIMAT-Setup", "Honig1234")) {
      logMessage("NETZ: Verbindung fehlgeschlagen / Portal offen.");
    } else {
      logf("NETZ: Verbunden. IP: %s", WiFi.localIP().toString().c_str());
      
      // NTP Zeit-Synchronisierung (Asynchron - blockiert nicht!)
      configTime(0, 0, "pool.ntp.org");
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); 
      tzset();
      
      // --- INFO-Screen beim Start ---
      tft.fillScreen(HANIMAT_BG);
      drawPageHeader("INFO", HANIMAT_ACCENT);

      // WLAN verbunden (grün)
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_SUCCESS);
      String connMsg = "WLAN verbunden";
      tft.getTextBounds(connMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 80);
      tft.println(connMsg);

      // IP-Adresse
      tft.setTextColor(HANIMAT_TEXT);
      String ipMsg = "IP: " + WiFi.localIP().toString();
      tft.getTextBounds(ipMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 105);
      tft.println(ipMsg);

      // Versions-Zeile
      tft.setTextColor(HANIMAT_INFO);
      String verMsg = String("Firmware ") + FIRMWARE_VERSION;
      tft.getTextBounds(verMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 130);
      tft.println(verMsg);

      // By-Zeile (kleiner)
      tft.setFont(&Poppins_Regular7pt7b);
      tft.setTextColor(HANIMAT_DIVIDER);
      String byMsg = "By Hanimat | Thomas Schoepf";
      tft.getTextBounds(byMsg, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 160);
      tft.println(byMsg);
      // Kein delay — Setup läuft sofort weiter, Screen bleibt kurz sichtbar
    }
  }

  // --- 7. FINALE DIENSTE ---
  // TLS-Zertifikat für Telegram setzen (DigiCert High Assurance EV Root CA)
  // TLS-verschlüsselt, aber ohne Zertifikats-Pinning.
  // Telegram rotiert Intermediate-CAs regelmäßig — setCACert() würde nach jedem CA-Wechsel brechen.
  // setInsecure() ist für diesen Kontext die stabilere Wahl.
  secured_client.setInsecure();
  setupWebServer();
  sendHanimatStatusPing();

  // Crash-Meldung via Telegram: nicht hier senden (blockiert Setup ~3s),
  // sondern im ersten Loop-Tick — Flag setzen, Loop sendet dann non-blocking.
  if (wasUnexpectedReset && telegramEnabled && telegramNotifyCrash) {
    pendingCrashTelegram = true;
  }

  logMessage("System: Setup abgeschlossen.");
  digitalWrite(BILL_INHIBIT_PIN, LOW); // Zahlung freischalten
  
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

  // --- Ausstehende Crash-Telegram-Meldung (aus Setup verschoben, blockiert nicht mehr den Start) ---
  if (pendingCrashTelegram) {
    pendingCrashTelegram = false;
    sendTelegramMessage(
      "⚠️ HANIMAT Absturz erkannt!\n"
      "Grund: " + lastResetReason + "\n"
      "Absturzzähler gesamt: " + String(crashCount) + "\n"
      "Firmware: " + String(FIRMWARE_VERSION)
    );
  }

  // --- Non-blocking Error Display Timeout ---
  if (errorDisplayActive && millis() >= errorDisplayUntil) {
    errorDisplayActive = false;
    resetDisplayToDefault();
  }

  // --- Non-blocking Melody Player ---
  processMelody();

  // --- Non-blocking Single Beep (Münze/Schein) ---
  processSingleBeep();

  // --- Non-blocking Relay-Test Jobs ---
  processRelayTestJobs();

// --- Reset-Logik: Non-blocking State Machine ---
  if (!resetPinIsFloating) {
    bool btnPressed = (digitalRead(WIFI_RESET_BUTTON) == LOW);

    if (resetButtonState == ResetButtonState::NONE) {
      if (btnPressed) {
        resetButtonState = ResetButtonState::DETECTING;
        resetDetectStartTime = millis();
      }

    } else if (resetButtonState == ResetButtonState::DETECTING) {
      if (!btnPressed) {
        // Signal instabil (Rauschen) – abbrechen
        resetButtonState = ResetButtonState::NONE;
      } else if (millis() - resetDetectStartTime >= 2000) {
        // 2 Sekunden stabil gedrückt → Bestätigungs-Dialog zeigen
        resetButtonState = ResetButtonState::CONFIRMING;
        resetConfirmStartTime = millis();
        logMessage("Reset-Knopf stabil gedrueckt. Warte auf # am Keypad...");
        lastDrawnMode = DrawnMode::NONE;
        tft.fillScreen(HANIMAT_BG);
        drawPageHeader("SYSTEM RESET", HANIMAT_ACCENT);
        tft.setFont(&Poppins_Regular10pt7b);
        tft.setTextColor(HANIMAT_TEXT);
        tft.setCursor(10, 110); tft.println("Bestaetigen mit #");
        tft.setCursor(10, 140); tft.println("Abbruch nach 5 Sek.");
        playKeyPressBeep();
      }

    } else if (resetButtonState == ResetButtonState::CONFIRMING) {
      char key = manualGetKeyState();
      if (key == '#') {
        logMessage("RESET BESTAETIGT!");
        lastDrawnMode = DrawnMode::NONE;
        tft.fillScreen(HANIMAT_BG);
        tft.setTextColor(HANIMAT_ERROR);
        tft.setFont(&Poppins_Black14pt7b);
        tft.setCursor(10, 80); tft.println("WERKSRESET...");
        playErrorSound();
        delay(2000); // Kurze Pause vor Neustart – hier bewusst OK
        preferences.begin("hanimat", false);
        preferences.clear();
        preferences.end();
        WiFiManager wm;
        wm.resetSettings();
        logMessage("Factory reset complete. Restarting...");
        ESP.restart();
      } else if (millis() - resetConfirmStartTime >= 5000) {
        logMessage("Reset abgebrochen.");
        resetButtonState = ResetButtonState::NONE;
        resetDisplayToDefault();
      }
    }
  }

  // Guthaben verzögert speichern (Wear Leveling)
  if (creditCents != lastCreditSavedCents && (millis() - lastCreditChangeTime > NVS_SAVE_DELAY)) {
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
    
    // Timeout für Teil-Eingabe im Buffer (z.B. "1" bei 16 Fächern wartet auf 2. Ziffer)
    if (keypadInputBuffer.length() > 0 && selectedSlot == -1 &&
        (millis() - lastKeypadInputTime > KEYPAD_INPUT_TIMEOUT)) {
      logf("Keypad: Eingabe-Timeout. Buffer '%s' geleert.", keypadInputBuffer.c_str());
      keypadInputBuffer = "";
      displayNeedsUpdate = true;
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

    // --- Fortschrittsbalken-Animation: Display alle 200ms neu zeichnen während Ausgabe läuft ---
    static unsigned long lastDispenseBarUpdate = 0;
    if (currentSystemState == CurrentSystemState::DISPENSING &&
        millis() - lastDispenseBarUpdate >= 200) {
      lastDispenseBarUpdate = millis();
      displayNeedsUpdate = true;
    }
  }

  // --- SUMUP Button Check (non-blocking debounce) ---
  // Nur prüfen wenn SumUp aktiviert ist UND keine Transaktion läuft
  static unsigned long sumupBtnPressTime = 0;
  if (sumupEnabled && !isSumUpTransactionActive) {
    if (digitalRead(SUMUP_BUTTON_PIN) == LOW) {
      if (sumupBtnPressTime == 0) {
        sumupBtnPressTime = millis(); // Ersten LOW-Zeitpunkt merken
      } else if (millis() - sumupBtnPressTime >= 100) {
        sumupBtnPressTime = 0;
        handleSumUpPaymentInitiation();
      }
    } else {
      sumupBtnPressTime = 0; // Button losgelassen, Timer zurücksetzen
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
            logf("SumUp Status Check: %s", status.c_str()); 
        }

if (status == "SUCCESSFUL") {
            logMessage(">>> SUMUP ZAHLUNG ERFOLGREICH! <<<");
            
            // Logik für korrekte Verrechnung:
            // Der via Karte bezahlte Restbetrag (remainingCents = Preis - Münzguthaben)
            // wird zum bestehenden Guthaben addiert — nicht überschrieben.
            // Ergebnis: creditCents == slotPriceCents[selectedSlot], scheduleDispense() zieht ab.
            int paidBySumUp = slotPriceCents[selectedSlot] - creditCents;
            if (paidBySumUp > 0) creditCents += paidBySumUp;

            logf("SumUp: Zahlung abgeschlossen. Internes Guthaben angepasst auf: %s EUR", centsToEurStr(creditCents).c_str());

            // Guthaben sofort sichern (Absturzsicherheit vor Warenausgabe)
            saveCreditToNVS(true);

            // SumUp Status zurücksetzen
            isSumUpTransactionActive = false; // Stoppt das Polling
            currentSumUpTxId = "";
            currentSystemState = CurrentSystemState::IDLE; // Zurück in den Standardmodus
            
            // Warenausgabe starten
            logf("Starte Warenausgabe fuer Fach %d", selectedSlot + 1);
            scheduleDispense(selectedSlot, PaymentMethod::SUMUP);
            
        }
        else if (status == "FAILED" || status == "CANCELLED") {
            logf("SumUp: Zahlung fehlgeschlagen oder abgebrochen (%s).", status.c_str());
            isSumUpTransactionActive = false; // Polling beenden
            currentSumUpTxId = "";
            displayErrorMessage("ZAHLUNG", "abgebrochen");
        }
        // Bei "PENDING" passiert nichts, der Loop läuft einfach weiter
    }
  }

  // Auto-logout from web interface after timeout
  if (activeSessionToken.length() > 0 && (millis() - lastActivityTimeWeb > WEB_TIMEOUT)) {
    activeSessionToken = "";
    logMessage("Web: Session abgelaufen (Inaktivität).");
  }

  // Update display only when needed.
  // Gesperrt während: OTA, SumUp-Zahlung, aktiver Error-Anzeige (verhindert Überschreiben durch Münzeinwurf etc.)
  if (displayNeedsUpdate
      && currentSystemState != CurrentSystemState::OTA_UPDATE
      && !isSumUpTransactionActive
      && !errorDisplayActive) {
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

  // --- TELEGRAM: Nicht-blockierende Nachrichtenverarbeitung ---
  processTelegramQueue();

  // --- HEAP MONITORING ---
  checkHeapMonitor();
  yield(); // CPU an andere Tasks abgeben (Watchdog, WiFi-Stack) ohne zu blockieren
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

  int priceCents = slotPriceCents[selectedSlot];

  // LOG START
  logf("SumUp: Prozess gestartet fuer Fach %d", selectedSlot + 1);

  // --- LOGIK FÜR MISCHZAHLUNG ---
  int remainingCents = priceCents - creditCents;

  logf("SumUp: Preis: %s EUR, Guthaben: %s EUR -> Zu zahlen: %s EUR", centsToEurStr(priceCents).c_str(), centsToEurStr(creditCents).c_str(), centsToEurStr(remainingCents).c_str());

  if (remainingCents <= 0) {
    // Falls das Guthaben bereits reicht, brauchen wir kein SumUp.
    logMessage("SumUp: Abbruch, Guthaben deckt bereits den Preis.");
    scheduleDispense(selectedSlot, PaymentMethod::CASH);
    return;
  }

  // --- CHECK: MINDESTBETRAG 1.00 EUR (SumUp Limit = 100 Cent) ---
  if (remainingCents < 100) {
    logf("SumUp: Restbetrag %s EUR ist zu gering. Minimum 1.00 EUR.", centsToEurStr(remainingCents).c_str());
    displayErrorMessage("MIN. KARTE", "ab 1.00 EUR");
    return;
  }
  // ---------------------------------------------------------------

  // 3. Anzeige auf TFT vorbereiten
  lastDrawnMode = DrawnMode::NONE; // fillScreen kommt gleich — Cache invalidieren
  tft.fillScreen(HANIMAT_BG);
  drawPageHeader("KARTENZAHLUNG", HANIMAT_HEADER);

  tft.setFont(&Poppins_Regular10pt7b);
  int yPos = 80;
  int lineSpacing = 25;

  tft.setCursor(10, yPos);
  tft.setTextColor(HANIMAT_TEXT);
  tft.print("Preis:");
  tft.setCursor(150, yPos);
  tft.print(centsToEurStr(priceCents).c_str()); tft.println(" EUR");

  if (creditCents > 0) {
    yPos += lineSpacing;
    tft.setCursor(10, yPos);
    tft.setTextColor(HANIMAT_SUCCESS);
    tft.print("- Guthaben:");
    tft.setCursor(150, yPos);
    tft.print(centsToEurStr(creditCents).c_str()); tft.println(" EUR");
  }

  // Divider Line for Total
  yPos += 15;
  tft.drawFastHLine(10, yPos, 220, HANIMAT_DIVIDER);
  yPos += 25;

  tft.setCursor(10, yPos);
  tft.setTextColor(HANIMAT_ACCENT);
  tft.print("Zu zahlen:");
  tft.setCursor(150, yPos);
  tft.print(centsToEurStr(remainingCents).c_str()); tft.println(" EUR");

  // Status Bereich initialisieren
  tft.setTextColor(HANIMAT_TEXT);
  tft.setCursor(10, 200);
  tft.print("Bitte am Terminal folgen...");

  // 4. Zahlung bei SumUp-Server anmelden (NUR DEN RESTBETRAG in Cent)
  // Diagnose: Konfigurationswerte vor dem API-Aufruf loggen
  logf("SumUp: MerchantID='%s' ReaderID='%s' APIKey-Len=%d",
       sumupMerchantId.c_str(), sumupReaderId.c_str(), sumupApiKey.length());

  String trackingId;
  if (sumUp.startPayment(remainingCents, trackingId)) {
    // 5. ASYNCHRONEN ZUSTAND SETZEN
    isSumUpTransactionActive = true;
    currentSumUpTxId = trackingId;
    pendingSumUpAmountCents = remainingCents;
    sumUpStartTime = millis();
    lastSumUpCheckTime = 0; // Sofort prüfen

    logf("SumUp: Checkout API OK. Tracking-ID: %s. Warte im Loop auf Terminal...", trackingId.c_str());
    
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
 * @brief Zeichnet nur Zone C (Credit-Zeile, y=50..100) neu.
 *        Löscht den Bereich zuerst mit fillRect, dann neuer Text.
 */
void drawZoneCredit(char* buffer) {
  tft.fillRect(0, 50, 320, 50, HANIMAT_BG);
  int16_t x1, y1; uint16_t w, h;

  if (creditCents > 0) {
    // Kleines "GUTHABEN" Label oben links
    tft.setFont(&Poppins_Regular7pt7b);
    tft.setTextColor(HANIMAT_DIVIDER);
    tft.setCursor(10, 65);
    tft.print("GUTHABEN");

    // Großer Betrag — zentriert, grün, Poppins Black
    tft.setFont(&Poppins_Black14pt7b);
    tft.setTextColor(HANIMAT_SUCCESS);
    snprintf(buffer, 50, "%s EUR", centsToEurStr(creditCents).c_str());
    tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 94);
    tft.print(buffer);
  } else {
    // Subtiles "Kein Guthaben" zentriert in der Zone
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_DIVIDER);
    tft.getTextBounds("Kein Guthaben", 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 80);
    tft.print("Kein Guthaben");
  }
  lastDrawnCreditCents = creditCents;
}

/**
 * @brief Zeichnet nur die Status-Zeile (y=188..215) im Slot-Screen neu.
 *        "Bereit zum Kauf!" oder "Fehlt: X EUR"
 */
void drawZoneSlotStatus(char* buffer) {
  tft.fillRect(0, 188, 320, 20, HANIMAT_BG); // Zone T: y=188..208
  tft.setFont(&Poppins_Regular10pt7b);
  int16_t x1, y1; uint16_t w, h;

  if (creditCents >= slotPriceCents[selectedSlot]) {
    tft.setTextColor(HANIMAT_SUCCESS);
    tft.getTextBounds("Bereit zum Kauf!", 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 204);
    tft.print("Bereit zum Kauf!");
  } else {
    int missingCents = slotPriceCents[selectedSlot] - creditCents;
    snprintf(buffer, 50, "Fehlt: %s EUR", centsToEurStr(missingCents).c_str());
    tft.setTextColor(HANIMAT_ACCENT);
    tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 204);
    tft.print(buffer);
  }
}

/**
 * @brief Updates the TFT display based on the current system state.
 *        Zone-basiertes Partial-Redraw — kein fillScreen außer beim Mode-Wechsel.
 *
 *  Zonen-Layout (320×240, Rotation 1):
 *    H  Header    y=  0.. 50  (HANIMAT-Titel + Divider + WiFi-Icon)
 *    C  Credit    y= 50..100  (Guthaben / Kein Guthaben)
 *    D  Dynamic   y=100..188  (Divider + Fach-Info + Preis)
 *    T  Status    y=188..215  (Bereit / Fehlt)
 *    F  Footer    y=215..240  (Slogan + URL)
 */
void updateDisplayScreen() {
  char buffer[50];
  int16_t x1, y1; uint16_t w, h;

  // ── DISPENSING ────────────────────────────────────────────────────
  if (currentSystemState == CurrentSystemState::DISPENSING) {
    const int barX = 10, barY = 162, barW = 300, barH = 22;

    if (!dispensingScreenDrawn) {
      tft.fillScreen(HANIMAT_BG);
      drawPageHeader("VIELEN DANK", HANIMAT_SUCCESS);

      // Slot + Preis Info (kleine Zeile unter Header)
      char slotInfo[32];
      snprintf(slotInfo, sizeof(slotInfo), "Fach #%d  \xB7  %s EUR",
               dispenseJob.slot + 1,
               centsToEurStr(slotPriceCents[dispenseJob.slot]).c_str());
      tft.setFont(&Poppins_Regular7pt7b);
      tft.setTextColor(HANIMAT_DIVIDER);
      tft.getTextBounds(slotInfo, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 68);
      tft.print(slotInfo);

      // Haupttext
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
      tft.getTextBounds("Bitte Produkt entnehmen", 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 108);
      tft.print("Bitte Produkt entnehmen");

      // "Ausgabe läuft..." Label über Bar
      tft.setFont(&Poppins_Regular7pt7b);
      tft.setTextColor(HANIMAT_DIVIDER);
      tft.getTextBounds("Ausgabe lauft...", 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 148);
      tft.print("Ausgabe lauft...");

      // Bar-Rahmen (einmalig)
      tft.drawRoundRect(barX, barY, barW, barH, 5, HANIMAT_DIVIDER);

      dispensingScreenDrawn = true;
      lastDrawnMode = DrawnMode::DISPENSING;
    }

    unsigned long elapsed = millis() - dispenseJob.startTime;
    if (elapsed > DISPENSE_RELAY_ON_TIME) elapsed = DISPENSE_RELAY_ON_TIME;
    int fillW = (int)((unsigned long)elapsed * (barW - 2) / DISPENSE_RELAY_ON_TIME);
    if (fillW > 0) {
      tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, HANIMAT_SUCCESS);
    }
    return;
  }

  // ── MODE-WECHSEL von DISPENSING → NORMAL ─────────────────────────
  // Einmalig Vollaufbau wenn wir aus DISPENSING oder dem Nichts kommen
  dispensingScreenDrawn = false;
  bool modeJustChanged = (lastDrawnMode != DrawnMode::NORMAL);
  if (modeJustChanged) {
    tft.fillScreen(HANIMAT_BG);
    lastDrawnMode = DrawnMode::NORMAL;
    drawPageHeader("HANIMAT", HANIMAT_HEADER);
    // Zone F (y=208..240, 32px): Slogan Regular 10pt + Footer Regular 7pt.
    // Zone-Clears gehen nur bis y=208 → Zone F wird nie überschrieben.
    tft.fillRect(0, 208, 320, 32, HANIMAT_BG); // Zone F löschen
    if (displaySlogan.length() > 0) {
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
      tft.getTextBounds(displaySlogan, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 224); // Baseline y=224 → top ~y=209 ✓
      tft.print(displaySlogan);
    }
    tft.setFont(&Poppins_Regular7pt7b);
    tft.setTextColor(HANIMAT_HEADER); // Gelb
    tft.getTextBounds(displayFooter, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 237); // Baseline y=237 → top ~y=227 ✓
    tft.print(displayFooter);
    // Alle Zonen-Caches invalidieren damit sie neu gezeichnet werden
    lastDrawnSlot         = -2;
    lastDrawnCreditCents  = -1;
    lastDrawnKeypadBuffer = "!!INVALID!!";
  }

  if (currentSystemState == CurrentSystemState::ERROR_DISPLAY) return;

  // ── FACH AUSGEWÄHLT ──────────────────────────────────────────────
  if (selectedSlot != -1) {
    bool slotAvail = slotAvailable[selectedSlot];
    bool slotLock  = slotLocked[selectedSlot];

    bool slotChanged   = (selectedSlot != lastDrawnSlot ||
                          slotAvail    != lastDrawnSlotAvail ||
                          slotLock     != lastDrawnSlotLocked ||
                          modeJustChanged);
    bool creditChanged = (creditCents != lastDrawnCreditCents);

    if (!slotChanged && !creditChanged) return; // Nichts zu tun

    // Zone C: Credit — nur wenn Credit sich geändert hat oder Modus neu
    if (creditChanged || modeJustChanged) {
      drawZoneCredit(buffer);
    }

    // Zone D: Slot-Info als Karte — nur wenn Slot neu
    if (slotChanged) {
      tft.fillRect(0, 100, 320, 88, HANIMAT_BG);

      // Karten-Hintergrund + farbiger Rand je nach Status
      uint16_t cardBorder = (!slotLock && slotAvail) ? HANIMAT_ACCENT : HANIMAT_ERROR;
      tft.fillRoundRect(8, 104, 304, 78, 10, HANIMAT_CARD);
      tft.drawRoundRect(8, 104, 304, 78, 10, cardBorder);

      int16_t x1, y1; uint16_t w, h;

      // "FACH" kleines Label oben links in der Karte
      tft.setFont(&Poppins_Regular7pt7b);
      tft.setTextColor(HANIMAT_DIVIDER);
      tft.setCursor(22, 122);
      tft.print("FACH");

      // Slot-Nummer groß
      tft.setFont(&Poppins_Black14pt7b);
      tft.setTextColor(HANIMAT_HEADER);
      snprintf(buffer, sizeof(buffer), "%d", selectedSlot + 1);
      tft.setCursor(22, 160);
      tft.print(buffer);

      // Status / Preis — rechts ausgerichtet in der Karte
      tft.setFont(&Poppins_Regular10pt7b);
      if (slotLock) {
        tft.setTextColor(HANIMAT_ERROR);
        tft.getTextBounds("Gesperrt", 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(304 - w - 14, 155);
        tft.print("Gesperrt");
      } else if (!slotAvail) {
        tft.setTextColor(HANIMAT_ERROR);
        tft.getTextBounds("Leer", 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(304 - w - 14, 155);
        tft.print("Leer");
      } else {
        // "PREIS" Label
        tft.setFont(&Poppins_Regular7pt7b);
        tft.setTextColor(HANIMAT_DIVIDER);
        tft.getTextBounds("PREIS", 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(304 - w - 14, 122);
        tft.print("PREIS");
        // Preisbetrag
        tft.setFont(&Poppins_Regular10pt7b);
        tft.setTextColor(HANIMAT_ACCENT);
        snprintf(buffer, sizeof(buffer), "%s EUR", centsToEurStr(slotPriceCents[selectedSlot]).c_str());
        tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(304 - w - 14, 155);
        tft.print(buffer);
      }

      lastDrawnSlot       = selectedSlot;
      lastDrawnSlotAvail  = slotAvail;
      lastDrawnSlotLocked = slotLock;
    }

    // Zone T: Status (Bereit / Fehlt) — wenn Credit oder Slot sich geändert hat
    if ((slotChanged || creditChanged) && !slotLock && slotAvail) {
      drawZoneSlotStatus(buffer);
    }

    lastDrawnKeypadBuffer = "";
    return;
  }

  // ── TEILWEISE EINGABE ────────────────────────────────────────────
  if (keypadInputBuffer.length() > 0) {
    if (keypadInputBuffer == lastDrawnKeypadBuffer && !modeJustChanged) return;

    // Zone C + D neu zeichnen (Header bleibt stehen)
    tft.fillRect(0, 50, 320, 158, HANIMAT_BG); // C+D+T bis y=208, Zone F bleibt
    drawZoneCredit(buffer);
    tft.drawFastHLine(10, 112, 300, HANIMAT_DIVIDER);

    // Eingabe-Karte
    tft.fillRoundRect(8, 118, 304, 58, 10, HANIMAT_CARD);
    tft.drawRoundRect(8, 118, 304, 58, 10, HANIMAT_HEADER);

    // "FACH" Label klein oben links in der Karte
    tft.setFont(&Poppins_Regular7pt7b);
    tft.setTextColor(HANIMAT_DIVIDER);
    tft.setCursor(22, 135);
    tft.print("FACH");

    // Eingegebene Ziffer(n) groß + Cursor-Unterstrich
    String displayInput = keypadInputBuffer + "_";
    tft.setFont(&Poppins_Black14pt7b);
    tft.setTextColor(HANIMAT_HEADER);
    tft.setCursor(22, 165);
    tft.print(displayInput);

    // Hinweis-Text unterhalb der Karte
    int16_t x1, y1; uint16_t w, h;
    tft.setFont(&Poppins_Regular7pt7b);
    tft.setTextColor(HANIMAT_DIVIDER);
    tft.getTextBounds("2. Ziffer oder # bestaetigen", 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, 188);
    tft.print("2. Ziffer oder # bestaetigen");

    lastDrawnKeypadBuffer = keypadInputBuffer;
    lastDrawnSlot = -2;
    return;
  }

  // ── IDLE ─────────────────────────────────────────────────────────
  {
    bool idleContentChanged = ((creditCents > 0) != (lastDrawnCreditCents > 0));
    if (!modeJustChanged && lastDrawnSlot == -1 && !idleContentChanged &&
        creditCents == lastDrawnCreditCents) return;

    // Zone C + D neu zeichnen
    tft.fillRect(0, 50, 320, 158, HANIMAT_BG); // C+D+T bis y=208, Zone F bleibt
    drawZoneCredit(buffer);
    tft.drawFastHLine(10, 112, 300, HANIMAT_DIVIDER);

    int16_t x1, y1; uint16_t w, h;

    if (creditCents > 0) {
      // Hat Guthaben → Aufforderung Fach wählen
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
      tft.getTextBounds("Fach waehlen", 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 143);
      tft.print("Fach waehlen");

      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_SUCCESS);
      tft.getTextBounds("# zum Kaufen druecken", 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 171);
      tft.print("# zum Kaufen druecken");
    } else {
      // Kein Guthaben → Einwurf-Aufforderung
      tft.setFont(&Poppins_Regular10pt7b);
      tft.setTextColor(HANIMAT_TEXT);
      tft.getTextBounds("Geld einwerfen", 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 140);
      tft.print("Geld einwerfen");

      // Dünne Trennlinie
      tft.fillRect(120, 150, 80, 1, HANIMAT_DIVIDER);

      // Oder-Hinweis klein
      tft.setFont(&Poppins_Regular7pt7b);
      tft.setTextColor(HANIMAT_DIVIDER);
      snprintf(buffer, 50, "oder Fach 1\x96%d waehlen", activeSlots);
      tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor((tft.width() - w) / 2, 172);
      tft.print(buffer);
    }

    lastDrawnSlot         = -1;
    lastDrawnKeypadBuffer = "";
  }
}

/**
 * @brief Processes keypad input, updates buffer, and handles '#' and '*' keys.
 */
void processKeypad() {
  char key = manualGetKeyState();
  if (key == 0) return; // No new key press

  playKeyPressBeep();
  logf("Keypad: Processed Key: '%c'", key);
  lastUserInteractionTime = millis();
  currentSystemState = CurrentSystemState::USER_INTERACTION;

  if (isdigit(key)) {
    lastKeypadInputTime = millis();

    // Logic to handle 1 or 2-digit slot numbers
    if (keypadInputBuffer.length() >= 2) {
      keypadInputBuffer = ""; // Reset buffer if it's already full
    }
    keypadInputBuffer += key;
    logf("Keypad: Buffer updated to: %s", keypadInputBuffer.c_str());
    processKeypadSelection();

  } else if (key == '#') { // Confirm selection or purchase
    if (keypadInputBuffer.length() > 0) {
      logf("Keypad: '#' pressed. Finalizing selection from buffer: %s", keypadInputBuffer.c_str());
      processKeypadSelection();
    }
      
    if (selectedSlot != -1) {
      if (slotLocked[selectedSlot]) {
        displayErrorMessage("FACH " + String(selectedSlot + 1), "gesperrt!");
      } else if (!slotAvailable[selectedSlot]) {
        displayErrorMessage("FACH " + String(selectedSlot + 1), "ist leer!");
      } else if (creditCents >= slotPriceCents[selectedSlot]) {
        logf("Purchase attempt: Slot %d, Credit: %s EUR, Price: %s EUR.", selectedSlot + 1, centsToEurStr(creditCents).c_str(), centsToEurStr(slotPriceCents[selectedSlot]).c_str());
        scheduleDispense(selectedSlot, PaymentMethod::CASH);
      } else {
        int missingCents = slotPriceCents[selectedSlot] - creditCents;
        displayErrorMessage("FEHLT: " + centsToEurStr(missingCents) + " EUR", "bitte einwerfen");
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
  logf("processKeypadSelection: Buffer '%s', toInt: %d", keypadInputBuffer.c_str(), slotNum);

  if (slotNum >= 1 && slotNum <= activeSlots) {
    selectedSlot = slotNum - 1;
    logf("Keypad: Slot %d selected from buffer.", selectedSlot + 1);
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
      logf("Keypad: Selection '%s' is final. Clearing buffer.", keypadInputBuffer.c_str());
      keypadInputBuffer = "";
    } else {
      logMessage("Keypad: Waiting for second digit or '#' to confirm.");
    }

  } else {
    if (keypadInputBuffer.length() == 2) {
      // 2-stellige Eingabe komplett ungültig
      displayErrorMessage("FACH " + keypadInputBuffer, "ungueltig!");
      selectedSlot = -1;
      keypadInputBuffer = "";
    } else {
      // 1-stellige Eingabe ungültig (z.B. "0") — sofort leeren, kein stuck state
      logf("Keypad: Ungueltige Einzelziffer '%s'. Buffer geleert.", keypadInputBuffer.c_str());
      keypadInputBuffer = "";
      selectedSlot = -1;
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
    logf("ERROR: Invalid slot index for relay: %d", slot);
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
    logf("Relay for slot %d %s command sent successfully.", slot + 1, activate ? "ON" : "OFF");
    lastRelayChangeTime = millis();
    return true;
  } else {
    logf("ERROR: I2C failed for slot %d. Code: %d", slot + 1, error);
    return false;
  }
}

/**
 * @brief Fügt einen Verkaufseintrag in den RAM-Ringpuffer ein.
 *        Ältere Einträge werden bei vollem Puffer überschrieben.
 */
void addSaleLogEntry(int slot, int priceCents, PaymentMethod method) {
  SaleLogEntry& e = saleLog[saleLogNext];
  e.slot       = slot;
  e.priceCents = priceCents;
  e.method     = method;

  // Zeitstempel via NTP; Fallback auf Laufzeit in Sekunden
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    strftime(e.time, sizeof(e.time), "%d.%m. %H:%M:%S", &timeinfo);
  } else {
    snprintf(e.time, sizeof(e.time), "%lus", millis() / 1000UL);
  }

  saleLogNext = (saleLogNext + 1) % SALE_LOG_SIZE;
  if (saleLogCount < SALE_LOG_SIZE) saleLogCount++;
}

/**
 * @brief Initializes a dispense job for a given slot.
 * @param slotToDispense The slot index to be dispensed.
 */
void scheduleDispense(int slotToDispense, PaymentMethod method) {
  logf("scheduleDispense: Called for slot %d", slotToDispense + 1);
  if (slotToDispense < 0 || slotToDispense >= activeSlots) {
    logf("scheduleDispense: FEHLER: Ungültiger Slot-Index %d (aktive Fächer: %d)", slotToDispense, activeSlots);
    return;
  }
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
  dispenseJob.method = method;
  logf("Dispense job scheduled for slot %d", slotToDispense + 1);
  // Direkt auf DISPENSING setzen — processDispenseJob() aktiviert das Relay im nächsten Loop-Tick.
  // Kein Zwischenscreen: war nur für Millisekunden sichtbar und verursachte einen Flash.
  currentSystemState = CurrentSystemState::DISPENSING;
  displayNeedsUpdate = true;
}

/**
 * @brief Manages the active dispense job, from activating the relay to deactivating it after a timeout.
 */
void processDispenseJob() {
  if (!dispenseJob.active) return;

  unsigned long currentTime = millis();
  // State nur auf USER_INTERACTION setzen wenn wir noch nicht im DISPENSING-State sind
  if (currentSystemState != CurrentSystemState::DISPENSING) {
    currentSystemState = CurrentSystemState::USER_INTERACTION;
  }

  // --- Step 1: Activate Relay and Process Payment ---
  if (!dispenseJob.relayActivated) {
    digitalWrite(BILL_INHIBIT_PIN, HIGH); // Inhibit bill acceptor during dispense

    if (!controlSlotRelay(dispenseJob.slot, true)) {
      logf("processDispenseJob: ERROR activating relay for slot %d", dispenseJob.slot + 1);
      displayErrorMessage("RELAIS FEHLER", "Kauf abgebrochen");
      dispenseJob.active = false;
      digitalWrite(BILL_INHIBIT_PIN, LOW);
      resetDisplayToDefault();
      return;
    }
      
    // 1. Guthaben abziehen (Integer-Arithmetik, kein Rundungsfehler)
    creditCents -= slotPriceCents[dispenseJob.slot];
    if (creditCents < 0) creditCents = 0;

    // 2. Slot als leer markieren + Verkaufsstatistik aktualisieren
    slotAvailable[dispenseJob.slot] = false;
    slotSalesCount[dispenseJob.slot]++;
    totalRevenueCents += slotPriceCents[dispenseJob.slot];
    logf("Purchase complete for slot %d. New credit: %s EUR. Total revenue: %s EUR",
         dispenseJob.slot + 1, centsToEurStr(creditCents).c_str(), centsToEurStr(totalRevenueCents).c_str());

    // 3. Änderungen permanent im Flash speichern
    char availKey[12];
    snprintf(availKey, sizeof(availKey), "avail%d", dispenseJob.slot);
    char salesKey[12];
    snprintf(salesKey, sizeof(salesKey), "sales%d", dispenseJob.slot);

    // Alle Änderungen in einer einzigen NVS-Session schreiben (spart Flash-Öffnungs-Overhead)
    preferences.begin("hanimat", false);
    preferences.putBool(availKey, false);
    preferences.putInt(salesKey, slotSalesCount[dispenseJob.slot]);
    preferences.putInt("totalRev", totalRevenueCents);
    preferences.putInt("creditCts", creditCents);
    preferences.end();
    lastCreditSavedCents = creditCents; // saveCreditToNVS überspringt doppeltes Schreiben
    logf("NVS: Kauf + Guthaben gesichert (%s EUR)", centsToEurStr(creditCents).c_str());
    
    // Send notifications
    if (telegramNotifyOnSale) {
        String saleMessage = "🍯 VERKAUF: Fach #" + String(dispenseJob.slot + 1) + " wurde verkauft und ist jetzt leer.";
        sendTelegramMessage(saleMessage);
    }
    checkOverallStockLevel();
    addSaleLogEntry(dispenseJob.slot, slotPriceCents[dispenseJob.slot], dispenseJob.method);

    // State auf DISPENSING setzen — verhindert dass updateDisplayScreen() überschreibt
    currentSystemState = CurrentSystemState::DISPENSING;
    playThankYouMelody();

    // Mark step 1 as complete (BEVOR displayNeedsUpdate gesetzt wird)
    dispenseJob.relayActivated = true;
    dispenseJob.startTime = currentTime; // Reset timer for dispense duration
    displayNeedsUpdate = true; // updateDisplayScreen() zeichnet jetzt DISPENSING-Screen
  }

  // --- Step 2: Deactivate Relay after Timeout ---
  if (dispenseJob.relayActivated && (currentTime - dispenseJob.startTime >= DISPENSE_RELAY_ON_TIME)) {
    logf("Dispense time elapsed. Deactivating relay for slot %d", dispenseJob.slot + 1);
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

    logf("Münzprüfer: %d Pulse erkannt.", pulsesToProcess);

    // 2. MAPPING PRÜFEN (pulseValues Array)
    if (pulsesToProcess > 0 && pulsesToProcess < (sizeof(pulseValues) / sizeof(pulseValues[0]))) {
      int coinValueCents = pulseValues[pulsesToProcess];
      
      if (coinValueCents > 0) {
        // Guthaben im RAM erhöhen
        creditCents += coinValueCents;

        // Timer für die verzögerte Flash-Speicherung (Wear-Leveling)
        lastCreditChangeTime = millis();

        logf("Guthaben aktualisiert: +%s EUR", centsToEurStr(coinValueCents).c_str());
        
        // Display-Update anfordern und System-Status setzen
        displayNeedsUpdate = true;
        lastUserInteractionTime = millis();
        currentSystemState = CurrentSystemState::USER_INTERACTION;
        
        startBeep(1200, 40); // Non-blocking Münz-Feedback
      } else {
        logf("Münz-Fehler: Wert für %d Pulse ist 0.", pulsesToProcess);
      }
    } else {
      // Hilft beim Debugging: Zeigt an, wie viele Pulse bei Fehlern wirklich ankamen
      logf("Coin Fehler: %d Pulse passen zu keinem Mapping.", pulsesToProcess);
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
      logf("Bill: Pulses ignored (noise after relay action). Count: %d", billAcceptorPulseCount);
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

    logf("Bill: Processing %d pulses.", pulsesToProcess);

    if (pulsesToProcess > 0 && pulsesToProcess < (sizeof(billValues) / sizeof(billValues[0]))) {
      int billValueEuros = billValues[pulsesToProcess];
      if (billValueEuros > 0) {
        creditCents += billValueEuros * 100; // EUR → Cent
        logf("Bill accepted: %d pulses -> %d EUR. New credit: %s EUR", pulsesToProcess, billValueEuros, centsToEurStr(creditCents).c_str());
        
        lastCreditChangeTime = millis();

        displayNeedsUpdate = true;
        lastUserInteractionTime = millis();
        currentSystemState = CurrentSystemState::USER_INTERACTION;
        startBeep(1000, 150); // Non-blocking Schein-Feedback
      } else {
        logf("Bill: %d pulses has a value of 0.", pulsesToProcess);
      }
    } else {
      logf("Bill: Invalid pulse count rejected: %d", pulsesToProcess);
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

    drawPageHeader("HINWEIS", HANIMAT_ACCENT);

    int16_t x1, y1; uint16_t w, h;

    // Fehler-Karte mit rotem Rand
    tft.fillRoundRect(10, 58, 300, 110, 10, HANIMAT_CARD);
    tft.drawRoundRect(10, 58, 300, 110, 10, HANIMAT_ERROR);

    // Roter Akzentbalken oben in der Karte
    tft.fillRoundRect(10, 58, 300, 6, 4, HANIMAT_ERROR);

    // Zeile 1 — groß, rot
    tft.setFont(&Poppins_Regular10pt7b);
    tft.setTextColor(HANIMAT_ERROR);
    tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((tft.width() - w) / 2, line2.length() > 0 ? 105 : 120);
    tft.print(line1);

    // Zeile 2 — falls vorhanden, weiß
    if (line2.length() > 0) {
        tft.setFont(&Poppins_Regular10pt7b);
        tft.setTextColor(HANIMAT_TEXT);
        tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor((tft.width() - w) / 2, 138);
        tft.print(line2);
    }

    playErrorSound();
    displayNeedsUpdate = false;
    // Error hat fillScreen gemacht → Mode-Cache invalidieren damit danach Header+Footer neu gezeichnet werden
    lastDrawnMode = DrawnMode::NONE;
    // Non-blocking: Timeout wird im Loop() über errorDisplayActive geprüft
    errorDisplayActive = true;
    errorDisplayUntil = millis() + 4000; // 4s statt 3s — bessere Lesbarkeit
}

// ------------------------------------------
//         NEUE FUNKTIONEN: ONLINE UPDATE
// ------------------------------------------

void handleCheckOnlineUpdate() {
    if (!isAuth()) return;
    
    // Prüfen ob offline
    if (digitalRead(OFFLINE_MODE_PIN) == LOW) {
        server.send(500, "text/plain", "Fehler: Offline Modus aktiv.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure(); // TLS ohne Zertifikatsprüfung — Update-Server wird von uns betrieben
    client.setTimeout(15000);
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(15000);

    logf("Online-Update: Prüfe Version auf %s", UPDATE_VERSION_URL);

    if (http.begin(client, UPDATE_VERSION_URL)) {
        int httpCode = http.GET();
        if (httpCode == 200) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (!error) {
                String remoteVersion = doc["version"].as<String>();
                if (remoteVersion.length() > 0) {
                    String msg = String("Installiert: ") + FIRMWARE_VERSION + " | Online: " + remoteVersion;
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
            String errMsg = "HTTP Fehler " + String(httpCode) + ": " + http.errorToString(httpCode);
            logf("Online-Update Versionsprüfung: %s", errMsg.c_str());
            server.send(500, "text/plain", errMsg);
        }
        http.end();
    } else {
        server.send(500, "text/plain", "Verbindung zu " + String(UPDATE_VERSION_URL) + " fehlgeschlagen.");
    }
}

void handleStartOnlineUpdate() {
    if (!isAuth()) return;
    
    // Prüfen ob offline
    if (digitalRead(OFFLINE_MODE_PIN) == LOW) {
        server.send(500, "text/plain", "Fehler: Offline Modus aktiv.");
        return;
    }

    // Antwort senden bevor der Prozess startet (da er blockiert)
    server.send(200, "text/plain", "Update gestartet! Bitte warten, Gerät startet neu...");
    delay(500); 
    
    logf("Online-Update: Starte Download von %s", UPDATE_FIRMWARE_URL);
    
    lastDrawnMode = DrawnMode::NONE;
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
    client.setInsecure(); // TLS ohne Zertifikatsprüfung — Update-Server wird von uns betrieben
    client.setTimeout(30000); // 30s — Firmware-Download kann dauern

    // Globale Flag setzen um Loop zu pausieren
    otaUpdateInProgress = true;

    // Update durchführen (Blockiert bis fertig oder Fehler)
    t_httpUpdate_return ret = httpUpdate.update(client, UPDATE_FIRMWARE_URL);

    switch (ret) {
      case HTTP_UPDATE_FAILED: {
        String errDetail = httpUpdate.getLastErrorString();
        int    errCode   = httpUpdate.getLastError();
        logf("Online-Update FEHLER (%d): %s", errCode, errDetail.c_str());
        tft.setFont(&Poppins_Regular7pt7b);
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 190);
        tft.print("Fehler ");
        tft.print(errCode);
        tft.print(": ");
        tft.println(errDetail.substring(0, 30));
        // Fehler 4 Sekunden anzeigen – Web-Server dabei weiter bedienen
        { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
        otaUpdateInProgress = false;
        resetDisplayToDefault();
        break;
      }
        
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
  // Cookie-Header einlesen (nötig für Session-Token-Prüfung)
  const char* headerKeys[] = { "Cookie" };
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/resetcrashcount", HTTP_POST, handleResetCrashCount);
  server.on("/resetsalesstats", HTTP_POST, handleResetSalesStats);
  server.on("/setwifi", HTTP_POST, handleSetWifi);
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
  server.on("/saleslog", HTTP_GET, handleSalesLog);
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
    logf("HTTP 404: %s", server.uri().c_str());
  });

server.on("/savesumup", HTTP_POST, []() {
  if (!isAuth()) return;
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
  logf("Web: SumUp Einstellungen gespeichert. Timeout: %ds, Enabled: %d", timeoutSec, sumupEnabled);
  server.sendHeader("Location", "/#sumup-config", true);
  server.send(302);
});

server.on("/pairsumup", HTTP_POST, []() {
  if (!isAuth()) return;
  String code = server.arg("code");
  logf("Web: SumUp Pairing gestartet mit Code %s", code.c_str());
  String newId = sumUp.pairReader(code);
  if (newId != "") {
    sumupReaderId = newId;
    preferences.begin("hanimat", false);
    preferences.putString("suRid", newId);
    preferences.end();
    logf("Web: SumUp Pairing erfolgreich. Reader ID: %s", newId.c_str());
    server.send(200, "text/html", "Erfolg! ID: " + newId + " <meta http-equiv='refresh' content='2;url=/' />");
  } else {
    logMessage("Web: SumUp Pairing fehlgeschlagen.");
    server.send(200, "text/html", "Fehler! Code prüfen. <meta http-equiv='refresh' content='2;url=/' />");
  }
});

server.on("/disconnectsumup", HTTP_POST, []() {
  if (!isAuth()) return;
  
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
// Prüft ob der gespeicherte Reader noch auf SumUp's Servern existiert (GET-Request)
server.on("/checksumup", HTTP_GET, []() {
  if (!isAuth()) return;
  bool ok = sumUp.checkReader();
  String msg = ok
    ? "Reader OK! ID: " + sumupReaderId + " ist aktiv."
    : "Reader NICHT gefunden (404) — bitte neu koppeln. ID war: " + sumupReaderId;
  server.send(200, "text/html", msg + " <meta http-equiv='refresh' content='3;url=/' />");
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
  if (!isAuth()) {
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

  // --- Brute-Force: Sperre prüfen ---
  if (loginLockoutUntil > 0 && millis() < loginLockoutUntil) {
    unsigned long secsLeft = (loginLockoutUntil - millis()) / 1000;
    logf("Web: Login gesperrt – noch %lu s.", secsLeft);
    server.send(429, "text/plain",
      "Zu viele Fehlversuche. Bitte " + String(secsLeft) + " Sekunden warten.");
    return;
  }

  if (server.hasArg("password") && server.arg("password") == savedPassword) {
    // Erfolg → Zähler und Sperre zurücksetzen
    loginFailCount    = 0;
    loginLockoutUntil = 0;
    activeSessionToken = generateSessionToken();
    logf("Web: Login erfolgreich. Session: %s", activeSessionToken.c_str());
    server.sendHeader("Set-Cookie", "HANIMAT_SESSION=" + activeSessionToken + "; HttpOnly; Path=/");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  } else {
    loginFailCount++;
    logf("Web: Login fehlgeschlagen (%d/%d).", loginFailCount, LOGIN_MAX_FAILS);

    if (loginFailCount >= LOGIN_MAX_FAILS) {
      loginLockoutUntil = millis() + LOGIN_LOCKOUT_MS;
      loginFailCount    = 0; // Zurücksetzen – nach Ablauf der Sperre neu zählen
      logMessage("Web: Brute-Force erkannt! IP gesperrt für 5 Minuten.");
      if (telegramNotifyBruteForce) {
        sendTelegramMessage(
          "🚨 HANIMAT Sicherheitswarnung!\n"
          "Zu viele falsche Login-Versuche.\n"
          "Web-Interface für 5 Minuten gesperrt.\n"
          "Firmware: " + String(FIRMWARE_VERSION)
        );
      }
      server.send(429, "text/plain", "Zu viele Fehlversuche. Gesperrt für 5 Minuten.");
    } else {
      showLoginPage();
    }
  }
}

/**
 * @brief Loggt den Benutzer aus (löscht Session-Token + Cookie).
 */
void handleLogout() {
  activeSessionToken = "";
  // Cookie löschen durch MaxAge=0
  server.sendHeader("Set-Cookie", "HANIMAT_SESSION=deleted; HttpOnly; Path=/; Max-Age=0");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
  logMessage("Web: Logout.");
}

/**
 * @brief Setzt den Absturzzähler auf 0 zurück (NVS + RAM).
 */
void handleResetCrashCount() {
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  crashCount         = 0;
  wasUnexpectedReset = false;
  lastResetReason    = "Zurückgesetzt";
  preferences.begin("hanimat", false);
  preferences.putInt("crashCount", 0);
  preferences.end();
  logMessage("Web: Absturzzähler manuell zurückgesetzt.");
  server.send(200, "text/html", "Absturzzaehler zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/' />");
}

/**
 * @brief Setzt Gesamtumsatz und alle Fach-Verkaufszähler auf 0 (NVS + RAM).
 */
void handleResetSalesStats() {
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  totalRevenueCents = 0;
  for (int i = 0; i < activeSlots; i++) slotSalesCount[i] = 0;
  preferences.begin("hanimat", false);
  preferences.putInt("totalRev", 0);
  for (int i = 0; i < activeSlots; i++) {
    char key[12];
    snprintf(key, sizeof(key), "sales%d", i);
    preferences.putInt(key, 0);
  }
  preferences.end();
  // RAM-Ringpuffer ebenfalls leeren
  saleLogCount = 0;
  saleLogNext  = 0;
  logMessage("Web: Umsatz- und Verkaufsstatistik zurückgesetzt.");
  server.send(200, "text/html", "Statistik zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/#saleslog-section' />");
}

/**
 * @brief Speichert neue WLAN-Zugangsdaten in NVS und startet den ESP32 neu.
 *        In setup() werden die Daten ausgelesen und über WiFiManager gesetzt.
 */
void handleSetWifi() {
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  if (newSsid.length() == 0) {
    server.send(400, "text/plain", "SSID darf nicht leer sein.");
    return;
  }
  preferences.begin("hanimat", false);
  preferences.putString("wifi_ssid", newSsid);
  preferences.putString("wifi_pass", newPass);
  preferences.end();
  logf("Web: WLAN-Wechsel auf '%s' gespeichert. Starte neu...", newSsid.c_str());
  server.send(200, "text/html",
    "<p>WLAN wird gewechselt zu <b>" + newSsid + "</b>.<br>"
    "Der Automat verbindet sich neu. Falls das neue Netz nicht erreichbar ist, "
    "wechselt er automatisch in den Setup-Modus (AP: <b>HANIMAT-Setup</b>, PW: <b>Honig1234</b>).</p>"
    "<meta http-equiv='refresh' content='8;url=/'>"
  );
  delay(500);
  ESP.restart();
}

/**
 * @brief Handles the change password form submission.
 */
void handleChangePasswordWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
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
 * @brief Handles bulk update of all slot prices in one form submit.
 *        Erwartet Parameter price_0 … price_N (als EUR-Float-String, z.B. "5.10").
 */
void handleUpdatePriceWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }

  int changed = 0;
  char argName[12], nvKey[12];

  preferences.begin("hanimat", false);
  for (int i = 0; i < activeSlots; i++) {
    snprintf(argName, sizeof(argName), "price_%d", i);
    if (!server.hasArg(argName)) continue;

    int priceCents = (int)roundf(server.arg(argName).toFloat() * 100.0f);
    if (priceCents < 0) priceCents = 0;

    if (slotPriceCents[i] != priceCents) {
      slotPriceCents[i] = priceCents;
      snprintf(nvKey, sizeof(nvKey), "priceC%d", i);
      preferences.putInt(nvKey, priceCents);
      logf("Web: Preis Fach %d → %s EUR", i + 1, centsToEurStr(priceCents).c_str());
      changed++;
    }
  }
  preferences.end();

  logf("Web: Preise gespeichert (%d geändert).", changed);
  displayNeedsUpdate = true;
  server.sendHeader("Location", "/#slots-config", true);
  server.send(302, "text/plain", "");
}

/**
 * @brief Handles refilling a single slot.
 */
void handleRefillWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      if (!slotLocked[slot]) {
          slotAvailable[slot] = true;
          char nvKey[12]; snprintf(nvKey, sizeof(nvKey), "avail%d", slot);
          preferences.begin("hanimat", false);
          preferences.putBool(nvKey, true);
          preferences.end();
          logf("Web: Slot %d refilled.", slot + 1);
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
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("amount")) {
    int amountCents = (int)roundf(server.arg("amount").toFloat() * 100.0f);
    if (amountCents != 0) {
        creditCents += amountCents;
        saveCreditToNVS(true);
        logf("Web: Credit adjusted by %s EUR. New credit: %s EUR.", centsToEurStr(amountCents).c_str(), centsToEurStr(creditCents).c_str());
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
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  creditCents = 0;
  saveCreditToNVS(true);
  logMessage("Web: Credit reset to 0.");
  server.send(200, "text/html", "Guthaben zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/' />");
  displayNeedsUpdate = true;
}

/**
 * @brief Handles refilling all available (and not locked) slots.
 */
void handleRefillAllWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  char nvKey[12];
  preferences.begin("hanimat", false);
  for (int i = 0; i < activeSlots; i++) {
    if (!slotLocked[i]) {
        slotAvailable[i] = true;
        snprintf(nvKey, sizeof(nvKey), "avail%d", i);
        preferences.putBool(nvKey, true);
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
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  // Schutz: Kein Test während eine echte Ausgabe läuft
  if (dispenseJob.active) { server.send(409, "text/plain", "Ausgabe laeuft gerade – bitte warten."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      logf("Web: Testing relay for slot %d", slot + 1);
      // Non-blocking: Relais einschalten, processRelayTestJobs() schaltet nach 1s ab
      controlSlotRelay(slot, true);
      singleRelayTest = { true, slot, millis() };
      server.send(200, "text/html", String("Relais Fach ") + (slot+1) + " ausgeloest. <meta http-equiv='refresh' content='1;url=/' />");
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Triggers all relays in sequence for testing.
 */
void handleTriggerAllRelaysWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  // Schutz: Kein Test während eine echte Ausgabe läuft
  if (dispenseJob.active) { server.send(409, "text/plain", "Ausgabe laeuft gerade – bitte warten."); return; }
  logMessage("Web: Starte Relais-Sequenztest...");
  // Non-blocking: processRelayTestJobs() übernimmt die Sequenz
  controlSlotRelay(0, true);
  allRelaysTest = { true, 0, millis(), true };
  server.send(200, "text/html", "Relais-Test gestartet (laeuft im Hintergrund). <meta http-equiv='refresh' content='1;url=/' />");
}

/**
 * @brief Sets static IP configuration and reboots.
 */
void handleSetStaticIPWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }

  String ip   = server.arg("static_ip");
  String gw   = server.arg("gateway");
  String sn   = server.arg("subnet");
  String dns  = server.arg("dns1");

  preferences.begin("hanimat", false);
  if (ip.length() == 0) {
    // Leeres IP-Feld → DHCP-Modus: statische Einträge löschen
    preferences.remove("static_ip");
    preferences.remove("gateway");
    preferences.remove("subnet");
    preferences.remove("dns1");
    logMessage("Web: Netzwerk auf DHCP umgestellt. Neustart erforderlich.");
  } else {
    // Statische IP speichern
    preferences.putString("static_ip", ip);
    preferences.putString("gateway",   gw.length()  > 0 ? gw  : "192.168.1.1");
    preferences.putString("subnet",    sn.length()  > 0 ? sn  : "255.255.255.0");
    preferences.putString("dns1",      dns.length() > 0 ? dns : "8.8.8.8");
    logf("Web: Statische IP gespeichert: %s / GW %s. Neustart erforderlich.", ip.c_str(), gw.c_str());
  }
  preferences.end();

  // Antwort senden, dann ESP neu starten (delay damit HTTP-Response rausgeht)
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='8;url=/'>"
    "<style>body{font-family:sans-serif;background:#0F1115;color:#fff;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
    ".box{text-align:center;padding:2rem;background:#1C1F26;border-radius:16px;border:1px solid #2D3139;}"
    "h2{color:#FF9F1C;}p{color:#9CA3AF;}</style></head><body>"
    "<div class='box'><h2>&#128190; Gespeichert</h2>"
    "<p>Netzwerkeinstellungen wurden gespeichert.<br>Der Automat startet neu...</p>"
    "<p style='font-size:0.8rem;margin-top:1rem;'>Weiterleitung in 8 Sekunden.</p></div>"
    "</body></html>"
  );
  delay(500);
  ESP.restart();
}

/**
 * @brief Updates the total number of active slots.
 */
void handleUpdateSlotsWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("maxSlots")) {
    int newNumSlots = server.arg("maxSlots").toInt();
    if (newNumSlots > 0 && newNumSlots <= MAX_SLOTS) {
      activeSlots = newNumSlots;
      char nvKey[12];
      preferences.begin("hanimat", false);
      preferences.putInt("activeSlots", activeSlots);
      // Initialize new slots if they don't exist in preferences
      for(int i = 0; i < activeSlots; i++) {
          snprintf(nvKey, sizeof(nvKey), "avail%d", i);
          if(!preferences.isKey(nvKey)) {
              slotAvailable[i] = true;
              preferences.putBool(nvKey, true);
          }
          snprintf(nvKey, sizeof(nvKey), "priceC%d", i);
          if(!preferences.isKey(nvKey)) {
              slotPriceCents[i] = 500; // Default 5,00 EUR
              preferences.putInt(nvKey, 500);
          }
      }
      preferences.end();
      logf("Web: Number of active slots set to %d", activeSlots);
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
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      slotLocked[slot] = !slotLocked[slot];
      char nvKey[12]; snprintf(nvKey, sizeof(nvKey), "locked%d", slot);
      preferences.begin("hanimat", false);
      preferences.putBool(nvKey, slotLocked[slot]);
      preferences.end();
      logf("Web: Slot %d %s", slot + 1, slotLocked[slot] ? "locked." : "unlocked.");
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
  if (!isAuth()) {
    server.send(401, "text/plain", "Not authorized.");
    return;
  }

  String logContent = "";
  int startIdx = logIndex;
  for (int i = 0; i < MAX_LOG_LINES; i++) {
    int currentReadPos = (startIdx + i) % MAX_LOG_LINES;
    if (logBuffer[currentReadPos][0] != '\0') { // Slot belegt?
      logContent += logBuffer[currentReadPos];
      logContent += '\n';
    }
  }
  server.send(200, "text/plain", logContent);
}

/**
 * @brief Liefert den RAM-Ringpuffer der Verkäufe als JSON-Array.
 *        Ältester Eintrag zuerst (chronologische Reihenfolge).
 *        Format: [{"time":"dd.MM. HH:mm:ss","slot":1,"price":"1.50","method":"CASH"}, ...]
 */
void handleSalesLog() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) {
    server.send(401, "text/plain", "Not authorized.");
    return;
  }

  String json = "[";
  // Ringpuffer in chronologischer Reihenfolge ausgeben
  int startIdx = (saleLogCount < SALE_LOG_SIZE) ? 0 : saleLogNext;
  for (int i = 0; i < saleLogCount; i++) {
    int idx = (startIdx + i) % SALE_LOG_SIZE;
    const SaleLogEntry& e = saleLog[idx];
    if (i > 0) json += ",";
    json += "{\"time\":\"";
    json += e.time;
    json += "\",\"slot\":";
    json += String(e.slot + 1);
    json += ",\"price\":\"";
    json += centsToEurStr(e.priceCents);
    json += "\",\"method\":\"";
    json += (e.method == PaymentMethod::SUMUP) ? "SUMUP" : "CASH";
    json += "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}


// --- OTA Update Handlers ---

/**
 * @brief Displays the OTA update page.
 */
void handleOTAUpdatePage() {
  if (!isAuth()) {
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
  if (!isAuth()) { return; }
  lastActivityTimeWeb = millis();
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUpdateInProgress = true;
    otaStatusMessage = "Upload started... Writing firmware.";
    logf("OTA: Upload started: %s", upload.filename.c_str());
    displayOTAMessageTFT("Update gestartet", "Nicht ausschalten!", "", ILI9341_ORANGE);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      logf("OTA ERROR: Update.begin() failed. Error: %d", Update.getError());
      otaStatusMessage = "ERROR: Could not start update (Error: " + String(Update.getError()) + ")";
      displayOTAMessageTFT("Update Fehler!", "Start fehlgeschlagen", "Details im Log", ILI9341_RED);
      otaUpdateInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      logf("OTA ERROR: Update.write() failed. Error: %d", Update.getError());
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
            logf("OTA ERROR: Update.end() failed. Error: %d", Update.getError());
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
    if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();

    preferences.begin("hanimat", false);
    COIN_PROCESSING_DELAY            = (unsigned long)server.arg("coin_delay").toInt();
    BILL_ISR_DEBOUNCE_MS             = (unsigned long)server.arg("bill_isr_debounce").toInt();
    BILL_GROUP_PROCESSING_TIMEOUT_MS = (unsigned long)server.arg("bill_group_timeout").toInt();
    DISPENSE_RELAY_ON_TIME           = (unsigned long)server.arg("disp_time").toInt();
    KEYPAD_INPUT_TIMEOUT             = (unsigned long)server.arg("keypad_time").toInt();
    SLOT_SELECTION_TIMEOUT           = (unsigned long)server.arg("slot_sel_time").toInt();
    DISPLAY_TIMEOUT                  = (unsigned long)server.arg("disp_timeout").toInt();
    unsigned long webToutSec = (unsigned long)server.arg("web_timeout").toInt();
    if (webToutSec < 30) webToutSec = 30;
    WEB_TIMEOUT = webToutSec * 1000UL;
    statusEnabled = server.hasArg("status_enabled");

    preferences.putULong("coinDelay",      COIN_PROCESSING_DELAY);
    preferences.putULong("billIsrDeb",     BILL_ISR_DEBOUNCE_MS);
    preferences.putULong("billGrpTout",    BILL_GROUP_PROCESSING_TIMEOUT_MS);
    preferences.putULong("dispTime",       DISPENSE_RELAY_ON_TIME);
    preferences.putULong("keypadTime",     KEYPAD_INPUT_TIMEOUT);
    preferences.putULong("slotSelTime",    SLOT_SELECTION_TIMEOUT);
    preferences.putULong("dispTimeout",    DISPLAY_TIMEOUT);
    preferences.putULong("webTout",        WEB_TIMEOUT);
    preferences.putBool("statusEnabled",   statusEnabled);
    preferences.end();

    logMessage("Web: Zeiteinstellungen gespeichert und sofort übernommen.");
    otaStatusMessage = "Zeiteinstellungen gespeichert! Neustart empfohlen.";
    server.sendHeader("Location", "/#timing-config", true);
    server.send(302, "text/plain", "");
}

/**
 * @brief Handles Telegram and stock notification form submission.
 */
void handleSaveTelegramConfig() {
    if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();

    telegramEnabled           = server.hasArg("tg_enabled");
    telegramBotToken          = server.arg("tg_token");
    telegramChatId            = server.arg("tg_chat_id");
    telegramNotifyOnSale      = server.hasArg("notify_sale");
    telegramNotifyAlmostEmpty = server.hasArg("notify_almost_empty");
    telegramNotifyEmpty       = server.hasArg("notify_empty");
    telegramNotifyCrash       = server.hasArg("notify_crash");
    telegramNotifyBruteForce  = server.hasArg("notify_bruteforce");
    almostEmptyThreshold      = server.arg("almost_empty_threshold").toInt();

    preferences.begin("hanimat", false);
    preferences.putBool("tgEnabled",      telegramEnabled);
    preferences.putString("tgToken",      telegramBotToken);
    preferences.putString("tgChatId",     telegramChatId);
    preferences.putInt("tgAlmostThres",   almostEmptyThreshold);
    preferences.putBool("tgNotifySale",   telegramNotifyOnSale);
    preferences.putBool("tgNotifyAlmost", telegramNotifyAlmostEmpty);
    preferences.putBool("tgNotifyEmpty",  telegramNotifyEmpty);
    preferences.putBool("tgNotifyCrash",  telegramNotifyCrash);
    preferences.putBool("tgNotifyBrute",  telegramNotifyBruteForce);
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
    if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
    lastActivityTimeWeb = millis();
    String message = String("👋 Hallo vom HANIMAT! Dies ist eine Testnachricht. Alles scheint zu funktionieren. Version: ") + FIRMWARE_VERSION;
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
  if (!isAuth()) { server.send(401); return; }

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
  lastActivityTimeWeb = millis();
  lastDrawnMode = DrawnMode::NONE; // Vollaufbau erzwingen damit Slogan/Footer neu gezeichnet wird
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
  String html;
  html.reserve(90000); // Heap-Fragmentierung minimieren: 51x += ohne Reallokation
  html = R"HTML(
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
.stat-heap-ok   { color: var(--success); }
.stat-heap-warn { color: #f39c12; }
.stat-heap-crit { color: var(--danger); }

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

/* --- TIMING CONFIG TOOLTIPS --- */
.timing-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 1.5rem; }
.timing-label { display: flex; align-items: center; gap: 6px; color: var(--text-sec); font-size: 0.85rem; margin-bottom: 0.5rem; flex-wrap: wrap; }
.timing-label .unit { color: #555; font-size: 0.78rem; }
.info-icon { display: inline-flex; align-items: center; justify-content: center; width: 17px; height: 17px; border-radius: 50%; background: rgba(255,159,28,0.15); color: var(--brand); font-size: 10px; font-weight: 700; cursor: help; position: relative; flex-shrink: 0; font-style: normal; line-height: 1; user-select: none; }
.info-icon .tip { display: none; position: absolute; bottom: calc(100% + 8px); left: 50%; transform: translateX(-50%); background: #1e2128; border: 1px solid var(--border); color: var(--text-main); font-size: 0.78rem; font-weight: 400; padding: 10px 13px; border-radius: 10px; width: 240px; z-index: 200; line-height: 1.5; text-align: left; box-shadow: 0 6px 24px rgba(0,0,0,0.5); pointer-events: none; white-space: normal; }
.info-icon .tip::after { content: ''; position: absolute; top: 100%; left: 50%; transform: translateX(-50%); border: 6px solid transparent; border-top-color: var(--border); }
.info-icon:hover .tip, .info-icon.open .tip { display: block; }
.timing-section-title { font-size: 0.75rem; font-weight: 700; color: #555; text-transform: uppercase; letter-spacing: 1px; margin: 1.5rem 0 1rem 0; border-top: 1px solid var(--border); padding-top: 1.2rem; }
.timing-section-title:first-child { margin-top: 0; border-top: none; padding-top: 0; }
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
    <li><button class='nav-btn' onclick='go("saleslog-section")'><span class='nav-icon'>&#128202;</span> Verkaufsstatistik</button></li>
    <li><button class='nav-btn' onclick='go("logs")'><span class='nav-icon'>&#128466;</span> Logs</button></li>
    <li><button class='nav-btn' onclick='go("ota-update-section")'><span class='nav-icon'>&#128229;</span> Update</button></li>
  </ul>
<div class='footer-info'>
    FW )HTML"; html += FIRMWARE_VERSION; html += R"HTML(<br>
    Hanimat<br>
    <a href='https://www.hanimat.at' target='_blank' style='color:#666; text-decoration:none;'>www.hanimat.at</a><br>
    <a href='/logout' style='color:#c00; text-decoration:none; font-weight:600;'>&#x23FB; Abmelden</a>
  </div>
</nav>

<!-- Content -->
<div class='main'>

  <!-- DASHBOARD -->
  <section id='dashboard' class='page'>
    <div class='top-bar'><h1>Dashboard</h1></div>

    <div class='stats-grid'>
      <div class='stat-box'>
        <div class='stat-val stat-highlight'>)HTML"; html += centsToEurStr(creditCents) + R"HTML( &euro;</div>
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
      <div class='stat-box'>
        <div class='stat-val )HTML";
// Heap-Farbe: grün > 60KB, gelb 30–60KB, rot < 30KB
uint32_t heapNow = ESP.getFreeHeap();
if      (heapNow >= 60000) html += "stat-heap-ok";
else if (heapNow >= 30000) html += "stat-heap-warn";
else                        html += "stat-heap-crit";
html += R"HTML('>)HTML"; html += String(heapNow / 1024) + R"HTML( KB</div>
        <div class='stat-lbl'>Freier Heap (Min: )HTML"; html += String(ESP.getMinFreeHeap()/1024) + R"HTML( KB)</div>
      </div>
      <div class='stat-box'>
        <div class='stat-val )HTML";
html += (wasUnexpectedReset ? "stat-heap-crit" : "stat-heap-ok");
html += R"HTML(' style='font-size:0.95rem; word-break:break-word;'>)HTML";
html += lastResetReason + R"HTML(</div>
        <div class='stat-lbl'>Letzter Neustart &nbsp;|&nbsp; Absturz: )HTML";
html += String(crashCount) + R"HTML(x</div>
        )HTML";
if (crashCount > 0) {
  html += R"HTML(<form action='/resetcrashcount' method='post' style='margin-top:0.6rem;'>
          <button type='submit' style='font-size:0.75rem; padding:4px 10px; background:transparent; border:1px solid #c00; color:#c00; border-radius:6px; cursor:pointer;'>&#x21BA; Zähler zurücksetzen</button>
        </form>)HTML";
}
html += R"HTML(
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
    html += "<div><div class='slot-title'>Fach #" + String(i+1) + "</div><div class='slot-price'>" + centsToEurStr(slotPriceCents[i]) + " &euro;</div></div>";
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

    <!-- Schnell-Befüllung: Alle auf gleichen Preis -->
    <div style='display:flex; gap:10px; align-items:flex-end; margin-bottom:1.5rem; flex-wrap:wrap;'>
      <div class='input-group' style='margin:0; flex:1; min-width:140px; max-width:200px;'>
        <label style='font-size:0.8rem;'>Gleicher Preis für alle</label>
        <input type='number' id='bulkPrice' step='0.01' min='0' placeholder='z.B. 5.00' style='margin:0;'>
      </div>
      <button type='button' onclick='applyBulkPrice()' class='btn-sec' style='height:46px; white-space:nowrap;'>&#128256; Alle übernehmen</button>
    </div>

    <!-- Kompakt-Grid aller Fächer -->
    <form action='/updateprice' method='post' id='priceForm'>
    <div style='display:grid; grid-template-columns:repeat(auto-fill, minmax(130px,1fr)); gap:8px; margin-bottom:1.5rem;'>
)HTML";
  for (int i = 0; i < activeSlots; i++) {
    String dotColor = slotLocked[i] ? "#6B7280" : (slotAvailable[i] ? "var(--success)" : "var(--danger)");
    html += "<div style='background:var(--card); border:1px solid var(--border); border-radius:10px; padding:10px 12px;'>";
    html += "<div style='display:flex; align-items:center; gap:6px; margin-bottom:6px;'>";
    html += "<span style='width:8px;height:8px;border-radius:50%;background:" + dotColor + ";flex-shrink:0;'></span>";
    html += "<span style='font-size:0.78rem; color:var(--text-sec); font-weight:600;'>Fach #" + String(i+1) + "</span>";
    html += "</div>";
    html += "<div style='display:flex; align-items:center; gap:4px;'>";
    html += "<input type='number' step='0.01' min='0' name='price_" + String(i) + "' value='" + centsToEurStr(slotPriceCents[i]) + "' style='margin:0; padding:6px 8px; font-size:0.9rem; width:100%;'>";
    html += "<span style='font-size:0.8rem; color:var(--text-sec); flex-shrink:0;'>€</span>";
    html += "</div></div>";
  }
  html += R"HTML(
    </div>
    <button type='submit' class='btn-main'>&#128190; Alle Preise Speichern</button>
    </form>

    <script>
    function applyBulkPrice() {
      const v = document.getElementById('bulkPrice').value;
      if (!v || isNaN(parseFloat(v))) return;
      const val = parseFloat(v).toFixed(2);
      document.querySelectorAll('#priceForm input[type=number]').forEach(el => el.value = val);
    }
    </script>
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
    <div class='top-bar'><h1>Zeitsteuerung</h1></div>
    <div class='stat-box'>
      <form action='/savetimingconfig' method='post'>

        <p class='timing-section-title'>&#127981; Münzakzeptor</p>
        <div class='timing-grid'>
          <div class='input-group'>
            <div class='timing-label'>
              Münzeinwurf Verzögerung
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Wartezeit nach dem Einwurf einer Münze, bevor die Impulse ausgewertet werden. Bei Rauschproblemen oder Fehlzählungen erhöhen. Standardwert: 150 ms</span></i>
            </div>
            <input type='number' name='coin_delay' min='0' max='5000' value=')HTML" + String(COIN_PROCESSING_DELAY) + R"HTML('>
          </div>
        </div>

        <p class='timing-section-title'>&#128181; Scheinakzeptor</p>
        <div class='timing-grid'>
          <div class='input-group'>
            <div class='timing-label'>
              Entprellzeit Scheineinwurf
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Mindestabstand zwischen zwei Impuls-Flanken des Scheinprüfers. Verhindert Doppelzählungen durch Prellen. Standardwert: 75 ms</span></i>
            </div>
            <input type='number' name='bill_isr_debounce' min='0' max='500' value=')HTML" + String(BILL_ISR_DEBOUNCE_MS) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Scheingruppen Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Wartezeit nach dem letzten Impuls eines Scheins, bis der Gesamtwert verarbeitet wird. Muss größer sein als die Lücke zwischen den Impulsen eines Scheins. Standardwert: 1500 ms</span></i>
            </div>
            <input type='number' name='bill_group_timeout' min='100' max='10000' value=')HTML" + String(BILL_GROUP_PROCESSING_TIMEOUT_MS) + R"HTML('>
          </div>
        </div>

        <p class='timing-section-title'>&#9881; Ausgabe &amp; Bedienung</p>
        <div class='timing-grid'>
          <div class='input-group'>
            <div class='timing-label'>
              Relais Ausgabezeit
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Wie lange das Ausgabe-Relais aktiviert bleibt (= Motorlaufzeit). Zu kurz &rarr; Produkt wird nicht ausgegeben. Standardwert: 5000 ms</span></i>
            </div>
            <input type='number' name='disp_time' min='500' max='30000' value=')HTML" + String(DISPENSE_RELAY_ON_TIME) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Tastatureingabe Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Wartezeit auf eine zweite Ziffer nach dem ersten Tastendruck. Standardwert: 3000 ms</span></i>
            </div>
            <input type='number' name='keypad_time' min='500' max='15000' value=')HTML" + String(KEYPAD_INPUT_TIMEOUT) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Fachauswahl Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Wie lange ein gew&auml;hltes Fach aktiv bleibt, ohne dass der Kauf best&auml;tigt wird. Standardwert: 10000 ms</span></i>
            </div>
            <input type='number' name='slot_sel_time' min='2000' max='60000' value=')HTML" + String(SLOT_SELECTION_TIMEOUT) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Display Ruhemodus
              <span class='unit'>ms</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Nach dieser Inaktivit&auml;tsdauer kehrt das Display automatisch zum Startbildschirm zur&uuml;ck. Standardwert: 20000 ms</span></i>
            </div>
            <input type='number' name='disp_timeout' min='5000' max='300000' value=')HTML" + String(DISPLAY_TIMEOUT) + R"HTML('>
          </div>
        </div>

        <p class='timing-section-title'>&#128274; Web-Interface</p>
        <div class='timing-grid'>
          <div class='input-group'>
            <div class='timing-label'>
              Sitzungs Timeout
              <span class='unit'>Sekunden</span>
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Nach dieser Zeit ohne Aktivit&auml;t im Web-Interface wird die Anmeldung automatisch beendet. Standardwert: 600 s</span></i>
            </div>
            <input type='number' name='web_timeout' min='30' max='86400' value=')HTML" + String(WEB_TIMEOUT / 1000) + R"HTML('>
          </div>
        </div>

        <p class='timing-section-title'>&#128200; System</p>
        <label class='check-row'>
          <input type='checkbox' name='status_enabled' )HTML" + String(statusEnabled ? "checked" : "") + R"HTML(>
          <div>
            <div style='font-weight:600;'>&#128268; Status-Ping aktiviert</div>
            <div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Sendet alle 60 Minuten einen anonymen Heartbeat an hanimat.at. Wird f&uuml;r System&uuml;berwachung genutzt.</div>
          </div>
        </label>

        <button type='submit' class='btn-main' style='margin-top:2rem;'>Einstellungen Speichern</button>
      </form>
    </div>
  </section>

  <!-- CONFIG TELEGRAM -->
  <section id='telegram-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Telegram</h1></div>
    <div class='stat-box'>
      <form action='/savetelegramconfig' method='post'>
        <div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:1.5rem;'>
          <label class='check-row' style='margin-bottom:0; flex-shrink:0;'>
            <input type='checkbox' name='tg_enabled' )HTML" + String(telegramEnabled ? "checked" : "") + R"HTML(>
            <b>Telegram Integration Aktivieren</b>
          </label>
          <a href='https://hanimat.at/telegram.html' target='_blank' style='color:var(--brand); text-decoration:none; font-size:0.8rem; background:rgba(255,159,28,0.1); padding:5px 12px; border-radius:8px; border:1px solid rgba(255,159,28,0.3);'>
            &#128214; Setup-Anleitung
          </a>
        </div>
        <p class='timing-section-title'>&#128272; Bot-Zugangsdaten</p>
        <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap:1.5rem;'>
          <div class='input-group'>
            <div class='timing-label'>Bot Token
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Den Token erh&auml;ltst du vom @BotFather auf Telegram.</span></i>
            </div>
            <input type='password' name='tg_token' value=')HTML" + telegramBotToken + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>Chat ID
              <i class='info-icon' onclick='this.classList.toggle("open")'>i<span class='tip'>Die numerische ID deines Chats. Mit @userinfobot herausfinden.</span></i>
            </div>
            <input type='text' name='tg_chat_id' value=')HTML" + telegramChatId + R"HTML('>
          </div>
        </div>
        <p class='timing-section-title'>&#128276; Benachrichtigungen</p>
        <label class='check-row'>
          <input type='checkbox' name='notify_sale' )HTML" + String(telegramNotifyOnSale ? "checked" : "") + R"HTML(>
          <div><div style='font-weight:600;'>&#127815; Verkauf</div><div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Nachricht bei jedem erfolgreichen Kauf</div></div>
        </label>
        <label class='check-row' style='margin-top:0.5rem;'>
          <input type='checkbox' name='notify_almost_empty' )HTML" + String(telegramNotifyAlmostEmpty ? "checked" : "") + R"HTML(>
          <div><div style='font-weight:600;'>&#9888;&#65039; Fast leer</div><div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Warnung wenn die Restmenge den Schwellwert erreicht</div></div>
        </label>
        <div style='margin-left:44px; margin-top:0.4rem; margin-bottom:0.8rem;'>
          <div class='timing-label' style='margin-bottom:0.4rem;'>Schwellwert (St&uuml;ck)</div>
          <input type='number' name='almost_empty_threshold' min='1' max='50' value=')HTML" + String(almostEmptyThreshold) + R"HTML(' style='max-width:140px;'>
        </div>
        <label class='check-row' style='margin-top:0.5rem;'>
          <input type='checkbox' name='notify_empty' )HTML" + String(telegramNotifyEmpty ? "checked" : "") + R"HTML(>
          <div><div style='font-weight:600;'>&#128683; Fach leer</div><div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Meldung wenn ein Fach vollst&auml;ndig leer ist</div></div>
        </label>
        <label class='check-row' style='margin-top:0.5rem;'>
          <input type='checkbox' name='notify_crash' )HTML" + String(telegramNotifyCrash ? "checked" : "") + R"HTML(>
          <div><div style='font-weight:600;'>&#128680; Absturz / Neustart</div><div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Benachrichtigung bei unerwartetem Neustart</div></div>
        </label>
        <label class='check-row' style='margin-top:0.5rem;'>
          <input type='checkbox' name='notify_bruteforce' )HTML" + String(telegramNotifyBruteForce ? "checked" : "") + R"HTML(>
          <div><div style='font-weight:600;'>&#128272; Brute-Force Angriff</div><div style='font-size:0.8rem; color:var(--text-sec); margin-top:2px;'>Benachrichtigung bei zu vielen fehlerhaften Login-Versuchen</div></div>
        </label>
        <button type='submit' class='btn-main' style='margin-top:1.5rem;'>Einstellungen Speichern</button>
      </form>
      <form action='/sendtesttelegram' method='post' style='margin-top:1.5rem; border-top:1px solid var(--border); padding-top:1.5rem;'>
        <p style='color:var(--text-sec); font-size:0.85rem; margin:0 0 1rem 0;'>Sendet eine Test-Nachricht an die oben eingetragene Chat ID.</p>
        <button type='submit' class='btn-sec'>&#9992;&#65039; Test-Nachricht Senden</button>
      </form>
    </div>
  </section>

  <!-- CONFIG NETWORK -->
  )HTML";
  preferences.begin("hanimat", false);
  String staticIP_val = preferences.getString("static_ip", "");
  String gateway_val  = preferences.getString("gateway", "");
  String subnet_val   = preferences.getString("subnet", "");
  String dns1_val     = preferences.getString("dns1", "8.8.8.8");
  preferences.end();
  html += R"HTML(
  <section id='network-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Netzwerk</h1></div>
    <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap:1rem; margin-bottom:1.5rem;'>
      <div class='stat-box' style='padding:1rem;'>
        <div class='stat-lbl'>IP-Adresse</div>
        <div style='font-size:1.1rem; font-weight:700; color:var(--success); margin-top:4px;'>)HTML" + WiFi.localIP().toString() + R"HTML(</div>
      </div>
      <div class='stat-box' style='padding:1rem;'>
        <div class='stat-lbl'>Modus</div>
        <div style='font-size:1.1rem; font-weight:700; margin-top:4px; color:)HTML" + String(staticIP_val.length() > 0 ? "var(--brand)" : "var(--success)") + R"HTML(;'>)HTML" + String(staticIP_val.length() > 0 ? "Statisch" : "DHCP") + R"HTML(</div>
      </div>
      <div class='stat-box' style='padding:1rem;'>
        <div class='stat-lbl'>WLAN-Netz</div>
        <div style='font-size:1rem; font-weight:700; margin-top:4px;'>)HTML" + WiFi.SSID() + R"HTML(</div>
      </div>
      <div class='stat-box' style='padding:1rem;'>
        <div class='stat-lbl'>Signal</div>
        <div style='font-size:1.1rem; font-weight:700; margin-top:4px;'>)HTML" + String(WiFi.RSSI()) + R"HTML( dBm</div>
      </div>
      <div class='stat-box' style='padding:1rem;'>
        <div class='stat-lbl'>MAC-Adresse</div>
        <div style='font-size:0.85rem; font-weight:600; margin-top:4px; color:var(--text-sec);'>)HTML" + WiFi.macAddress() + R"HTML(</div>
      </div>
    </div>
    <div class='stat-box'>
      <form action='/setstaticip' method='post' id='netForm'>
        <p class='timing-section-title' style='margin-top:0;'>&#127758; IP-Konfiguration</p>
        <div style='display:flex; gap:0.75rem; margin-bottom:1.5rem;'>
          <button type='button' id='btnDhcp' onclick='setNetMode(false)'
            style='flex:1; padding:0.75rem; border-radius:10px; border:2px solid; cursor:pointer; font-weight:700; font-size:0.9rem; transition:0.2s;'>
            &#127760; DHCP
          </button>
          <button type='button' id='btnStatic' onclick='setNetMode(true)'
            style='flex:1; padding:0.75rem; border-radius:10px; border:2px solid; cursor:pointer; font-weight:700; font-size:0.9rem; transition:0.2s;'>
            &#128204; Statische IP
          </button>
        </div>
        <div id='staticFields'>
          <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap:1.5rem;'>
            <div class='input-group'><div class='timing-label'>IP-Adresse</div>
              <input type='text' id='inp_static_ip' name='static_ip' placeholder='z.B. 192.168.1.100' value=')HTML" + staticIP_val + R"HTML('></div>
            <div class='input-group'><div class='timing-label'>Gateway</div>
              <input type='text' name='gateway' placeholder='z.B. 192.168.1.1' value=')HTML" + gateway_val + R"HTML('></div>
            <div class='input-group'><div class='timing-label'>Subnetzmaske</div>
              <input type='text' name='subnet' placeholder='255.255.255.0' value=')HTML" + subnet_val + R"HTML('></div>
            <div class='input-group'><div class='timing-label'>DNS-Server</div>
              <input type='text' name='dns1' placeholder='8.8.8.8' value=')HTML" + dns1_val + R"HTML('></div>
          </div>
        </div>
        <div style='display:flex; gap:1rem; margin-top:1.5rem; flex-wrap:wrap;'>
          <button type='submit' class='btn-main' style='flex:1; min-width:180px;'>&#128190; Speichern &amp; Neustart</button>
        </div>
        <p style='color:var(--text-sec); font-size:0.8rem; margin:0.8rem 0 0 0;'>&#9888;&#65039; Nach dem Speichern startet der Automat neu.</p>
      </form>
    </div>

    <!-- WLAN wechseln -->
    <div class='stat-box' style='margin-top:1rem;'>
      <p class='timing-section-title' style='margin-top:0;'>&#128246; WLAN wechseln</p>
      <p style='color:var(--text-sec); font-size:0.82rem; margin:0 0 1.2rem 0;'>
        Aktuell verbunden mit: <b>)HTML" + WiFi.SSID() + R"HTML(</b><br>
        Nach dem Speichern verbindet sich der Automat mit dem neuen Netz und startet neu.<br>
        Falls das Netz nicht erreichbar ist, wechselt er automatisch in den Setup-Modus<br>
        (AP: <b>HANIMAT-Setup</b> &middot; Passwort: <b>Honig1234</b>).
      </p>
      <form action='/setwifi' method='post' onsubmit='return confirm("WLAN wirklich wechseln? Das Gerät startet neu.");'>
        <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap:1.5rem; margin-bottom:1.2rem;'>
          <div class='input-group'>
            <div class='timing-label'>SSID (Netzwerkname)</div>
            <input type='text' name='ssid' placeholder='Mein WLAN' required autocomplete='off'>
          </div>
          <div class='input-group'>
            <div class='timing-label'>Passwort</div>
            <input type='password' name='pass' placeholder='••••••••' autocomplete='new-password'>
          </div>
        </div>
        <button type='submit' class='btn-main' style='width:auto;'>&#128246; Verbinden &amp; Neustart</button>
      </form>
    </div>
  </section>
  <script>
  function setNetMode(useStatic) {
    var sf=document.getElementById('staticFields'), btnD=document.getElementById('btnDhcp'), btnS=document.getElementById('btnStatic'), ipInp=document.getElementById('inp_static_ip');
    if(useStatic){
      sf.style.display='';
      btnS.style.background='var(--brand)';btnS.style.color='#000';btnS.style.borderColor='var(--brand)';
      btnD.style.background='transparent';btnD.style.color='var(--text-sec)';btnD.style.borderColor='var(--border)';
    }else{
      sf.style.display='none';
      if(ipInp)ipInp.value='';
      btnD.style.background='var(--success)';btnD.style.color='#000';btnD.style.borderColor='var(--success)';
      btnS.style.background='transparent';btnS.style.color='var(--text-sec)';btnS.style.borderColor='var(--border)';
    }
  }
  setNetMode()HTML" + String(staticIP_val.length() > 0 ? "(true)" : "(false)") + R"HTML();
  </script>

  <!-- CONFIG PASSWORD -->
  <section id='password-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>Sicherheit</h1></div>
    <div class='stat-box' style='max-width:500px;'>
      <form action='/changepassword' method='post'>
         <div class='input-group'>
            <label>Neues Admin Passwort</label>
            <input type='password' name='newPassword' required minlength='4' placeholder='****'>
         </div>
         <button type='submit' class='btn-main'>Passwort &auml;ndern</button>
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
         <div id='online-update-status' style='margin-bottom:1rem; color:var(--brand);'>Status: Warte auf Pr&uuml;fung...</div>
         <div style='display:flex; gap:1rem;'>
            <button onclick='checkOnlineUpdate()' class='btn-sec' style='width:auto;'>Version Pr&uuml;fen</button>
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
          <button type='submit' class='btn-main'>Upload &amp; Flash</button>
       </form>
    </div>
  </section>

  <!-- VERKAUFSSTATISTIK -->
  <section id='saleslog-section' class='page' style='display:none;'>
    <div class='top-bar'><h1>&#128202; Verkaufsstatistik</h1></div>

    <div class='stats-grid' style='margin-bottom:1.5rem;'>
      <div class='stat-box'>
        <div class='stat-val stat-highlight'>)HTML"; html += centsToEurStr(totalRevenueCents) + R"HTML( &euro;</div>
        <div class='stat-lbl'>Gesamtumsatz (gespeichert)</div>
      </div>
      <div class='stat-box'>
        <div class='stat-val'>)HTML";
        { int total = 0; for(int i=0;i<activeSlots;i++) total+=slotSalesCount[i]; html += String(total); }
        html += R"HTML(</div>
        <div class='stat-lbl'>Verkäufe gesamt (gespeichert)</div>
      </div>
    </div>

    <div class='stat-box'>
      <p style='color:var(--text-sec); font-size:0.82rem; margin:0 0 1rem 0;'>&#9888;&#65039; Die folgende Liste wird nur im Arbeitsspeicher gehalten und geht bei einem Neustart verloren.</p>
      <div style='overflow-x:auto;'>
        <table id='sales-table' style='width:100%; border-collapse:collapse; font-size:0.88rem;'>
          <thead>
            <tr style='color:var(--text-sec); border-bottom:1px solid var(--border);'>
              <th style='text-align:left; padding:6px 10px; font-weight:600;'>#</th>
              <th style='text-align:left; padding:6px 10px; font-weight:600;'>Zeit</th>
              <th style='text-align:left; padding:6px 10px; font-weight:600;'>Fach</th>
              <th style='text-align:right; padding:6px 10px; font-weight:600;'>Preis</th>
              <th style='text-align:left; padding:6px 10px; font-weight:600;'>Zahlung</th>
            </tr>
          </thead>
          <tbody id='sales-tbody'>
            <tr><td colspan='5' style='padding:1rem; color:var(--text-sec); text-align:center;'>Lade Daten...</td></tr>
          </tbody>
        </table>
      </div>
      <div id='sales-empty' style='display:none; text-align:center; color:var(--text-sec); padding:2rem; font-size:0.9rem;'>&#128683; Noch keine Verk&auml;ufe seit dem letzten Neustart.</div>
    </div>

    <div class='stat-box' style='margin-top:1rem; border-top:1px solid var(--border); padding-top:1rem;'>
      <p style='color:var(--text-sec); font-size:0.82rem; margin:0 0 1rem 0;'>Setzt Gesamtumsatz, alle Fach-Z&auml;hler und die Session-Liste dauerhaft auf 0 zur&uuml;ck.</p>
      <form action='/resetsalesstats' method='post' onsubmit='return confirm("Alle Verkaufsstatistiken wirklich löschen?");'>
        <button type='submit' class='btn-sec' style='color:var(--danger); border-color:var(--danger);'>&#128465; Statistik zur&uuml;cksetzen</button>
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
          <div style='display:flex; gap:10px; flex-wrap:wrap;'>
            <form action='/checksumup' method='get'>
               <button type='submit' class='btn-sec'>&#128268; Reader pr&uuml;fen</button>
            </form>
            <form action='/disconnectsumup' method='post'>
               <button type='submit' class='btn-sec' style='color:var(--danger); border-color:var(--danger);'>Entkoppeln (Reset)</button>
            </form>
          </div>
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
  document.querySelectorAll('.page').forEach(p => p.style.display = 'none');
  const target = document.getElementById(id);
  if(target) target.style.display = 'block';
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.nav-btn').forEach(btn => {
     if(btn.getAttribute('onclick').includes(id)) btn.classList.add('active');
  });
  if(window.innerWidth <= 768) toggleMenu();
  if(id === 'logs') fetchLogs();
  if(id === 'saleslog-section') fetchSalesLog();
}

document.addEventListener('click', function(e) {
  if (!e.target.closest('.info-icon'))
    document.querySelectorAll('.info-icon.open').forEach(el => el.classList.remove('open'));
});

function fetchLogs(){
  const con = document.getElementById('log-output');
  if(!con) return;
  fetch('/logdata').then(r=>r.text()).then(t => { con.textContent = t; con.scrollTop = con.scrollHeight; });
}

function fetchSalesLog(){
  fetch('/saleslog')
    .then(r => r.json())
    .then(data => {
      const tbody = document.getElementById('sales-tbody');
      const empty = document.getElementById('sales-empty');
      if(!tbody) return;
      if(!data || data.length === 0) {
        tbody.innerHTML = '';
        if(empty) empty.style.display = 'block';
        return;
      }
      if(empty) empty.style.display = 'none';
      let html = '';
      data.forEach((e, i) => {
        const rowBg = i % 2 === 0 ? 'rgba(255,255,255,0.03)' : 'transparent';
        const payBadge = e.method === 'SUMUP'
          ? '<span style="background:rgba(0,180,120,0.15);color:#00b478;padding:2px 8px;border-radius:6px;font-size:0.78rem;font-weight:700;">SumUp</span>'
          : '<span style="background:rgba(255,159,28,0.15);color:var(--brand);padding:2px 8px;border-radius:6px;font-size:0.78rem;font-weight:700;">Bar</span>';
        html += '<tr style="border-bottom:1px solid var(--border);background:' + rowBg + ';">';
        html += '<td style="padding:8px 10px;color:var(--text-sec);">' + (i+1) + '</td>';
        html += '<td style="padding:8px 10px;">' + e.time + '</td>';
        html += '<td style="padding:8px 10px;">Fach #' + e.slot + '</td>';
        html += '<td style="padding:8px 10px;text-align:right;font-weight:700;color:var(--success);">' + e.price + ' &euro;</td>';
        html += '<td style="padding:8px 10px;">' + payBadge + '</td>';
        html += '</tr>';
      });
      tbody.innerHTML = html;
    })
    .catch(() => {
      const tbody = document.getElementById('sales-tbody');
      if(tbody) tbody.innerHTML = '<tr><td colspan="5" style="padding:1rem;color:var(--danger);text-align:center;">Fehler beim Laden.</td></tr>';
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
 */
int countAvailableSlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

/**
 * @brief Counts the number of slots that are empty (sold out) and not locked.
 */
int countEmptySlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (!slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

/**
 * @brief Checks overall stock level and sends Telegram notifications at thresholds.
 */
void checkOverallStockLevel() {
  int totalAvailable = countAvailableSlots();

  if (telegramNotifyEmpty && totalAvailable == 0) {
    if (!emptyNotificationSent) {
      String message = "ð¨ ALARM: Der HANIMAT ist komplett ausverkauft! Bitte auffÃ¼llen! ð­";
      sendTelegramMessage(message);
      emptyNotificationSent = true;
      almostEmptyNotificationSent = true;
      logMessage("Telegram: Alarm 'Ausverkauft' gesendet.");
    }
  } else if (telegramNotifyAlmostEmpty && totalAvailable > 0 && totalAvailable <= almostEmptyThreshold) {
    if (!almostEmptyNotificationSent) {
      String message = "â ï¸ INFO: Der HANIMAT ist fast leer!\nVearfÃ¼gbare FÃ¤cher: " + String(totalAvailable);
      sendTelegramMessage(message);
      almostEmptyNotificationSent = true;
      logMessage("Telegram: Info 'Fast leer' gesendet (" + String(totalAvailable) + " Ã¼brig).");
    }
  } else if (totalAvailable > almostEmptyThreshold) {
    if (almostEmptyNotificationSent || emptyNotificationSent) {
      logMessage("Bestand wieder ok (" + String(totalAvailable) + "). Flags zurÃ¼ckgesetzt.");
    }
    almostEmptyNotificationSent = false;
    emptyNotificationSent = false;
  }
}

/**
 * @brief Monitors free heap and sends a Telegram warning if it drops below threshold.
 *        Runs at HEAP_CHECK_INTERVAL; resets warning flag when heap recovers.
 */
void checkHeapMonitor() {
  if (millis() - lastHeapCheckTime < HEAP_CHECK_INTERVAL) return;
  lastHeapCheckTime = millis();

  uint32_t freeHeap = ESP.getFreeHeap();
  logf("Heap: %u bytes frei (Min: %u)", freeHeap, ESP.getMinFreeHeap());

  if (freeHeap < HEAP_WARN_THRESHOLD) {
    if (!heapWarningSent) {
      String msg = "â ï¸ HANIMAT Heap-Warnung: Nur noch " + String(freeHeap / 1024) + " KB frei. Neustart empfohlen.";
      sendTelegramMessage(msg);
      heapWarningSent = true;
      logMessage("Heap-Warnung gesendet.");
    }
  } else {
    heapWarningSent = false; // Reset sobald Heap sich erholt
  }
}

/**
 * @brief Sends a periodic status ping to hanimat.at.
 *        Respects the hardware offline switch and statusEnabled flag.
 */
void sendHanimatStatusPing() {
  if (digitalRead(OFFLINE_MODE_PIN) == LOW) return;
  if (!statusEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (dispenseJob.active || currentSystemState != CurrentSystemState::IDLE) {
    lastStatusPing = millis() - (statusInterval - 60000UL);
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setConnectTimeout(1500);
  http.setTimeout(1500);

  String chipId = String((uint32_t)ESP.getEfuseMac(), HEX);
  chipId.toUpperCase();
  String url = String(statusServerUrl) + "?id=" + chipId + "&key=" + statusApiKey + "&v=" + FIRMWARE_VERSION;

  logMessage("Status: Sende Heartbeat...");
  if (http.begin(client, url)) {
    int code = http.GET();
    if      (code == 200) logMessage("Status: OK (200)");
    else if (code >  0)   logf("Status: Server Fehler %d", code);
    else                  logMessage("Status: Timeout/Netzwerkfehler");
    http.end();
  } else {
    logMessage("Status: Verbindung fehlgeschlagen");
  }
  lastStatusPing = millis();
}
