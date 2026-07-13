#pragma once
// =================================================================
//  STATUS MODULE
//  Bestandsprüfung, Heap-Monitoring und Status-Heartbeat.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

/** @brief Prüft Gesamtbestand und meldet per Telegram, wenn er fast leer oder leer ist. */
void checkOverallStockLevel() {
  int totalAvailable = countAvailableSlots();

  if (telegramNotifyEmpty && totalAvailable == 0) {
    if (!emptyNotificationSent) {
      sendTelegramMessage("🚨 ALARM: Der HANIMAT ist komplett ausverkauft! Bitte auffüllen! 😭");
      emptyNotificationSent       = true;
      almostEmptyNotificationSent = true;
      logEvent("Telegram: Alarm 'Ausverkauft' gesendet.");
    }
  } else if (telegramNotifyAlmostEmpty && totalAvailable > 0 && totalAvailable <= almostEmptyThreshold) {
    if (!almostEmptyNotificationSent) {
      sendTelegramMessage("⚠️ INFO: Der HANIMAT ist fast leer!\nVerfügbare Fächer: " + String(totalAvailable));
      almostEmptyNotificationSent = true;
      logEvent("Telegram: Info 'Fast leer' gesendet (" + String(totalAvailable) + " übrig).");
    }
  } else if (totalAvailable > almostEmptyThreshold) {
    if (almostEmptyNotificationSent || emptyNotificationSent) {
      logEvent("Bestand wieder ok (" + String(totalAvailable) + "). Flags zurückgesetzt.");
    }
    almostEmptyNotificationSent = false;
    emptyNotificationSent       = false;
  }
}

/** @brief Überwacht freien Heap, warnt per Telegram bei Unterschreitung (Intervall: HEAP_CHECK_INTERVAL). */
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
      logEventf("Heap-Warnung gesendet (%u Bytes frei).", freeHeap);
    }
  } else {
    heapWarningSent = false; // Reset, sobald Heap sich erholt hat
  }
}

/** @brief Sendet periodischen Status-Heartbeat an hanimat.at (respektiert Offline-Schalter & statusEnabled). */
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
