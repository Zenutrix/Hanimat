/**
 * @file main.cpp
 * @author Thomas Schöpf / Hanimat
 * @brief Firmware für die HANIMAT Verkaufsmaschine basierend auf der ESP32 Plattform.
 * @version 1.5.3
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
#include <Adafruit_ILI9341.h> // bindet Adafruit_GFX/SPITFT selbst ein (Pixeltransport fuer LVGL)
#include <WiFi.h>
#include <WiFiClientSecure.h> // Für sichere HTTPS-Verbindungen zu Telegram
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <Update.h>
#include <HTTPUpdate.h> // Für Online-Updates
#include <UniversalTelegramBot.h> // Telegram-Bot-Bibliothek
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>

// =================================================================
//  LOGGING-SYSTEM
// =================================================================
#define MAX_LOG_LINES    48
#define MAX_LOG_LINE_LEN 100  // Zeichen pro Eintrag (inkl. Zeitstempel)
// Fixer char-Array statt String-Array: kein Heap-Overhead, keine Fragmentierung
char logBuffer[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
int  logIndex = 0;

/**
 * @brief Schreibt den Zeitstempel ("dd.mm. HH:MM:SS", Fallback Sekunden seit
 *        Boot) in den Puffer - gemeinsamer Formatierer für alle Logs.
 */
static void formatLogTimestamp(char* out, size_t outSize) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {  // 0ms Timeout — nicht blockierend
    strftime(out, outSize, "%d.%m. %H:%M:%S", &timeinfo);
  } else {
    snprintf(out, outSize, "%lus", millis() / 1000UL);
  }
}

/**
 * @brief Interne Log-Kernfunktion — schreibt Zeitstempel + Nachricht in den
 *        Ring-Puffer (kein Heap-Alloc). Wird von logMessage() und logf() genutzt.
 */
static void _logWrite(const char* msg) {
  char timeString[20];
  formatLogTimestamp(timeString, sizeof(timeString));
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

// =================================================================
//  EREIGNIS-LOG (persistent auf LittleFS) — wichtige Ereignisse ca. 3 Tage
//  zurueckverfolgbar. Fuer Routine-Meldungen weiterhin logMessage()/logf().
// =================================================================
const char*  EVENT_LOG_FILE           = "/eventlog.txt";
const char*  EVENT_LOG_FILE_OLD       = "/eventlog_old.txt";
const size_t EVENT_LOG_MAX_FILE_SIZE  = 8192; // 8 KB je Datei (aktuell + alt = max. ~16 KB)
const size_t EVENT_LOG_MIN_FREE_SPACE = 8192; // 8 KB Reserve — Sicherheitsnetz gegen vollen Speicher

/**
 * @brief Haengt eine Zeile ans Ereignis-Log an, mit Groessen-Rotation und
 *        Speicherplatz-Absicherung (opfert noetigenfalls die aelteste Datei).
 */
static void _persistEventLog(const String &msg) {
  size_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeSpace < EVENT_LOG_MIN_FREE_SPACE && LittleFS.exists(EVENT_LOG_FILE_OLD)) {
    LittleFS.remove(EVENT_LOG_FILE_OLD); // Selbstheilung: aeltestes Log freigeben
    freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  }
  if (freeSpace < EVENT_LOG_MIN_FREE_SPACE) return; // Sicherheitsnetz: Dateisystem nicht volllaufen lassen

  char timeString[20];
  formatLogTimestamp(timeString, sizeof(timeString));
  String line = "[" + String(timeString) + "] " + msg + "\n";

  size_t currentSize = 0;
  if (LittleFS.exists(EVENT_LOG_FILE)) {
    File check = LittleFS.open(EVENT_LOG_FILE, "r");
    if (check) { currentSize = check.size(); check.close(); }
  }

  if (currentSize + line.length() > EVENT_LOG_MAX_FILE_SIZE) {
    if (LittleFS.exists(EVENT_LOG_FILE_OLD)) LittleFS.remove(EVENT_LOG_FILE_OLD);
    if (LittleFS.exists(EVENT_LOG_FILE))     LittleFS.rename(EVENT_LOG_FILE, EVENT_LOG_FILE_OLD);
  }

  File f = LittleFS.open(EVENT_LOG_FILE, "a");
  if (f) { f.print(line); f.close(); }
}

/**
 * @brief Wie logMessage(), aber zusaetzlich dauerhaft im Ereignis-Log gespeichert.
 */
void logEvent(const String &msg) {
  logMessage(msg);
  _persistEventLog(msg);
}

/**
 * @brief Wie logf(), aber zusaetzlich dauerhaft im Ereignis-Log gespeichert.
 */
void logEventf(const char* fmt, ...) {
  // Eigener, groesserer Puffer (200 statt 100 Zeichen) — mit dem kleinen
  // logf()-Puffer wurden lange Meldungen sonst still abgeschnitten.
  char buf[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logMessage(String(buf));
  _persistEventLog(String(buf));
}


#include "SumUpController.h" // SumUp Klasse muss im selben Ordner liegen



// =================================================================
//                      FIRMWARE-VERSION
// =================================================================
const char* FIRMWARE_VERSION = "V1.5.3";
const char* FS_VERSION       = "V1.5.3-fs1";

// =================================================================
//                      KONFIGURATIONS-KONSTANTEN
// =================================================================

// --- Online-Update-Konfiguration ---
const char* UPDATE_VERSION_URL  = "https://www.hanimat.at/DAT/update/version.json";
const char* UPDATE_FIRMWARE_URL = "https://www.hanimat.at/DAT/update/firmware.bin";
const char* UPDATE_FS_URL       = "https://www.hanimat.at/DAT/update/littlefs.bin";

// --- LittleFS Update Sicherheit ---
// Installierte LittleFS-Version (aus NVS) — für Update-Vergleich
String installedFsVersion = "";

// --- Verkaufsautomat-Konfiguration ---
#define RELAYS_PER_EXPANDER 16
#define NUM_EXPANDERS       8
const int MAX_SLOTS = RELAYS_PER_EXPANDER * NUM_EXPANDERS; // 128 Fächer max

// --- Zeit- und Timeout-Werte (in Millisekunden) ---
unsigned long COIN_PROCESSING_DELAY = 120;
unsigned long BILL_ISR_DEBOUNCE_MS = 75;
unsigned long BILL_GROUP_PROCESSING_TIMEOUT_MS = 1500;
unsigned long DISPENSE_RELAY_ON_TIME = 5000;
unsigned long KEYPAD_INPUT_TIMEOUT = 3000;
unsigned long WEB_TIMEOUT = 600000;
unsigned long SLOT_SELECTION_TIMEOUT = 10000;
unsigned long DISPLAY_TIMEOUT = 20000;
const unsigned long STARTUP_IGNORE_BILL_TIME = 5000; // Scheinimpulse beim Start kurz ignorieren

// --- Hardware-Pin-Definitionen ---
#define TFT_CS    26
#define TFT_DC    4
#define TFT_RST   16
#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_MISO  -1 // MISO nicht verwendet

#define COIN_ACCEPTOR_PIN 5
#define BILL_ACCEPTOR_PIN 32
#define BILL_INHIBIT_PIN 33
#define WIFI_RESET_BUTTON 34
#define RELAY_I2C_ADDRESS 0x20
#define BUZZER_PIN 25
#define OFFLINE_MODE_PIN 27
#define SUMUP_BUTTON_PIN 0

// --- Zahlungs-Mapping ---
// Ordnet die Pulsanzahl einem Cent-Wert für Münzen zu. Index = Pulsanzahl.
// Editierbar via Web-Interface, gespeichert in NVS ("coinPulses").
int pulseValues[7] = {0, 0, 10, 20, 50, 100, 200}; // 0..6 pulses → Cent

// Ordnet die Pulsanzahl einem Euro-Wert für Scheine zu. Index = Pulsanzahl.
// Editierbar via Web-Interface, gespeichert in NVS ("billPulses").
int billValues[17] = {
//Impulse: 0, 1, 2, 3, 4, 5, 6, 7, 8,  9, 10, 11, 12, 13, 14, 15, 16
          0, 0, 0, 0, 5, 0, 0, 0, 10, 0,  0,  0,  0,  0,  0,  0,  20
};

// --- Zahlungskanal-Aktivierung ---
bool coinAcceptorEnabled = true;   // Münzprüfer aktiv
bool billAcceptorEnabled = true;   // Scheinprüfer aktiv

// --- Sicherheit ---
const String DEFAULT_PASSWORD = "admin"; // Standard-Passwort für die Weboberfläche

// --- System-Status ---
enum class CurrentSystemState {
  IDLE,             // Grundzustand, wartet auf Benutzerinteraktion
  USER_INTERACTION, // Benutzer interagiert über Tastatur oder Zahlung
  ERROR_DISPLAY,    // Fehlermeldung wird angezeigt
  OTA_UPDATE,       // OTA-Update läuft
  SUMUP_PENDING,    // Wartet auf SumUp-Zahlung
  DISPENSING,       // Relay aktiv, Ausgabe läuft — Display zeigt VIELEN DANK
  PICKUP_PIN_ENTRY  // Abholfach gewaehlt, wartet auf PIN-Code-Eingabe
};
CurrentSystemState currentSystemState = CurrentSystemState::IDLE;

// =================================================================
//                      GLOBALE VARIABLEN
// =================================================================

// --- SumUp Konfiguration ---
String sumupApiKey = "";
String sumupMerchantId = "";
String sumupReaderId = "";
bool sumupEnabled = false;
unsigned long sumupTimeout = 80000; // Millisekunden (Default 80s, mehr Puffer fuer SumUp-History-Verzoegerung)

// Controller Instanz
SumUpController sumUp("", "", "");

// --- SumUp Asynchrone Status Variablen ---
bool isSumUpTransactionActive = false;
String currentSumUpTxId = "";
int pendingSumUpAmountCents = 0; // Zu zahlender Betrag in Cent (bei Erfolg 1:1 gutgeschrieben, NICHT neu aus dem Fachpreis berechnet)
int pendingSumUpPriceCents  = 0; // Fach-Preis zum Zeitpunkt des Zahlungsstarts (Snapshot fuer Umsatz/Kassenstand — unabhaengig von spaeteren Preisaenderungen)
bool pendingSumUpWasMixed   = false; // true = beim Start der Kartenzahlung war bereits Bar-Guthaben vorhanden (Bar+Karte)
unsigned long sumUpStartTime = 0;
unsigned long lastSumUpCheckTime = 0;


// --- Timing & Status-Tracking ---
unsigned long slotSelectedTime = 0;
unsigned long bootTime = 0;
unsigned long lastRelayChangeTime = 0;
unsigned long lastUserInteractionTime = 0;

// --- Webserver & Speicher ---
WebServer server(80);
Preferences preferences;

// --- Relais-Steuerung ---
static uint16_t expanderOutputStates[NUM_EXPANDERS]; // Bitmaske pro Expander

// --- Tastatur-Konfiguration ---
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 3;
char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[KEYPAD_ROWS] = {15, 14, 12, 17}; // ESP32 GPIO-Pins für Tastatur-Zeilen
byte colPins[KEYPAD_COLS] = {2, 19, 13};  // ESP32 GPIO-Pins für Tastatur-Spalten

// --- Tastatur-Status ---
char lastPhysicallyPressedKey = 0;
char lastReturnedKey = 0;
unsigned long lastKeyPressTime = 0;
const unsigned long KEYPAD_DEBOUNCE_PERIOD = 50; // Entprellzeit für die Tastatur

// --- Display ---
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);


// --- Fach-Daten ---
int slotPriceCents[MAX_SLOTS]; // Preise in Cent (z.B. 500 = 5,00 EUR)
bool slotAvailable[MAX_SLOTS];
bool slotLocked[MAX_SLOTS];
int activeSlots = MAX_SLOTS;

// --- Abholfach-Daten ---
// Heap-allokiert statt statisches Array: dram0_0_seg (~124 KB) ist fast voll
// (~176 Byte frei), der Heap hat dagegen reichlich Platz. Groesse `activeSlots`
// wird erst in setup() bekannt.
bool  *slotIsPickup = nullptr;    // true = Abholfach (kein Preis, PIN-Code statt Kauf)
char (*slotPinCode)[7] = nullptr; // "" = kein Code hinterlegt, sonst 4-6 Ziffern + Nullterminator

/**
 * @brief Abholfach ohne hinterlegten Code = faktisch leer. Eine gemeinsame
 *        Definition fuer Keypad-Logik und Display, damit beide nie auseinanderlaufen.
 */
inline bool isPickupSlotEmpty(int slot) {
  return slotIsPickup[slot] && strlen(slotPinCode[slot]) == 0;
}

// --- Telegram-Benachrichtigungs-Konfiguration ---
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

// --- Hanimat-Status-Netzwerk ---
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
int cashBoxCents        = 0;          // Kassenstand: nur Bar-Einnahmen seit letztem Reset
int totalRevenueCents   = 0;          // Gesamtumsatz in Cent
int slotSalesCount[MAX_SLOTS] = {0};  // Verkaufsanzahl pro Fach

// --- Brute-Force Schutz ---
int           loginFailCount    = 0;
unsigned long loginLockoutUntil = 0;
const int     LOGIN_MAX_FAILS   = 5;
const unsigned long LOGIN_LOCKOUT_MS = 5UL * 60UL * 1000UL; // 5 Minuten

// --- Heap-Überwachung ---
unsigned long lastHeapCheckTime = 0;
const unsigned long HEAP_CHECK_INTERVAL = 60000; // 60 Sekunden
const uint32_t HEAP_WARN_THRESHOLD     = 30000;  // Warnung unter 30 KB
bool heapWarningSent = false;                     // Damit nicht jede Minute gewarnt wird

// Flag zur Erkennung eines offenen Pins
bool resetPinIsFloating = false; 

// --- Zahlung & Guthaben ---
// Guthaben und Preise werden intern in CENT (Integer) gespeichert,
// um Gleitkomma-Präzisionsfehler bei Geldbeträgen zu vermeiden.
int creditCents = 0;

// Sicherheitsnetz: Guthaben darf nie ueber diesen Wert steigen (faengt Bugs und
// Fehleingaben ab). Im Webif einstellbar (Einstellungen > Guthaben), Standard 100 EUR.
int maxCreditCents = 10000;

// Maximaler Betrag pro manuellem "Guthaben hinzufuegen"-Vorgang. Ebenfalls im
// Webif einstellbar, Standard 50 EUR.
int maxTopUpCents = 5000;

// Telegram-Warnung, wenn das Guthaben eine einstellbare Schwelle ueberschreitet
// (Einstellung auf der Telegram-Seite im Webif, unabhaengig vom Sicherheitsnetz oben).
bool telegramNotifyCreditThreshold = false;
int  creditWarnThresholdCents      = 5000; // Standard 50 EUR
bool creditWarnSent                = false; // verhindert Mehrfach-Meldung, solange ueberschritten

String centsToEurStr(int cents); // Definition weiter unten (Helper-Bereich)

/**
 * @brief Haelt creditCents im gueltigen Bereich [0, maxCreditCents] und loggt,
 *        falls das je noetig war. Nach JEDER Aenderung von creditCents aufrufen.
 */
void enforceCreditCap() {
  if (creditCents > maxCreditCents) {
    logEventf("SICHERHEIT: Guthaben (%s EUR) ueberschritt die Obergrenze von %s EUR - wurde gedeckelt!",
         centsToEurStr(creditCents).c_str(), centsToEurStr(maxCreditCents).c_str());
    creditCents = maxCreditCents;
  }
  if (creditCents < 0) creditCents = 0; // Guthaben kann nie negativ sein
}

// --- Automatischer Guthaben-Reset (taeglich zu fester Uhrzeit, im Webif einstellbar) ---
bool autoCreditResetEnabled = false;
int  autoCreditResetHour    = 3;    // 0-23
int  autoCreditResetMinute  = 0;    // 0-59
int  autoCreditResetLastDay = -1;   // tm_yday des letzten erfolgreichen Resets (-1 = noch nie)
bool autoCreditResetPending = false; // Zielzeit erreicht, wartet auf einen Kauf-freien Moment
unsigned long lastAutoCreditResetCheck = 0;

// --- Idle-Guthaben-Reset (unabhaengig von der Uhrzeit, funktioniert auch ohne NTP) ---
// Setzt Guthaben zurueck, wenn der Automat X Minuten unbenutzt war. Standardmaessig aus.
bool idleCreditResetEnabled = false;
int  idleCreditResetMinutes = 10; // 1-120

volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;

volatile unsigned long billAcceptorPulseCount = 0;
volatile unsigned long lastBillPulseEdgeTime = 0;
volatile unsigned long lastBillDebounceEdgeTime = 0;

int lastCreditSavedCents = 0;
unsigned long lastCreditChangeTime = 0;
const unsigned long NVS_SAVE_DELAY = 10000; // 10 Sekunden warten nach letztem Einwurf

// --- Display-Anpassung ---
const int SLOGAN_MAX_LENGTH = 24;
String displaySlogan   = "";
String displayFooter   = "www.hanimat.at";
bool   displayWhiteMode = false;

// --- Eingabe-Status ---
String keypadInputBuffer = "";
unsigned long lastKeypadInputTime = 0;
int selectedSlot = -1;
String pinEntryBuffer = ""; // Eingabepuffer fuer Abholfach-PIN (getrennt von keypadInputBuffer)

// --- Authentifizierung ---
String savedPassword       = DEFAULT_PASSWORD;
String activeSessionToken  = ""; // Leer = niemand eingeloggt
unsigned long lastActivityTimeWeb = 0;

bool displayNeedsUpdate = true;

// --- Verkaufsstatistik: Ringpuffer nur im RAM (max. 50 Einträge) ---
enum class PaymentMethod { CASH, SUMUP, PICKUP, MIXED }; // MIXED = Teilbetrag bar + Rest per Karte
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

// --- OTA-Update ---
String otaStatusMessage = "";
bool otaUpdateInProgress = false;

// --- Nicht-blockierende Fehleranzeige ---
bool errorDisplayActive = false;
unsigned long errorDisplayUntil = 0;

// --- Nicht-blockierender Melodie-Player ---
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

// --- Nicht-blockierender Einzel-Beep (Münze / Schein / Fehler) ---
struct SingleBeep {
  bool          active       = false;
  unsigned long endTime      = 0;
  // Optionale zweite Stufe (z.B. für Fehlerklang 2500 Hz → 2000 Hz)
  bool          hasNextTone  = false;
  int           nextFreq     = 0;
  unsigned long nextDuration = 0;
};
SingleBeep singleBeep;

// --- Nicht-blockierende Relais-Test-Jobs ---
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
  bool relayOn;    // true = AN-Phase (300ms), false = AUS-Pause (100ms)
  bool emptyOnly;  // true = nur leere, unverriegelte Faecher werden angefahren
};
RelaySequenceJob allRelaysTest = { false, 0, 0, false, false };

// --- Nicht-blockierende Reset-Taster-Zustandsmaschine ---
enum class ResetButtonState { NONE, DETECTING, MENU, CONFIRM };
ResetButtonState resetButtonState = ResetButtonState::NONE;
unsigned long resetDetectStartTime = 0;
unsigned long resetConfirmStartTime = 0;
int resetChoice = 0; // 1 = nur WLAN, 2 = Werksreset

// --- Telegram Bot ---
WiFiClientSecure secured_client;
String telegramBotToken = ""; // Platzhalter für den Telegram-Bot-Token
String telegramChatId = "";   // Platzhalter für die Telegram-Chat-ID
// Token ist beim Start noch leer – wird in setup() nach NVS-Laden via bot.updateToken() gesetzt
UniversalTelegramBot bot("", secured_client);


// =================================================================
//                      FUNKTIONS-PROTOTYPEN
// =================================================================
void setupWebServer();
void updateDisplayScreen();
char manualGetKeyState();
void processKeypad();
void processKeypadSelection();
void processPickupPinKey(char key);
void scheduleDispense(int slot, PaymentMethod method);
void processDispenseJob();
void addSaleLogEntry(int slot, int priceCents, PaymentMethod method);
bool controlSlotRelay(int slot, bool activate);
void processBillAcceptorPulses();
void resetDisplayToDefault();
void processAcceptedCoin();
void handleLogDataRequest();
void displayOTAMessageTFT(String line1, String line2 = "", String line3 = "", uint16_t color = 0);
void displayOTAProgressTFT(int pct);
void checkOverallStockLevel();
void sendHanimatStatusPing();
void checkHeapMonitor();
void handleSumUpPaymentInitiation();
void drawPageHeader(String title);
void saveCreditToNVS(bool force = false);
void processMelody();
void processSingleBeep();
void startBeep(int freq, int durationMs);
void processRelayTestJobs();
// Online-Update-Funktionen
void handleCheckOnlineUpdate();
void handleStartOnlineUpdate();

// LittleFS-API-Handler
void handleApiStatus();
void handleApiConfig();
void handleStartFsUpdate();

// Webserver-Handler
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

// HTML-Seiten-Generatoren
void showLoginPage();
void showDashboard();

// Hilfsfunktionen
int countAvailableSlots();
int countEmptySlots();
void displayErrorMessage(const String &line1, const String &line2 = "");
void playThankYouMelody();
void playErrorSound();
void playKeyPressBeep();
bool checkRelayBoardOnline();
void sendTelegramMessage(const String& message);
void processTelegramQueue();

// Interrupt-Service-Routinen
void IRAM_ATTR coinAcceptorISR();
void IRAM_ATTR billAcceptorISR();

// =================================================================
//                      HILFSFUNKTIONEN
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
// --- WEB-SESSION-HILFSFUNKTIONEN ---
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

// --- Relais-Hilfsfunktion ---
/**
 * @brief Prüft, ob das I2C-Relais-Expander-Board erreichbar ist.
 */
bool checkRelayBoardOnline() {
  Wire.beginTransmission(RELAY_I2C_ADDRESS);
  byte error = Wire.endTransmission();
  if (error != 0) {
    logf("ERROR: Relay board I2C not reachable (Addr: 0x%X, Code: %d)", RELAY_I2C_ADDRESS, error);
  }
  return (error == 0);
}

// --- NVS-Hilfsfunktion ---
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
 * @brief Zentrale Stelle fuer jede Guthaben-Erhoehung (Muenze, Schein, Karte,
 *        manuell) - haelt die Obergrenze ein und merkt den Aenderungszeitpunkt.
 */
void addCredit(int amountCents) {
  creditCents += amountCents;
  enforceCreditCap();
  lastCreditChangeTime = millis();
  displayNeedsUpdate   = true;
}

/**
 * @brief Zentrale Stelle fuer jeden Guthaben-Reset auf 0: sichert sofort in
 *        NVS und schreibt den Grund ins Ereignis-Log (nur wenn Guthaben da war).
 */
void resetCredit(const String &reason) {
  if (creditCents != 0) {
    logEventf("Guthaben-Reset (%s): %s EUR -> 0.00 EUR.",
              reason.c_str(), centsToEurStr(creditCents).c_str());
  }
  creditCents = 0;
  saveCreditToNVS(true);
  displayNeedsUpdate = true;
}

/**
 * @brief Prueft, ob der taegliche Guthaben-Reset faellig ist, und fuehrt ihn aus,
 *        sobald gerade kein Kauf/keine Zahlung laeuft (sonst verschoben).
 */
void checkAutoCreditReset() {
  if (!autoCreditResetEnabled) return;
  if (millis() - lastAutoCreditResetCheck < 5000) return; // alle 5 Sekunden reicht
  lastAutoCreditResetCheck = millis();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return; // Uhrzeit noch nicht synchronisiert (kein WLAN/NTP)

  if (!autoCreditResetPending &&
      timeinfo.tm_hour == autoCreditResetHour &&
      timeinfo.tm_min  == autoCreditResetMinute &&
      timeinfo.tm_yday != autoCreditResetLastDay) {
    autoCreditResetPending = true;
    logEventf("Automatischer Guthaben-Reset: Zielzeit %02d:%02d erreicht, warte auf freien Moment.",
              autoCreditResetHour, autoCreditResetMinute);
  }

  if (autoCreditResetPending) {
    bool busy = dispenseJob.active || isSumUpTransactionActive ||
                currentSystemState != CurrentSystemState::IDLE;
    if (!busy) {
      resetCredit("Uhrzeit-Reset");
      autoCreditResetLastDay = timeinfo.tm_yday;
      autoCreditResetPending = false;
    }
  }
}

/**
 * @brief Setzt das Guthaben zurueck, wenn der Automat laenger als
 *        idleCreditResetMinutes unbenutzt war (unabhaengig von Uhrzeit/NTP).
 */
void checkIdleCreditReset() {
  if (!idleCreditResetEnabled) return;
  if (creditCents <= 0) return; // nichts zurueckzusetzen

  // Gleicher Schutz wie beim Uhrzeit-Reset: nie mitten in einem Kauf/einer
  // Zahlung eingreifen (Geld-Pfad, defense-in-depth).
  if (dispenseJob.active || isSumUpTransactionActive ||
      currentSystemState != CurrentSystemState::IDLE) return;

  unsigned long idleMs = (unsigned long)idleCreditResetMinutes * 60000UL;
  if (millis() - lastUserInteractionTime >= idleMs) {
    resetCredit(String(idleCreditResetMinutes) + " min Inaktivitaet");
  }
}

/**
 * @brief Prueft periodisch, ob das Guthaben die Telegram-Warnschwelle ueberschreitet
 *        (unabhaengig davon, wodurch sich der Wert veraendert hat).
 */
void checkCreditWarnThreshold() {
  if (!telegramNotifyCreditThreshold) return;

  if (creditCents > creditWarnThresholdCents) {
    if (!creditWarnSent) {
      sendTelegramMessage("⚠️ HANIMAT: Guthaben hat " + centsToEurStr(creditWarnThresholdCents) +
                          " EUR ueberschritten (aktuell " + centsToEurStr(creditCents) + " EUR).");
      creditWarnSent = true;
      logEvent("Telegram: Guthaben-Warnschwelle ueberschritten, Nachricht gesendet.");
    }
  } else {
    creditWarnSent = false; // Wieder unter der Schwelle -> naechstes Ueberschreiten erneut melden
  }
}

// =================================================================
//                      INTERRUPT-SERVICE-ROUTINEN
// =================================================================

/**
 * @brief ISR für den Münzprüfer — zählt Impulse.
 */
void IRAM_ATTR coinAcceptorISR() {
  unsigned long now = millis();
  // Nur zaehlen, wenn der letzte Puls mind. 20ms her ist (filtert Prellen bei FAST-Einstellung)
  if (now - lastCoinPulseTime > 20) { 
    coinPulseCount++;
    lastCoinPulseTime = now;
  }
}

/**
 * @brief ISR für den Scheinprüfer — zählt Impulse mit Entprellung.
 */
void IRAM_ATTR billAcceptorISR() {
  unsigned long currentMillis = millis();
  if (currentMillis < STARTUP_IGNORE_BILL_TIME) return; // Impulse beim Start ignorieren

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
//  MODULE INCLUDES
//  Reihenfolge beachten: jedes Modul nutzt die Globals und Funktionen
//  die zuvor in dieser Datei definiert wurden.
// =================================================================
#include "audio.h"
#include "telegram.h"
#include "display.h"
#include "status.h"
#include "payment.h"
#include "web_handlers.h"


// =================================================================
//                            SETUP
// =================================================================
/**
 * @brief Zentrale Setup-Routine - Display-Init zuerst (vermeidet Boot-Verzug),
 *        dann Hardware-Aktivierung und zeitbegrenztes Netzwerk-Setup.
 */
void setup() {
  // --- 1. SOFORTIGER VISUELLER START (Millisekunden-Bereich) ---
  Serial.begin(115200);
  memset(logBuffer, 0, sizeof(logBuffer)); // Log-Buffer sauber initialisieren
  
  // Display als allererstes starten, um "weißen Bildschirm" zu vermeiden
  tft.begin();
  tft.setRotation(1);
  initLVGL();
  displayStartupScreen();


  logf("System Start: HANIMAT %s", FIRMWARE_VERSION);
  bootTime = millis();
  checkAndLogResetReason(); // Reset-Grund sofort sichern (vor allem anderen)

  // --- 2. I2C & RELAIS (ELEKTRISCHE INITIALISIERUNG) ---
  Wire.begin();
  Wire.setClock(400000L);       // Fast-Mode 400kHz
  Wire.setTimeout(3000); // 3ms Timeout bei Bus-Hänger (kein Einfrieren mehr)

  // LittleFS initialisieren (Web-Interface Assets)
  if (!LittleFS.begin(true)) { // true = formatOnFail
    logMessage("FEHLER: LittleFS konnte nicht gestartet werden!");
  } else {
    logMessage("LittleFS gestartet. Freier Speicher: " + String(LittleFS.totalBytes() - LittleFS.usedBytes()) + " Bytes");
  }

  // Alle Relais sofort in definierten AUS-Zustand zwingen (alle Expander)
  for (int e = 0; e < NUM_EXPANDERS; e++) {
    expanderOutputStates[e] = 0x0000;
    uint8_t addr = RELAY_I2C_ADDRESS + e;
    Wire.beginTransmission(addr);
    Wire.write(0x06); // Konfiguration: alle Pins als Ausgang
    Wire.write(0x00);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) break; // Expander nicht vorhanden → abbrechen
    Wire.beginTransmission(addr);
    Wire.write(0x02); // Ausgangs-Port 0: alle LOW
    Wire.write(0x00);
    Wire.write(0x00);
    Wire.endTransmission();
  }

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

  // Tastatur-Matrix-Pins
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

  // --- 4. NVS PREFERENCES (LADEN DER DATEN) ---
  preferences.begin("hanimat", false);
  char kBuf[16];

  if (!preferences.isKey("initialized")) {
    logMessage("NVS: Erst-Initialisierung...");
    for (int i = 0; i < 16; i++) {
      snprintf(kBuf, sizeof(kBuf), "priceC%d", i);
      preferences.putInt(kBuf, 500 + i * 10);
      snprintf(kBuf, sizeof(kBuf), "avail%d", i);
      preferences.putBool(kBuf, true);
      snprintf(kBuf, sizeof(kBuf), "locked%d", i);
      preferences.putBool(kBuf, false);
    }
    preferences.putInt("activeSlots", 16);
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
  telegramNotifyCreditThreshold = preferences.getBool("tgNotifyCredit", false);
  creditWarnThresholdCents      = preferences.getInt("creditWarnCts", 5000);
  almostEmptyThreshold = preferences.getInt("tgAlmostThres", 5);
  statusEnabled = preferences.getBool("statusEnabled", true);
  autoCreditResetEnabled = preferences.getBool("acrEnabled", false);
  autoCreditResetHour    = preferences.getInt("acrHour", 3);
  autoCreditResetMinute  = preferences.getInt("acrMinute", 0);
  idleCreditResetEnabled = preferences.getBool("idleCrEn", false);
  idleCreditResetMinutes = preferences.getInt("idleCrMin", 10);
  maxCreditCents = preferences.getInt("maxCreditCts", 10000);
  maxTopUpCents  = preferences.getInt("maxTopUpCts", 5000);
  bot.updateToken(telegramBotToken);
  
  sumupEnabled = preferences.getBool("suEnabled", false);
  sumupApiKey = preferences.getString("suApiKey", "");
  sumupMerchantId = preferences.getString("suMid", "");
  sumupReaderId = preferences.getString("suRid", "");
  sumupTimeout = preferences.getULong("suTimeout", 80000);
  sumUp = SumUpController(sumupApiKey, sumupMerchantId, sumupReaderId);

  displaySlogan    = preferences.getString("dispSlogan", "");
  displayFooter    = preferences.getString("dispFooter", "www.hanimat.at");
  displayWhiteMode = preferences.getBool("dispWhite", false);
  applyLVGLTheme();
  activeSlots = preferences.getInt("activeSlots", 16);

  // Abholfach-Arrays sind heap-allokiert (siehe Kommentar oben) und werden erst hier
  // auf Groesse `activeSlots` angelegt. new T[n]() initialisiert automatisch auf 0/false.
  slotIsPickup = new bool[activeSlots]();
  slotPinCode  = new char[activeSlots][7]();
  for (int i = 0; i < activeSlots; i++) {
    snprintf(kBuf, sizeof(kBuf), "pickup%d", i);
    slotIsPickup[i] = preferences.getBool(kBuf, false);
    snprintf(kBuf, sizeof(kBuf), "pin%d", i);
    preferences.getString(kBuf, slotPinCode[i], sizeof(slotPinCode[i]));
  }

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
  cashBoxCents      = preferences.getInt("cashBox",  0);

  // Münz- und Schein-Pulse-Mapping aus NVS laden (falls gespeichert)
  if (preferences.isKey("coinPulses")) {
    preferences.getBytes("coinPulses", pulseValues, sizeof(pulseValues));
  }
  if (preferences.isKey("billPulses")) {
    preferences.getBytes("billPulses", billValues,  sizeof(billValues));
  }
  coinAcceptorEnabled = preferences.getBool("coinEnabled", true);
  billAcceptorEnabled = preferences.getBool("billEnabled", true);

  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(kBuf, sizeof(kBuf), "sales%d", i);
    slotSalesCount[i] = preferences.getInt(kBuf, 0);
  }

  // NVS Migration V2: Slot-Einträge außerhalb der aktiven Fächer löschen (behebt NVS-Overflow)
  if (!preferences.isKey("nvsMigV2")) {
    logMessage("NVS: Migration V2 – bereinige inaktive Slot-Eintraege...");
    for (int i = activeSlots; i < MAX_SLOTS; i++) {
      snprintf(kBuf, sizeof(kBuf), "priceC%d", i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
      snprintf(kBuf, sizeof(kBuf), "avail%d",  i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
      snprintf(kBuf, sizeof(kBuf), "locked%d", i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
      snprintf(kBuf, sizeof(kBuf), "sales%d",  i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
      snprintf(kBuf, sizeof(kBuf), "pickup%d", i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
      snprintf(kBuf, sizeof(kBuf), "pin%d",    i);  if (preferences.isKey(kBuf)) preferences.remove(kBuf);
    }
    preferences.putBool("nvsMigV2", true);
    logf("NVS: Migration V2 abgeschlossen (Slots %d-%d entfernt).", activeSlots, MAX_SLOTS - 1);
  }

  // Wenn Firmware-Version neu (USB-Flash), NVS-FS-Version zurücksetzen
  String storedFwVer = preferences.getString("fwVerNvs", "");
  if (storedFwVer != String(FIRMWARE_VERSION)) {
    preferences.putString("fwVerNvs", FIRMWARE_VERSION);
    preferences.putString("fsFwVer",  FS_VERSION);
    installedFsVersion = String(FS_VERSION);
  } else {
    installedFsVersion = preferences.getString("fsFwVer", String(FS_VERSION));
  }

  // Absturzzähler laden und ggf. inkrementieren
  crashCount = preferences.getInt("crashCount", 0);
  if (wasUnexpectedReset) {
    crashCount++;
    preferences.putInt("crashCount", crashCount);
    logf("NVS: Absturzzähler → %d", crashCount);
  }

  preferences.end();

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
  wm.setConnectTimeout(3);
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback(configModeCallback);

  if (offlineMode) {
    logMessage("NETZ: Offline-Modus (Access-Point)");
    WiFi.softAP("HANIMAT-Offline", "Honig1234");
    
    displayOfflineModeScreen(WiFi.softAPIP().toString());
    delay(300);

  } else {
      
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
      displayWifiConnectedScreen(WiFi.localIP().toString(), String(FIRMWARE_VERSION));
      // Kein delay — Setup läuft sofort weiter, Screen bleibt kurz sichtbar
    }
  }

  // --- 7. FINALE DIENSTE ---
  // TLS ohne Zertifikats-Pinning: Telegram rotiert Intermediate-CAs regelmaessig,
  // setCACert() wuerde bei jedem CA-Wechsel brechen — setInsecure() ist hier stabiler.
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
  // Webserver-Clients immer bedienen
  server.handleClient();
  lv_timer_handler();

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

  // --- Nicht-blockierender Fehleranzeige-Timeout ---
  if (errorDisplayActive && millis() >= errorDisplayUntil) {
    errorDisplayActive = false;
    resetDisplayToDefault();
  }

  // --- Nicht-blockierender Melodie-Player ---
  processMelody();

  // --- Nicht-blockierender Einzel-Beep (Münze/Schein) ---
  processSingleBeep();

  // --- Nicht-blockierende Relais-Test-Jobs ---
  processRelayTestJobs();

// --- Reset-Logik: Nicht-blockierende Zustandsmaschine ---
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
        // 2 Sekunden stabil gedrückt → Auswahl-Menü zeigen
        resetButtonState = ResetButtonState::MENU;
        resetConfirmStartTime = millis();
        logMessage("Reset-Knopf gedrueckt. Auswahl: 1=WLAN, 2=Werksreset, *=Abbruch");
        lastDrawnMode = DrawnMode::NONE;
        displayResetScreen(0);
        playKeyPressBeep();
      }

    } else if (resetButtonState == ResetButtonState::MENU) {
      char key = manualGetKeyState();
      if (key == '1' || key == '2') {
        resetChoice = key - '0';
        resetButtonState = ResetButtonState::CONFIRM;
        resetConfirmStartTime = millis();
        displayResetScreen(resetChoice); // 1 = WLAN-Bestaetigung, 2 = Werksreset-Bestaetigung
        playKeyPressBeep();
      } else if (key == '*' || millis() - resetConfirmStartTime >= 15000) {
        logMessage("Reset-Menue abgebrochen.");
        resetButtonState = ResetButtonState::NONE;
        resetDisplayToDefault();
      }

    } else if (resetButtonState == ResetButtonState::CONFIRM) {
      char key = manualGetKeyState();
      if (key == '#') {
        if (resetChoice == 1) {
          // Nur WLAN: Zugangsdaten löschen, alle Einstellungen bleiben
          logEvent("WLAN-RESET bestaetigt (Einstellungen bleiben erhalten).");
          lastDrawnMode = DrawnMode::NONE;
          displayResetScreen(3);
          playKeyPressBeep();
          delay(2000); // Kurze Anzeige vor Neustart – hier bewusst OK
          WiFiManager wm;
          wm.resetSettings();
          ESP.restart();
        } else {
          logEvent("WERKSRESET bestaetigt!");
          lastDrawnMode = DrawnMode::NONE;
          displayResetScreen(4);
          playErrorSound();
          delay(2000); // Kurze Pause vor Neustart – hier bewusst OK
          preferences.begin("hanimat", false);
          preferences.clear();
          preferences.end();
          WiFiManager wm;
          wm.resetSettings();
          logMessage("Factory reset complete. Restarting...");
          ESP.restart();
        }
      } else if (key == '*' || millis() - resetConfirmStartTime >= 10000) {
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

  // --- Haupt-Zustandsmaschine ---
  if (currentSystemState != CurrentSystemState::OTA_UPDATE && !isSumUpTransactionActive) {
    // Timeout bei Inaktivitaet — Display auf Standardansicht zuruecksetzen
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

    // Timeout für Fach-Auswahl
    if (selectedSlot != -1 && (millis() - slotSelectedTime > SLOT_SELECTION_TIMEOUT)) {
        logMessage("Slot selection timed out. Resetting selection.");
        resetDisplayToDefault();
    }

    // Alle Eingaben und Jobs verarbeiten
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

  // --- SumUp-Button-Check (nicht-blockierendes Entprellen) ---
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
    lastUserInteractionTime = now; // Display-Sleep waehrend Zahlung verhindern

    // --- Abbruch sofort prüfen (für reaktionsschnelle Bedienung) ---
    char key = manualGetKeyState();
    if (key == '*') {
        logMessage("SumUp: Abbruch durch Benutzer (* Taste)");
        sumUp.cancel(); // API Call zum Terminal
        
        isSumUpTransactionActive = false; 
        currentSumUpTxId = "";
        
        displayErrorMessage("ZAHLUNG", "ABGEBROCHEN");
        // Zustands-Reset stellt sicher, dass wir zurueck in IDLE gehen
        return; // Loop iteration beenden
    }
    // -----------------------------------------------

    // 1. TIMEOUT PRÜFEN
    if (now - sumUpStartTime > sumupTimeout) {
        logEvent("SumUp: Zeit abgelaufen (Timeout)!");
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

            // WICHTIG: Gutgeschrieben wird der bei Zahlungsstart tatsaechlich an SumUp
            // geschickte Betrag (pendingSumUpAmountCents), nicht der aktuelle Fachpreis.
            addCredit(pendingSumUpAmountCents);

            logEventf("SumUp: Zahlung abgeschlossen (%s EUR abgerechnet, %s). Internes Guthaben: %s EUR",
                 centsToEurStr(pendingSumUpAmountCents).c_str(),
                 pendingSumUpWasMixed ? "Bar+Karte" : "Karte",
                 centsToEurStr(creditCents).c_str());

            // Guthaben sofort sichern (Absturzsicherheit vor Warenausgabe)
            saveCreditToNVS(true);

            // SumUp Status zurücksetzen
            isSumUpTransactionActive = false; // Stoppt das Polling
            currentSumUpTxId = "";
            currentSystemState = CurrentSystemState::IDLE; // Zurück in den Standardmodus

            // Warenausgabe starten (Bar+Karte, falls vor der Kartenzahlung schon Guthaben vorhanden war)
            logf("Starte Warenausgabe fuer Fach %d", selectedSlot + 1);
            scheduleDispense(selectedSlot, pendingSumUpWasMixed ? PaymentMethod::MIXED : PaymentMethod::SUMUP);

        }
        else if (status == "FAILED" || status == "CANCELLED") {
            logEventf("SumUp: Zahlung fehlgeschlagen oder abgebrochen (%s).", status.c_str());
            isSumUpTransactionActive = false; // Polling beenden
            currentSumUpTxId = "";
            displayErrorMessage("ZAHLUNG", "abgebrochen");
        }
        // Bei "PENDING" passiert nichts, der Loop läuft einfach weiter
    }
  }

  // Automatischer Logout aus der Weboberflaeche nach Timeout
  if (activeSessionToken.length() > 0 && (millis() - lastActivityTimeWeb > WEB_TIMEOUT)) {
    activeSessionToken = "";
    logMessage("Web: Session abgelaufen (Inaktivität).");
  }

  // Display nur aktualisieren, wenn noetig.
  // Gesperrt während: OTA, SumUp-Zahlung, aktiver Error-Anzeige (verhindert Überschreiben durch Münzeinwurf etc.)
  if (displayNeedsUpdate
      && currentSystemState != CurrentSystemState::OTA_UPDATE
      && !isSumUpTransactionActive
      && !errorDisplayActive) {
    updateDisplayScreen();
    displayNeedsUpdate = false;
  }

  // WLAN-Verbindung periodisch pruefen und bei Verlust neu verbinden
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

  // --- HEAP-ÜBERWACHUNG ---
  checkHeapMonitor();

  // --- AUTOMATISCHER GUTHABEN-RESET ---
  checkAutoCreditReset();
  checkIdleCreditReset();
  checkCreditWarnThreshold();
  yield(); // CPU an andere Tasks abgeben (Watchdog, WiFi-Stack) ohne zu blockieren
}

// =================================================================
//                      KERN-LOGIK
// =================================================================
void resetDisplayToDefault() {
  selectedSlot = -1;
  keypadInputBuffer = "";
  pinEntryBuffer = "";
  currentSystemState = CurrentSystemState::IDLE;
  displayNeedsUpdate = true;
  lastUserInteractionTime = millis();
}

void processKeypad() {
  char key = manualGetKeyState();
  if (key == 0) return; // Kein neuer Tastendruck

  playKeyPressBeep();
  logf("Keypad: Processed Key: '%c'", key);
  lastUserInteractionTime = millis();

  if (currentSystemState == CurrentSystemState::PICKUP_PIN_ENTRY) {
    processPickupPinKey(key);
    return;
  }

  currentSystemState = CurrentSystemState::USER_INTERACTION;

  if (isdigit(key)) {
    lastKeypadInputTime = millis();

    // Logik fuer 1- oder 2-stellige Fachnummern
    if (keypadInputBuffer.length() >= 2) {
      keypadInputBuffer = ""; // Buffer voll -> zuruecksetzen
    }
    keypadInputBuffer += key;
    logf("Keypad: Buffer updated to: %s", keypadInputBuffer.c_str());
    processKeypadSelection();

  } else if (key == '#') { // Auswahl bestaetigen oder Kauf ausloesen
    if (keypadInputBuffer.length() > 0) {
      logf("Keypad: '#' pressed. Finalizing selection from buffer: %s", keypadInputBuffer.c_str());
      processKeypadSelection();
    }
      
    if (selectedSlot != -1) {
      bool pickupEmpty = isPickupSlotEmpty(selectedSlot);
      if (slotLocked[selectedSlot]) {
        displayErrorMessage("FACH " + String(selectedSlot + 1), "gesperrt!");
      } else if (!slotAvailable[selectedSlot] || pickupEmpty) {
        displayErrorMessage("FACH " + String(selectedSlot + 1), "ist leer!");
      } else if (slotIsPickup[selectedSlot]) {
        logf("Abholfach: Fach %d ausgewaehlt, wechsle zu PIN-Eingabe.", selectedSlot + 1);
        pinEntryBuffer = ""; // Kunde tippt den kompletten Code selbst, inkl. der eigenen fuehrenden "0"
        slotSelectedTime = millis(); // Timer fuer SLOT_SELECTION_TIMEOUT neu starten (PIN-Eingabe braucht mehr Zeit)
        currentSystemState = CurrentSystemState::PICKUP_PIN_ENTRY;
        displayNeedsUpdate = true;
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
    keypadInputBuffer = ""; // Buffer nach '#' leeren
    
} else if (key == '*') { // Abbrechen/zuruecksetzen
    logMessage("Keypad: '*' pressed. Resetting selection.");
    resetDisplayToDefault();
}
  displayNeedsUpdate = true;
}

/**
 * @brief Verarbeitet den Tastatur-Eingabepuffer zur Fachauswahl.
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

    // Pruefen, ob die Auswahl final ist (z.B. bei einstelligen Faechern oder nach 2 Ziffern)
    bool isFinal = (keypadInputBuffer.length() == 2) || (activeSlots < 10);
    if (keypadInputBuffer.length() == 1 && activeSlots >= 10) {
        // Falls die erste Ziffer fuer eine gueltige 2-stellige Zahl zu hoch ist, ist die Auswahl final
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
      // 1-stellige Eingabe ungültig (z.B. "0") — sofort leeren, kein Haengenbleiben
      logf("Keypad: Ungueltige Einzelziffer '%s'. Buffer geleert.", keypadInputBuffer.c_str());
      keypadInputBuffer = "";
      selectedSlot = -1;
    }
  }
  displayNeedsUpdate = true;
}

/**
 * @brief Verarbeitet einen Tastendruck waehrend der PIN-Eingabe fuer ein Abholfach.
 *        Wird von processKeypad() aufgerufen, solange currentSystemState == PICKUP_PIN_ENTRY.
 */
void processPickupPinKey(char key) {
  if (isdigit(key)) {
    if (pinEntryBuffer.length() < 4) {
      pinEntryBuffer += key;
      slotSelectedTime = millis(); // Aktive Eingabe verlaengert das Zeitfenster
      logf("Abholfach: PIN-Eingabe: %d/%d Ziffern.", pinEntryBuffer.length(), strlen(slotPinCode[selectedSlot]));
    }
  } else if (key == '#') {
    if (pinEntryBuffer == slotPinCode[selectedSlot]) {
      logf("Abholfach: PIN korrekt fuer Fach %d.", selectedSlot + 1);
      scheduleDispense(selectedSlot, PaymentMethod::PICKUP);
    } else {
      logf("Abholfach: PIN falsch fuer Fach %d.", selectedSlot + 1);
      displayErrorMessage("CODE FALSCH", "bitte erneut");
    }
    pinEntryBuffer = "";
  } else if (key == '*') {
    logMessage("Abholfach: PIN-Eingabe abgebrochen.");
    resetDisplayToDefault();
  }
  displayNeedsUpdate = true;
}

/**
 * @brief Aktiviert oder deaktiviert per I2C das Relais für ein Fach.
 * @return True bei Erfolg, false bei I2C-Kommunikationsfehler.
 */
bool controlSlotRelay(int slot, bool activate) {
  if (slot < 0 || slot >= MAX_SLOTS) {
    logf("ERROR: Invalid slot index for relay: %d", slot);
    return false;
  }

  int     expander = slot / RELAYS_PER_EXPANDER;
  int     bitPos   = slot % RELAYS_PER_EXPANDER;
  uint8_t addr     = RELAY_I2C_ADDRESS + expander;

  if (activate) {
    expanderOutputStates[expander] |=  (1 << bitPos);
  } else {
    expanderOutputStates[expander] &= ~(1 << bitPos);
  }

  uint8_t relayCommand = (bitPos < 8) ? 0x02 : 0x03; // Port B oder A
  uint8_t dataByte     = (bitPos < 8)
    ? (uint8_t)(expanderOutputStates[expander] & 0xFF)
    : (uint8_t)(expanderOutputStates[expander] >> 8);

  Wire.beginTransmission(addr);
  Wire.write(relayCommand);
  Wire.write(dataByte);
  byte error = Wire.endTransmission();

  if (error == 0) {
    logf("Relay slot %d (Exp %d, Bit %d) %s OK.", slot + 1, expander, bitPos, activate ? "ON" : "OFF");
    lastRelayChangeTime = millis();
    return true;
  } else {
    logf("ERROR: I2C failed slot %d (Exp %d, Addr 0x%X). Code: %d", slot + 1, expander, addr, error);
    return false;
  }
}

char manualGetKeyState() {
  char currentPhysicalKey = 0;

  // Zeilen durchlaufen
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    digitalWrite(rowPins[r], HIGH); // Eine Zeile aktivieren
    // Alle Spalten dieser Zeile pruefen
    for (int c = 0; c < KEYPAD_COLS; c++) {
      if (digitalRead(colPins[c]) == HIGH) {
        currentPhysicalKey = keys[r][c];
        break;
      }
    }
    digitalWrite(rowPins[r], LOW); // Zeile deaktivieren
    if (currentPhysicalKey != 0) {
      break;
    }
  }

  unsigned long now = millis();

  // Entprell-Logik
  if (currentPhysicalKey != lastPhysicallyPressedKey) {
    lastKeyPressTime = now;
    lastPhysicallyPressedKey = currentPhysicalKey;
    if (currentPhysicalKey == 0) {
        lastReturnedKey = 0; // Zurueckgegebene Taste beim Loslassen zuruecksetzen
    }
    return 0; // Bei erstem Druck/Loslassen nichts zurueckgeben
  }

  // Wenn Taste laenger als die Entprellzeit gehalten wird, einmal zurueckgeben
  if (currentPhysicalKey != 0 && (now - lastKeyPressTime > KEYPAD_DEBOUNCE_PERIOD)) {
    if (currentPhysicalKey != lastReturnedKey) {
      lastReturnedKey = currentPhysicalKey;
      return currentPhysicalKey;
    }
  }
   
  return 0; // Kein gueltiger Tastendruck
}
