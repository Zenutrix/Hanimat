#pragma once
// =================================================================
//  PAYMENT MODULE
//  Münzen, Scheine, Warenausgabe und SumUp-Zahlungslogik.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

/**
 * @brief Speichert den Verkaufs-Log persistent in NVS.
 *        Wird nach jedem Kauf und nach einem Reset aufgerufen.
 */
void saveSaleLogToNVS() {
  preferences.begin("hanimat", false);
  preferences.putBytes("saleLogData",  saleLog,      sizeof(saleLog));
  preferences.putInt ("saleLogCount",  saleLogCount);
  preferences.putInt ("saleLogNext",   saleLogNext);
  preferences.putInt ("cashBox",       cashBoxCents);
  preferences.end();
}

/**
 * @brief Fügt einen Verkaufseintrag in den Ringpuffer ein und speichert ihn in NVS.
 *        Ältere Einträge werden bei vollem Puffer (max. SALE_LOG_SIZE) überschrieben.
 */
void addSaleLogEntry(int slot, int priceCents, PaymentMethod method) {
  SaleLogEntry& e = saleLog[saleLogNext];
  e.slot       = slot;
  e.priceCents = priceCents;
  e.method     = method;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    strftime(e.time, sizeof(e.time), "%d.%m. %H:%M:%S", &timeinfo);
  } else {
    snprintf(e.time, sizeof(e.time), "%lus", millis() / 1000UL);
  }

  saleLogNext = (saleLogNext + 1) % SALE_LOG_SIZE;
  if (saleLogCount < SALE_LOG_SIZE) saleLogCount++;

  // Kassenstand: nur Bar-Zahlungen zählen
  if (method == PaymentMethod::CASH) {
    cashBoxCents += priceCents;
  }

  saveSaleLogToNVS();
}

/**
 * @brief Plant einen Ausgabe-Job für ein bestimmtes Fach.
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

  dispenseJob.active         = true;
  dispenseJob.slot           = slotToDispense;
  dispenseJob.startTime      = millis();
  dispenseJob.relayActivated = false;
  dispenseJob.method         = method;

  logf("Dispense job scheduled for slot %d", slotToDispense + 1);
  currentSystemState = CurrentSystemState::DISPENSING;
  displayNeedsUpdate = true;
}

/**
 * @brief Verwaltet den aktiven Ausgabe-Job vom Relais-AN bis zum Relais-AUS.
 */
void processDispenseJob() {
  if (!dispenseJob.active) return;

  unsigned long currentTime = millis();
  if (currentSystemState != CurrentSystemState::DISPENSING) {
    currentSystemState = CurrentSystemState::USER_INTERACTION;
  }

  // --- Schritt 1: Relais aktivieren und Bezahlung verbuchen ---
  if (!dispenseJob.relayActivated) {
    digitalWrite(BILL_INHIBIT_PIN, HIGH); // Scheinprüfer sperren

    if (!controlSlotRelay(dispenseJob.slot, true)) {
      logf("processDispenseJob: ERROR activating relay for slot %d", dispenseJob.slot + 1);
      displayErrorMessage("RELAIS FEHLER", "Kauf abgebrochen");
      dispenseJob.active = false;
      digitalWrite(BILL_INHIBIT_PIN, LOW);
      resetDisplayToDefault();
      return;
    }

    // Guthaben abziehen
    creditCents -= slotPriceCents[dispenseJob.slot];
    if (creditCents < 0) creditCents = 0;

    // Slot als leer markieren + Statistik
    slotAvailable[dispenseJob.slot] = false;
    slotSalesCount[dispenseJob.slot]++;
    totalRevenueCents += slotPriceCents[dispenseJob.slot];
    logf("Purchase complete for slot %d. New credit: %s EUR. Total revenue: %s EUR",
         dispenseJob.slot + 1,
         centsToEurStr(creditCents).c_str(),
         centsToEurStr(totalRevenueCents).c_str());

    // NVS in einer Session schreiben (spart Flash-Öffnungs-Overhead)
    char availKey[12], salesKey[12];
    snprintf(availKey, sizeof(availKey), "avail%d", dispenseJob.slot);
    snprintf(salesKey, sizeof(salesKey), "sales%d", dispenseJob.slot);

    preferences.begin("hanimat", false);
    preferences.putBool(availKey, false);
    preferences.putInt(salesKey, slotSalesCount[dispenseJob.slot]);
    preferences.putInt("totalRev", totalRevenueCents);
    preferences.putInt("creditCts", creditCents);
    preferences.end();
    lastCreditSavedCents = creditCents;
    logf("NVS: Kauf + Guthaben gesichert (%s EUR)", centsToEurStr(creditCents).c_str());

    // Benachrichtigungen
    if (telegramNotifyOnSale) {
      String saleMessage = "🍯 VERKAUF: Fach #" + String(dispenseJob.slot + 1) + " wurde verkauft und ist jetzt leer.";
      sendTelegramMessage(saleMessage);
    }
    checkOverallStockLevel();
    addSaleLogEntry(dispenseJob.slot, slotPriceCents[dispenseJob.slot], dispenseJob.method);

    currentSystemState = CurrentSystemState::DISPENSING;
    playThankYouMelody();

    dispenseJob.relayActivated = true;
    dispenseJob.startTime      = currentTime;
    displayNeedsUpdate         = true;
  }

  // --- Schritt 2: Relais nach Ablaufzeit deaktivieren ---
  if (dispenseJob.relayActivated && (currentTime - dispenseJob.startTime >= DISPENSE_RELAY_ON_TIME)) {
    logf("Dispense time elapsed. Deactivating relay for slot %d", dispenseJob.slot + 1);
    controlSlotRelay(dispenseJob.slot, false);
    dispenseJob.active = false;
    digitalWrite(BILL_INHIBIT_PIN, LOW);
    resetDisplayToDefault();
  }
}

/**
 * @brief Verarbeitet Münzpulse nach einem Debounce-Delay.
 */
void processAcceptedCoin() {
  if (!coinAcceptorEnabled) { coinPulseCount = 0; return; }
  if (coinPulseCount > 0 && (millis() - lastCoinPulseTime > COIN_PROCESSING_DELAY)) {
    int pulsesToProcess;

    noInterrupts();
    pulsesToProcess = coinPulseCount;
    coinPulseCount  = 0;
    interrupts();

    logf("Münzprüfer: %d Pulse erkannt.", pulsesToProcess);

    if (pulsesToProcess > 0 && pulsesToProcess < (int)(sizeof(pulseValues) / sizeof(pulseValues[0]))) {
      int coinValueCents = pulseValues[pulsesToProcess];
      if (coinValueCents > 0) {
        creditCents += coinValueCents;
        lastCreditChangeTime = millis();
        logf("Guthaben aktualisiert: +%s EUR", centsToEurStr(coinValueCents).c_str());
        displayNeedsUpdate       = true;
        lastUserInteractionTime  = millis();
        currentSystemState       = CurrentSystemState::USER_INTERACTION;
        startBeep(1200, 40);
      } else {
        logf("Münz-Fehler: Wert für %d Pulse ist 0.", pulsesToProcess);
      }
    } else {
      logf("Coin Fehler: %d Pulse passen zu keinem Mapping.", pulsesToProcess);
    }
  }
}

/**
 * @brief Verarbeitet Scheinpulse nach Gruppen-Timeout.
 */
void processBillAcceptorPulses() {
  if (!billAcceptorEnabled) { billAcceptorPulseCount = 0; digitalWrite(BILL_INHIBIT_PIN, HIGH); return; }
  // Rauschen direkt nach Relais-Schaltung ignorieren
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
    pulsesToProcess        = billAcceptorPulseCount;
    billAcceptorPulseCount = 0;
    interrupts();

    logf("Bill: Processing %d pulses.", pulsesToProcess);

    if (pulsesToProcess > 0 && pulsesToProcess < (int)(sizeof(billValues) / sizeof(billValues[0]))) {
      int billValueEuros = billValues[pulsesToProcess];
      if (billValueEuros > 0) {
        creditCents          += billValueEuros * 100;
        lastCreditChangeTime  = millis();
        logf("Bill accepted: %d pulses -> %d EUR. New credit: %s EUR",
             pulsesToProcess, billValueEuros, centsToEurStr(creditCents).c_str());
        displayNeedsUpdate      = true;
        lastUserInteractionTime = millis();
        currentSystemState      = CurrentSystemState::USER_INTERACTION;
        startBeep(1000, 150);
      } else {
        logf("Bill: %d pulses has a value of 0.", pulsesToProcess);
      }
    } else {
      logf("Bill: Invalid pulse count rejected: %d", pulsesToProcess);
    }
  }

  // Scheinprüfer inhibiten solange Pulseingabe läuft
  digitalWrite(BILL_INHIBIT_PIN, (billAcceptorPulseCount > 0) ? HIGH : LOW);
}

/**
 * @brief Leitet den SumUp-Zahlungsprozess ein (non-blocking).
 *        Setzt den State und kehrt zum Loop zurück; der Loop pollt dann den Status.
 */
void handleSumUpPaymentInitiation() {
  if (!sumupEnabled) {
    displayErrorMessage("SUMUP", "Deaktiviert");
    return;
  }
  if (selectedSlot == -1) {
    displayErrorMessage("KEIN FACH", "Bitte wählen!");
    return;
  }
  if (slotLocked[selectedSlot]) {
    displayErrorMessage("FACH " + String(selectedSlot + 1), "gesperrt!");
    return;
  }
  if (!slotAvailable[selectedSlot]) {
    displayErrorMessage("FACH " + String(selectedSlot + 1), "ist leer!");
    return;
  }

  currentSystemState      = CurrentSystemState::SUMUP_PENDING;
  lastUserInteractionTime = millis();

  int priceCents    = slotPriceCents[selectedSlot];
  int remainingCents = priceCents - creditCents;

  logf("SumUp: Prozess gestartet fuer Fach %d", selectedSlot + 1);
  logf("SumUp: Preis: %s EUR, Guthaben: %s EUR -> Zu zahlen: %s EUR",
       centsToEurStr(priceCents).c_str(),
       centsToEurStr(creditCents).c_str(),
       centsToEurStr(remainingCents).c_str());

  if (remainingCents <= 0) {
    logMessage("SumUp: Abbruch, Guthaben deckt bereits den Preis.");
    scheduleDispense(selectedSlot, PaymentMethod::CASH);
    return;
  }
  if (remainingCents < 100) {
    logf("SumUp: Restbetrag %s EUR ist zu gering. Minimum 1.00 EUR.",
         centsToEurStr(remainingCents).c_str());
    displayErrorMessage("MIN. KARTE", "ab 1.00 EUR");
    return;
  }

  // TFT vorbereiten
  lastDrawnMode = DrawnMode::NONE;
  String l1 = "KARTENZAHLUNG";
  String l2 = "Zu zahlen: " + centsToEurStr(remainingCents) + " EUR";
  String l3 = "Bitte am Terminal folgen...";
  displayOTAMessageTFT(l1, l2, l3, 0);

  // API-Aufruf
  logf("SumUp: MerchantID='%s' ReaderID='%s' APIKey-Len=%d",
       sumupMerchantId.c_str(), sumupReaderId.c_str(), sumupApiKey.length());

  String trackingId;
  if (sumUp.startPayment(remainingCents, trackingId)) {
    isSumUpTransactionActive = true;
    currentSumUpTxId         = trackingId;
    pendingSumUpAmountCents  = remainingCents;
    sumUpStartTime           = millis();
    lastSumUpCheckTime       = 0;
    logf("SumUp: Checkout API OK. Tracking-ID: %s. Warte im Loop auf Terminal...",
         trackingId.c_str());
  } else {
    logMessage("SumUp: Fehler beim Starten des Checkouts (API-Aufruf fehlgeschlagen).");
    displayErrorMessage("SUMUP FEHLER", "API nicht erreichbar");
    currentSystemState = CurrentSystemState::IDLE;
  }
}
