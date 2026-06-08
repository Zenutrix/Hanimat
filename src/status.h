#pragma once
// =================================================================
//  STATUS MODULE
//  Bestandsprüfung, Heap-Monitoring und Status-Heartbeat.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

/**
 * @brief Prüft den Gesamtbestand und sendet Telegram-Benachrichtigungen
 *        wenn Schwellenwerte (fast leer / komplett leer) erreicht werden.
 */
void checkOverallStockLevel() {
  int totalAvailable = countAvailableSlots();

  if (telegramNotifyEmpty && totalAvailable == 0) {
    if (!emptyNotificationSent) {
      sendTelegramMessage("🚨 ALARM: Der HANIMAT ist komplett ausverkauft! Bitte auffüllen! 😭");
      emptyNotificationSent       = true;
      almostEmptyNotificationSent = true;
      logMessage("Telegram: Alarm 'Ausverkauft' gesendet.");
    }
  } else if (telegramNotifyAlmostEmpty && totalAvailable > 0 && totalAvailable <= almostEmptyThreshold) {
    if (!almostEmptyNotificationSent) {
      sendTelegramMessage("⚠️ INFO: Der HANIMAT ist fast leer!\nVerfügbare Fächer: " + String(totalAvailable));
      almostEmptyNotificationSent = true;
      logMessage("Telegram: Info 'Fast leer' gesendet (" + String(totalAvailable) + " übrig).");
    }
  } else if (totalAvailable > almostEmptyThreshold) {
    if (almostEmptyNotificationSent || emptyNotificationSent) {
      logMessage("Bestand wieder ok (" + String(totalAvailable) + "). Flags zurückgesetzt.");
    }
    almostEmptyNotificationSent = false;
    emptyNotificationSent       = false;
  }
}

/**
 * @brief Überwacht den freien Heap und sendet eine Telegram-Warnung bei
 *        Unterschreitung des Schwellenwerts. Läuft im Intervall HEAP_CHECK_INTERVAL.
 */
void checkHeapMonitor() {
  if (millis() - lastHeapCheckTime < HEAP_CHECK_INTERVAL) return;
  lastHeapCheckTime = millis();

  uint32_t freeHeap = ESP.getFreeHeap();
  logf("Heap: %u bytes frei (Min: %u)", freeHeap, ESP.getMinFreeHeap());

  if (freeHeap < HEAP_WARN_THRESHOLD) {
    if (!heapWarningSent) {
      sendTelegramMessage("⚠️ HANIMAT Heap-Warnung: Nur noch " +
                          String(freeHeap / 1024) + " KB frei. Neustart empfohlen.");
      heapWarningSent = true;
      logMessage("Heap-Warnung gesendet.");
    }
  } else {
    heapWarningSent = false; // Reset sobald Heap sich erholt
  }
}

/**
 * @brief Sendet einen periodischen Status-Heartbeat an hanimat.at.
 *        Respektiert den Hardware-Offline-Schalter und das statusEnabled-Flag.
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
  String url = String(statusServerUrl) + "?id=" + chipId +
               "&key=" + statusApiKey +
               "&v="   + FIRMWARE_VERSION;

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
