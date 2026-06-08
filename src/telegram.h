#pragma once
// =================================================================
//  TELEGRAM MODULE
//  Nicht-blockierende Nachrichtenwarteschlange für Telegram-Benachrichtigungen.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

/**
 * @brief Reiht eine Telegram-Nachricht in die Queue ein.
 *        Der tatsächliche HTTPS-Aufruf erfolgt nicht-blockierend in
 *        processTelegramQueue() im loop() — nur wenn IDLE und kein
 *        Dispense aktiv ist.
 */
void sendTelegramMessage(const String& message) {
  if (!telegramEnabled) return;
  if (telegramBotToken.length() == 0 || telegramChatId.length() == 0) {
    logMessage("Telegram: Nicht konfiguriert, Nachricht verworfen.");
    return;
  }
  if (tgQueueCount >= TG_QUEUE_MAX) {
    logMessage("Telegram: Queue voll, älteste Nachricht überschrieben.");
    // Älteste überschreiben statt neue verwerfen
    tgQueue[tgQueueTail] = message;
    tgQueueTail = (tgQueueTail + 1) % TG_QUEUE_MAX;
    tgQueueHead = (tgQueueHead + 1) % TG_QUEUE_MAX;
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
    tgQueueHead = tgQueueTail = tgQueueCount = 0;
    return;
  }
  if (digitalRead(OFFLINE_MODE_PIN) == LOW) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (dispenseJob.active) return;                          // Nicht während Warenausgabe
  if (millis() - lastTelegramSend < 3000) return;         // Rate-Limit: 1 msg / 3 Sek.

  const String& msg = tgQueue[tgQueueHead];
  logf("Telegram: Sende '%s'...", msg.substring(0, 50).c_str());
  if (bot.sendMessage(telegramChatId, msg, "")) {
    logMessage("Telegram: Gesendet.");
  } else {
    logMessage("Telegram: Sendefehler.");
  }
  tgQueue[tgQueueHead] = ""; // String-Heap freigeben
  tgQueueHead = (tgQueueHead + 1) % TG_QUEUE_MAX;
  tgQueueCount--;
  lastTelegramSend = millis();
}
