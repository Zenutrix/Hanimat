#pragma once
// =================================================================
//  ZAHLUNGSMODUL
//  Münzen, Scheine, Warenausgabe und SumUp-Zahlungslogik.
//  Wird von main.cpp per #include eingebunden (einzelne Übersetzungseinheit).
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

  formatLogTimestamp(e.time, sizeof(e.time));

  saleLogNext = (saleLogNext + 1) % SALE_LOG_SIZE;
  if (saleLogCount < SALE_LOG_SIZE) saleLogCount++;

  // Kassenstand: Bar-Zahlungen voll, bei Mischzahlung nur der tatsächliche Bar-Anteil
  if (method == PaymentMethod::CASH) {
    cashBoxCents += priceCents;
  } else if (method == PaymentMethod::MIXED) {
    int cashPortion = priceCents - pendingSumUpAmountCents;
    if (cashPortion > 0) cashBoxCents += cashPortion;
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

  // Laufende Relais-Sequenzen/-Tests abbrechen: Der Kauf hat Vorrang, sonst könnte die
  // Sequenz das Relais wieder abschalten und die Ausgabe abwürgen (Ware fest, Geld abgebucht).
  if (allRelaysTest.active) {
    controlSlotRelay(allRelaysTest.currentSlot, false);
    allRelaysTest.active = false;
    logMessage("scheduleDispense: Relais-Sequenz fuer Kauf abgebrochen.");
  }
  if (singleRelayTest.active) {
    controlSlotRelay(singleRelayTest.slot, false);
    singleRelayTest.active = false;
    logMessage("scheduleDispense: Einzel-Relais-Test fuer Kauf abgebrochen.");
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

    bool isPickup    = (dispenseJob.method == PaymentMethod::PICKUP);
    bool isCardBased = (dispenseJob.method == PaymentMethod::SUMUP ||
                        dispenseJob.method == PaymentMethod::MIXED);
    // Preis-Snapshot vom Zahlungsstart nutzen: Live-Preis könnte sich während der
    // bis zu 80s Terminal-Wartezeit geändert und Umsatz/Kassenstand verfälscht haben.
    int saleCents = isPickup ? 0
                  : (isCardBased ? pendingSumUpPriceCents
                                 : slotPriceCents[dispenseJob.slot]);

    if (!isPickup) {
      // Guthaben abziehen (Abholfächer sind kostenlos)
      creditCents -= saleCents;
      if (creditCents < 0) creditCents = 0;
      totalRevenueCents += saleCents;
    }

    // Slot als leer markieren + Statistik
    slotAvailable[dispenseJob.slot] = false;
    slotSalesCount[dispenseJob.slot]++;
    if (isPickup) {
      slotPinCode[dispenseJob.slot][0] = '\0'; // Einmal-Code verbraucht
    }
    logf("Ausgabe abgeschlossen fuer Fach %d (%s). Guthaben: %s EUR. Gesamtumsatz: %s EUR",
         dispenseJob.slot + 1,
         isPickup ? "Abholung" : "Verkauf",
         centsToEurStr(creditCents).c_str(),
         centsToEurStr(totalRevenueCents).c_str());

    // NVS in einer Session schreiben (spart Flash-Öffnungs-Overhead)
    char availKey[12], salesKey[12], pinKey[12];
    snprintf(availKey, sizeof(availKey), "avail%d", dispenseJob.slot);
    snprintf(salesKey, sizeof(salesKey), "sales%d", dispenseJob.slot);

    preferences.begin("hanimat", false);
    preferences.putBool(availKey, false);
    preferences.putInt(salesKey, slotSalesCount[dispenseJob.slot]);
    if (isPickup) {
      snprintf(pinKey, sizeof(pinKey), "pin%d", dispenseJob.slot);
      preferences.putString(pinKey, "");
    } else {
      preferences.putInt("totalRev", totalRevenueCents);
      preferences.putInt("creditCts", creditCents);
    }
    preferences.end();
    lastCreditSavedCents = creditCents;
    logf("NVS: Ausgabe gesichert (%s EUR)", centsToEurStr(creditCents).c_str());

    // Benachrichtigungen
    if (telegramNotifyOnSale) {
      String saleMessage = isPickup
        ? "📦 ABHOLUNG: Fach #" + String(dispenseJob.slot + 1) + " wurde abgeholt und ist jetzt leer."
        : "🍯 VERKAUF: Fach #" + String(dispenseJob.slot + 1) + " wurde verkauft und ist jetzt leer.";
      sendTelegramMessage(saleMessage);
    }
    checkOverallStockLevel();
    addSaleLogEntry(dispenseJob.slot, saleCents, dispenseJob.method);

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
        addCredit(coinValueCents);
        logf("Guthaben aktualisiert: +%s EUR", centsToEurStr(coinValueCents).c_str());
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
        addCredit(billValueEuros * 100);
        logf("Bill accepted: %d pulses -> %d EUR. New credit: %s EUR",
             pulsesToProcess, billValueEuros, centsToEurStr(creditCents).c_str());
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
 * @brief Leitet den SumUp-Zahlungsprozess ein (nicht blockierend).
 *        Setzt den Zustand und kehrt zum Loop zurück; der Loop pollt danach den Status.
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
    pendingSumUpPriceCents   = priceCents;        // Preis-Snapshot für Umsatz/Kassenstand
    pendingSumUpWasMixed     = (creditCents > 0); // Schon Bar-Guthaben vorhanden -> Mischzahlung
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
