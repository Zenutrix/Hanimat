#pragma once
// =================================================================
//  WEBSERVER MODULE
//  HTTP-Handler, SPA-Dashboard und OTA-Funktionen für den HANIMAT.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

// --- Utility (gebraucht von showDashboard) ---
int countAvailableSlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

int countEmptySlots() {
  int count = 0;
  for (int i = 0; i < activeSlots; i++) {
    if (!slotAvailable[i] && !slotLocked[i]) count++;
  }
  return count;
}

void handleCheckOnlineUpdate() {
    if (!isAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }

    if (digitalRead(OFFLINE_MODE_PIN) == LOW) {
        server.send(503, "application/json", "{\"error\":\"Offline-Modus aktiv\"}");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(15000);

    logf("Online-Update: Pruefe Version auf %s", UPDATE_VERSION_URL);

    if (!http.begin(client, UPDATE_VERSION_URL)) {
        server.send(502, "application/json", "{\"error\":\"Verbindung fehlgeschlagen\"}");
        return;
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        server.send(502, "application/json",
            "{\"error\":\"HTTP " + String(httpCode) + "\"}");
        return;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        server.send(500, "application/json", "{\"error\":\"JSON ungueltig\"}");
        return;
    }

    String remoteFw    = doc["version"]    | "";
    String remoteFsVer = doc["fs_version"] | "";
    String changelog   = doc["changelog"]  | "";

    if (remoteFw.isEmpty()) {
        server.send(500, "application/json", "{\"error\":\"'version' fehlt in version.json\"}");
        return;
    }

    bool fwNeedsUpdate = (remoteFw != String(FIRMWARE_VERSION));
    bool fsNeedsUpdate = !remoteFsVer.isEmpty() && (remoteFsVer != installedFsVersion);
    bool updateAvail   = fwNeedsUpdate || fsNeedsUpdate;

    // JSON-sichere Strings (Anführungszeichen escapen)
    auto jsonStr = [](const String& s) -> String {
        String out; out.reserve(s.length() + 4);
        for (char c : s) { if (c == '"') out += "\\\""; else if (c == '\\') out += "\\\\"; else out += c; }
        return out;
    };

    String json = "{";
    json += "\"fw_installed\":\""   + jsonStr(FIRMWARE_VERSION)    + "\",";
    json += "\"fw_online\":\""      + jsonStr(remoteFw)            + "\",";
    json += String("\"fw_needs_update\":") + (fwNeedsUpdate ? "true" : "false") + ",";
    json += "\"fs_installed\":\""   + jsonStr(installedFsVersion)  + "\",";
    json += "\"fs_online\":\""      + jsonStr(remoteFsVer)         + "\",";
    json += String("\"fs_needs_update\":") + (fsNeedsUpdate ? "true" : "false") + ",";
    json += "\"changelog\":\""      + jsonStr(changelog)           + "\",";
    json += String("\"update_available\":") + (updateAvail ? "true" : "false");
    json += "}";

    server.send(200, "application/json", json);
}

void handleStartFullUpdate() {
    if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }

    if (digitalRead(OFFLINE_MODE_PIN) == LOW || WiFi.status() != WL_CONNECTED) {
        server.send(503, "text/plain", "Fehler: Offline oder kein WLAN.");
        return;
    }

    logMessage("Full-Update: Lade version.json...");
    displayOTAMessageTFT("SYSTEM UPDATE", "Verbinde...");

    WiFiClientSecure vClient;
    vClient.setInsecure();
    vClient.setTimeout(10000);
    HTTPClient vHttp;
    vHttp.setTimeout(10000);

    if (!vHttp.begin(vClient, UPDATE_VERSION_URL)) {
        server.send(502, "text/plain", "Fehler: Verbindung zu version.json fehlgeschlagen.");
        resetDisplayToDefault();
        return;
    }
    int vCode = vHttp.GET();
    if (vCode != 200) {
        vHttp.end();
        server.send(502, "text/plain", "Fehler: version.json HTTP " + String(vCode));
        resetDisplayToDefault();
        return;
    }
    String vPayload = vHttp.getString();
    vHttp.end();

    JsonDocument vDoc;
    if (deserializeJson(vDoc, vPayload)) {
        server.send(500, "text/plain", "Fehler: version.json JSON ungueltig.");
        resetDisplayToDefault();
        return;
    }

    String remoteFw  = vDoc["version"]    | "";
    String fsVersion = vDoc["fs_version"] | "";

    bool doFs = !fsVersion.isEmpty() && (fsVersion != installedFsVersion);
    bool doFw = !remoteFw.isEmpty()  && (remoteFw  != String(FIRMWARE_VERSION));

    if (!doFs && !doFw) {
        server.send(200, "text/plain", "Alles aktuell. Kein Update notwendig.");
        resetDisplayToDefault();
        return;
    }

    // Antwort senden bevor blockierender Prozess startet
    String msg = "Update:";
    if (doFs) msg += " WebIF(" + fsVersion + ")";
    if (doFw) msg += " Firmware(" + remoteFw + ")";
    msg += ". Geraet startet neu...";
    server.send(200, "text/plain", msg);
    delay(300);

    sendTelegramMessage(String("🔄 HANIMAT Update gestartet:\n") + msg);

    otaUpdateInProgress = true;
    currentSystemState  = CurrentSystemState::OTA_UPDATE;

    httpUpdate.onProgress([](int cur, int total) {
        if (total > 0) displayOTAProgressTFT((cur * 100) / total);
    });
    // Kein auto-reboot durch httpUpdate – wir steuern den Neustart selbst,
    // damit LittleFS und Firmware in einem Durchgang geflasht werden können.
    httpUpdate.rebootOnUpdate(false);

    bool fsOk = !doFs;  // wenn kein FS-Update nötig, gilt es als "OK"
    bool fwOk = !doFw;

    // ── LittleFS flashen (manuell via HTTPClient + Update.h) ──
    if (doFs) {
        logf("Full-Update: Flashe LittleFS '%s'...", fsVersion.c_str());
        displayOTAMessageTFT("SYSTEM UPDATE", "WebIF laden...", fsVersion.c_str());

        WiFiClientSecure fsClient;
        fsClient.setInsecure();
        fsClient.setTimeout(30);
        HTTPClient fsHttp;
        fsHttp.setTimeout(30000);
        fsHttp.begin(fsClient, UPDATE_FS_URL);
        int fsHttpCode = fsHttp.GET();

        if (fsHttpCode == 200) {
            int fsSize = fsHttp.getSize();
            logf("Full-Update: LittleFS Download OK, Groesse: %d Bytes", fsSize);

            // Direkt via IDF Partition-API flashen (Update.h hat Probleme mit LittleFS)
            const esp_partition_t* fsPart = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);

            if (!fsPart) {
                fsHttp.end();
                String errMsg = "LittleFS Partition nicht gefunden";
                logf("Full-Update: LittleFS Fehler: %s", errMsg.c_str());
                sendTelegramMessage("❌ HANIMAT LittleFS Fehler: " + errMsg);
                displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
                { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
                otaUpdateInProgress = false;
                currentSystemState  = CurrentSystemState::IDLE;
                resetDisplayToDefault();
                return;
            }

            logf("Full-Update: Partition gefunden @ 0x%X, %d Bytes", fsPart->address, fsPart->size);
            LittleFS.end();

            if (esp_partition_erase_range(fsPart, 0, fsPart->size) != ESP_OK) {
                fsHttp.end();
                String errMsg = "Partition-Erase fehlgeschlagen";
                logf("Full-Update: LittleFS Fehler: %s", errMsg.c_str());
                sendTelegramMessage("❌ HANIMAT LittleFS Fehler: " + errMsg);
                displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
                { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
                otaUpdateInProgress = false;
                currentSystemState  = CurrentSystemState::IDLE;
                resetDisplayToDefault();
                return;
            }

            WiFiClient* stream = fsHttp.getStreamPtr();
            // Puffer muss Vielfaches von 4 sein; wird erst geschrieben wenn voll
            const size_t WBUF_SIZE = 512;
            uint8_t* wBuf = (uint8_t*)malloc(WBUF_SIZE);
            if (!wBuf) {
                fsHttp.end();
                String errMsg = "Kein Heap fuer Puffer";
                logf("Full-Update: LittleFS Fehler: %s", errMsg.c_str());
                sendTelegramMessage("❌ HANIMAT LittleFS Fehler: " + errMsg);
                displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
                { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
                otaUpdateInProgress = false;
                currentSystemState  = CurrentSystemState::IDLE;
                resetDisplayToDefault();
                return;
            }
            size_t wBufPos = 0;
            size_t written  = 0;
            unsigned long dlStart = millis();
            bool writeError = false;

            while (fsHttp.connected() && (int)written + (int)wBufPos < fsSize) {
                size_t avail = stream->available();
                if (avail) {
                    size_t toRead = min(avail, WBUF_SIZE - wBufPos);
                    wBufPos += stream->readBytes(wBuf + wBufPos, toRead);
                    dlStart = millis();

                    bool lastChunk = ((int)(written + wBufPos) >= fsSize);
                    if (wBufPos == WBUF_SIZE || lastChunk) {
                        size_t toWrite = (wBufPos + 3) & ~3u;
                        if (toWrite > wBufPos) memset(wBuf + wBufPos, 0xFF, toWrite - wBufPos);
                        if (esp_partition_write(fsPart, written, wBuf, toWrite) != ESP_OK) {
                            writeError = true;
                            break;
                        }
                        written += wBufPos;
                        wBufPos = 0;
                        if (fsSize > 0) displayOTAProgressTFT((written * 100) / fsSize);
                    }
                } else if (millis() - dlStart > 30000) {
                    break;
                }
                yield();
            }
            fsHttp.end();
            free(wBuf);

            if (!writeError && (int)written == fsSize) {
                preferences.begin("hanimat", false);
                preferences.putString("fsFwVer", fsVersion);
                preferences.end();
                installedFsVersion = fsVersion;
                logMessage("Full-Update: LittleFS OK.");
                fsOk = true;
            } else {
                String errMsg = writeError ? "Schreibfehler" : "Unvollstaendig (" + String(written) + "/" + String(fsSize) + ")";
                logf("Full-Update: LittleFS Fehler: %s", errMsg.c_str());
                sendTelegramMessage("❌ HANIMAT LittleFS Fehler: " + errMsg);
                displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
                { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
                otaUpdateInProgress = false;
                currentSystemState  = CurrentSystemState::IDLE;
                resetDisplayToDefault();
                return;
            }
        } else {
            fsHttp.end();
            String errMsg = "HTTP " + String(fsHttpCode);
            logf("Full-Update: LittleFS Fehler: %s", errMsg.c_str());
            sendTelegramMessage("❌ HANIMAT LittleFS Fehler: " + errMsg);
            displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
            { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
            otaUpdateInProgress = false;
            currentSystemState  = CurrentSystemState::IDLE;
            resetDisplayToDefault();
            return;
        }
    }

    // ── Firmware flashen (direkt via ESP-IDF OTA API) ──
    if (doFw) {
        logf("Full-Update: Flashe Firmware '%s'...", remoteFw.c_str());
        displayOTAMessageTFT("SYSTEM UPDATE", "Firmware laden...", remoteFw.c_str());

        WiFiClientSecure fwClient;
        fwClient.setInsecure();
        HTTPClient fwHttp;
        fwHttp.setTimeout(30000);
        fwHttp.begin(fwClient, UPDATE_FIRMWARE_URL);
        int fwHttpCode = fwHttp.GET();

        if (fwHttpCode != 200) {
            fwHttp.end();
            String errMsg = "HTTP " + String(fwHttpCode);
            logf("Full-Update: Firmware Fehler: %s", errMsg.c_str());
            sendTelegramMessage("❌ HANIMAT Firmware Fehler: " + errMsg);
            displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
            { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
            otaUpdateInProgress = false;
            currentSystemState  = CurrentSystemState::IDLE;
            resetDisplayToDefault();
            return;
        }

        int fwSize = fwHttp.getSize();
        logf("Full-Update: Firmware Download OK, Groesse: %d Bytes", fwSize);

        const esp_partition_t* fwPart = esp_ota_get_next_update_partition(NULL);
        if (!fwPart) {
            fwHttp.end();
            String errMsg = "Keine OTA-Partition";
            logf("Full-Update: Firmware Fehler: %s", errMsg.c_str());
            sendTelegramMessage("❌ HANIMAT Firmware Fehler: " + errMsg);
            displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
            { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
            otaUpdateInProgress = false;
            currentSystemState  = CurrentSystemState::IDLE;
            resetDisplayToDefault();
            return;
        }
        logf("Full-Update: OTA-Partition @ 0x%X", fwPart->address);

        esp_ota_handle_t otaHandle = 0;
        if (esp_ota_begin(fwPart, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK) {
            fwHttp.end();
            String errMsg = "esp_ota_begin fehlgeschlagen";
            logf("Full-Update: Firmware Fehler: %s", errMsg.c_str());
            sendTelegramMessage("❌ HANIMAT Firmware Fehler: " + errMsg);
            displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
            { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
            otaUpdateInProgress = false;
            currentSystemState  = CurrentSystemState::IDLE;
            resetDisplayToDefault();
            return;
        }

        WiFiClient* stream = fwHttp.getStreamPtr();
        size_t written = 0;
        uint8_t* fwBuf = (uint8_t*)malloc(512);
        unsigned long dlStart = millis();
        bool fwWriteError = false;

        if (fwBuf) {
            while (fwHttp.connected() && (fwSize < 0 || (int)written < fwSize)) {
                size_t avail = stream->available();
                if (avail) {
                    size_t n = stream->readBytes(fwBuf, min(avail, (size_t)512));
                    if (esp_ota_write(otaHandle, fwBuf, n) != ESP_OK) {
                        fwWriteError = true;
                        break;
                    }
                    written += n;
                    if (fwSize > 0) displayOTAProgressTFT((written * 100) / fwSize);
                    dlStart = millis();
                } else if (millis() - dlStart > 30000) {
                    break;
                }
                yield();
            }
            free(fwBuf);
        } else {
            fwWriteError = true;
        }
        fwHttp.end();

        logf("Full-Update: Firmware Bytes geschrieben: %d / %d", (int)written, fwSize);

        String fwErrMsg = "";
        if (fwWriteError) {
            fwErrMsg = "esp_ota_write Fehler";
        } else if (fwSize > 0 && (int)written != fwSize) {
            fwErrMsg = "Groesse falsch: " + String((int)written) + "/" + String(fwSize);
        } else {
            esp_err_t endErr = esp_ota_end(otaHandle);
            if (endErr != ESP_OK) {
                fwErrMsg = "esp_ota_end 0x" + String(endErr, HEX);
            } else {
                esp_err_t bootErr = esp_ota_set_boot_partition(fwPart);
                if (bootErr != ESP_OK) {
                    fwErrMsg = "set_boot 0x" + String(bootErr, HEX);
                } else {
                    // fwVerNvs mitschreiben damit setup() keinen FS-Reset auslöst
                    preferences.begin("hanimat", false);
                    preferences.putString("fwVerNvs", remoteFw);
                    preferences.end();
                    logMessage("Full-Update: Firmware OK.");
                    fwOk = true;
                }
            }
        }
        if (!fwOk && fwErrMsg.isEmpty()) fwErrMsg = "Unbekannt";

        if (!fwOk) {
            if (fwWriteError || (fwSize > 0 && (int)written != fwSize)) esp_ota_abort(otaHandle);
            String errMsg = fwErrMsg;
            logf("Full-Update: Firmware Fehler: %s", errMsg.c_str());
            sendTelegramMessage("❌ HANIMAT Firmware Fehler: " + errMsg);
            displayErrorMessage("UPDATE FEHLER", errMsg.substring(0, 25));
            { unsigned long _t = millis() + 4000; while (millis() < _t) { server.handleClient(); yield(); } }
            otaUpdateInProgress = false;
            currentSystemState  = CurrentSystemState::IDLE;
            resetDisplayToDefault();
            return;
        }
    }

    // ── Neustart nach erfolgreichem Update ──
    if (fsOk && fwOk) {
        String doneMsg = "✅ HANIMAT Update OK:";
        if (doFs) doneMsg += " WebIF " + fsVersion;
        if (doFw) doneMsg += " Firmware " + remoteFw;
        sendTelegramMessage(doneMsg);
        displayOTAMessageTFT("SYSTEM UPDATE", "Update OK!", "Neustart...");
        delay(1500);
        ESP.restart();
    }
}

/**
 * @brief Verarbeitet nicht-blockierende Relay-Testjobs (Einzel- und Sequenztest).
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
 * @brief Serves the dashboard JavaScript from LittleFS (/app.js).
 */
void handleAppJs() {
  if (!LittleFS.exists("/app.js")) {
    server.send(404, "text/plain", "app.js not found on LittleFS");
    return;
  }
  File f = LittleFS.open("/app.js", "r");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.streamFile(f, "text/javascript; charset=UTF-8");
  f.close();
}


void handleStyleCss() {
  if (!LittleFS.exists("/style.css")) {
    server.send(404, "text/plain", "style.css not found on LittleFS");
    return;
  }
  File f = LittleFS.open("/style.css", "r");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.streamFile(f, "text/css; charset=UTF-8");
  f.close();
}

// =================================================================
//  API HANDLER: /api/status — liefert Live-Daten als JSON
// =================================================================
void handleApiStatus() {
  if (!isAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  lastActivityTimeWeb = millis();

  // Uptime berechnen
  unsigned long sec = millis() / 1000;
  unsigned long m = sec / 60; sec %= 60;
  unsigned long h = m / 60; m %= 60;
  unsigned long d = h / 24; h %= 24;
  char uptime[32];
  snprintf(uptime, sizeof(uptime), "%lud %02lu:%02lu:%02lu", d, h, m, sec);

  // Slots als JSON Array
  String slotsJson = "[";
  for (int i = 0; i < activeSlots; i++) {
    if (i > 0) slotsJson += ",";
    slotsJson += "{\"price\":\"" + centsToEurStr(slotPriceCents[i]) + "\","
               + "\"available\":" + (slotAvailable[i] ? "true" : "false") + ","
               + "\"locked\":" + (slotLocked[i] ? "true" : "false") + "}";
  }
  slotsJson += "]";

  // Gesamtverkäufe
  int totalSalesAll = 0;
  for (int i = 0; i < activeSlots; i++) totalSalesAll += slotSalesCount[i];

  // Reset-Grund JSON-sicher machen
  String safeReason = lastResetReason;
  safeReason.replace("\\", "\\\\");
  safeReason.replace("\"", "\\\"");

  String json = "{";
  json += "\"version\":\""   + String(FIRMWARE_VERSION) + "\",";
  json += "\"fsVersion\":\"" + installedFsVersion       + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"heapKb\":" + String(ESP.getFreeHeap() / 1024) + ",";
  json += "\"minHeapKb\":" + String(ESP.getMinFreeHeap() / 1024) + ",";
  json += "\"uptime\":\"" + String(uptime) + "\",";
  json += "\"creditEur\":\"" + centsToEurStr(creditCents) + "\",";
  json += "\"activeSlots\":" + String(activeSlots) + ",";
  json += "\"crashCount\":" + String(crashCount) + ",";
  json += "\"unexpectedReset\":" + String(wasUnexpectedReset ? "true" : "false") + ",";
  json += "\"resetReason\":\"" + safeReason + "\",";
  json += "\"cashBoxEur\":\"" + centsToEurStr(cashBoxCents) + "\",";
  json += "\"totalRevenueEur\":\"" + centsToEurStr(totalRevenueCents) + "\",";
  json += "\"totalSales\":" + String(totalSalesAll) + ",";
  json += "\"slots\":" + slotsJson;
  json += "}";

  server.send(200, "application/json", json);
}

// =================================================================
//  API HANDLER: /api/config — liefert Konfigurationsdaten als JSON
// =================================================================
void handleApiConfig() {
  if (!isAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  lastActivityTimeWeb = millis();

  // JSON-String escapen (Anführungszeichen, Backslash)
  auto jStr = [](const String& s) -> String {
    String r = "\"";
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if      (c == '"')  r += "\\\"";
      else if (c == '\\') r += "\\\\";
      else                r += c;
    }
    r += "\"";
    return r;
  };
  auto jBool = [](bool b) -> String { return b ? "true" : "false"; };

  String json = "{";

  // --- Zeitsteuerung ---
  json += "\"coin_delay\":"         + String(COIN_PROCESSING_DELAY)            + ",";
  json += "\"bill_isr_debounce\":"  + String(BILL_ISR_DEBOUNCE_MS)             + ",";
  json += "\"bill_group_timeout\":" + String(BILL_GROUP_PROCESSING_TIMEOUT_MS) + ",";
  json += "\"disp_time\":"          + String(DISPENSE_RELAY_ON_TIME)            + ",";
  json += "\"keypad_time\":"        + String(KEYPAD_INPUT_TIMEOUT)              + ",";
  json += "\"slot_sel_time\":"      + String(SLOT_SELECTION_TIMEOUT)            + ",";
  json += "\"disp_timeout\":"       + String(DISPLAY_TIMEOUT)                   + ",";
  json += "\"web_timeout\":"        + String(WEB_TIMEOUT / 1000UL)              + ",";
  json += "\"status_enabled\":"     + jBool(statusEnabled)                      + ",";

  // --- Telegram ---
  json += "\"tg_enabled\":"              + jBool(telegramEnabled)           + ",";
  json += "\"tg_token\":"                + jStr(telegramBotToken)           + ",";
  json += "\"tg_chat_id\":"             + jStr(telegramChatId)              + ",";
  json += "\"notify_sale\":"            + jBool(telegramNotifyOnSale)       + ",";
  json += "\"notify_almost_empty\":"    + jBool(telegramNotifyAlmostEmpty)  + ",";
  json += "\"almost_empty_threshold\":" + String(almostEmptyThreshold)      + ",";
  json += "\"notify_empty\":"           + jBool(telegramNotifyEmpty)        + ",";
  json += "\"notify_crash\":"           + jBool(telegramNotifyCrash)        + ",";
  json += "\"notify_bruteforce\":"      + jBool(telegramNotifyBruteForce)   + ",";

  // --- Display ---
  json += "\"slogan\":"              + jStr(displaySlogan)    + ",";
  json += "\"footer\":"              + jStr(displayFooter)    + ",";
  json += "\"display_white_mode\":"  + jBool(displayWhiteMode) + ",";

  // --- SumUp ---
  json += "\"sumup_enabled\":"    + jBool(sumupEnabled)           + ",";
  json += "\"sumup_apiKey\":"     + jStr(sumupApiKey)             + ",";
  json += "\"sumup_merchantId\":" + jStr(sumupMerchantId)         + ",";
  json += "\"sumup_readerId\":"   + jStr(sumupReaderId)           + ",";
  json += "\"sumup_timeout\":"    + String(sumupTimeout / 1000UL) + ",";

  // --- Netzwerk (aus NVS lesen) ---
  preferences.begin("hanimat", true);
  String sip = preferences.getString("static_ip", "");
  String gw  = preferences.getString("gateway",   "");
  String sn  = preferences.getString("subnet",    "");
  String d1  = preferences.getString("dns1",      "8.8.8.8");
  preferences.end();
  json += "\"static_ip\":" + jStr(sip) + ",";
  json += "\"gateway\":"   + jStr(gw)  + ",";
  json += "\"subnet\":"    + jStr(sn)  + ",";
  json += "\"dns1\":"      + jStr(d1)  + ",";

  // --- Zahlungs-Pulse (denomination-centric) ---
  json += "\"coinEnabled\":" + jBool(coinAcceptorEnabled) + ",";
  json += "\"billEnabled\":" + jBool(billAcceptorEnabled) + ",";
  const int  cDenoms[] = {1,2,5,10,20,50,100,200};
  const char* cKeys[]  = {"coin_1","coin_2","coin_5","coin_10","coin_20","coin_50","coin_100","coin_200"};
  for (int d = 0; d < 8; d++) {
    int fp = 0;
    for (int p = 1; p <= 6; p++) { if (pulseValues[p] == cDenoms[d]) { fp = p; break; } }
    json += "\"" + String(cKeys[d]) + "\":" + String(fp) + ",";
  }
  const int  bDenoms[] = {5,10,20,50,100};
  const char* bKeys[]  = {"bill_5","bill_10","bill_20","bill_50","bill_100"};
  for (int d = 0; d < 5; d++) {
    int fp = 0;
    for (int p = 1; p <= 16; p++) { if (billValues[p] == bDenoms[d]) { fp = p; break; } }
    json += "\"" + String(bKeys[d]) + "\":" + String(fp) + ",";
  }

  // --- Slots ---
  json += "\"activeSlots\":" + String(activeSlots) + ",";
  json += "\"slotPrices\":[";
  for (int i = 0; i < activeSlots; i++) {
    if (i > 0) json += ",";
    json += String(slotPriceCents[i]);
  }
  json += "]";  // kein Komma – letztes Feld

  json += "}";
  server.send(200, "application/json", json);
}

/**
 * @brief Sets up all web server endpoints (routes).
 */
void handleResetCashBox();  // forward declaration
void handlePaymentConfig();
void handleSavePaymentConfig();
void handleOTAFileUploadFs();
void setupWebServer() {
  // Cookie-Header einlesen (nötig für Session-Token-Prüfung)
  const char* headerKeys[] = { "Cookie" };
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/app.js",    HTTP_GET, handleAppJs);
  server.on("/style.css", HTTP_GET, handleStyleCss);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/resetcrashcount", HTTP_POST, handleResetCrashCount);
  server.on("/resetsalesstats", HTTP_POST, handleResetSalesStats);
  server.on("/resetcashbox",   HTTP_POST, handleResetCashBox);
  server.on("/paymentconfig",  HTTP_GET,  handlePaymentConfig);
  server.on("/savepaymentconfig", HTTP_POST, handleSavePaymentConfig);
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
  
  // Online Update Routen
  server.on("/check-online-update", HTTP_GET,  handleCheckOnlineUpdate);
  server.on("/start-full-update",   HTTP_POST, handleStartFullUpdate);

  // API Routen (JSON Daten für LittleFS SPA)
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/config", HTTP_GET, handleApiConfig);

  // OTA Upload Handler (Firmware)
  server.on("/ota-upload", HTTP_POST, []() {
    server.sendHeader("Location", "/otaupdate", true);
    server.send(302, "text/plain", "");
  }, handleOTAFileUpload);

  // OTA Upload Handler (LittleFS)
  server.on("/ota-upload-fs", HTTP_POST, []() {
    server.sendHeader("Location", "/otaupdate", true);
    server.send(302, "text/plain", "");
  }, handleOTAFileUploadFs);

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
  // RAM-Ringpuffer und NVS-Log leeren
  saleLogCount = 0;
  saleLogNext  = 0;
  preferences.begin("hanimat", false);
  preferences.remove("saleLogData");
  preferences.remove("saleLogCount");
  preferences.remove("saleLogNext");
  preferences.end();
  logMessage("Web: Umsatz- und Verkaufsstatistik zurückgesetzt.");
  server.send(200, "text/html", "Statistik zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/#saleslog-section' />");
}

/**
 * @brief Setzt den Kassenstand (cashBoxCents) auf 0 zurück.
 *        Wird aufgerufen wenn die Kasse geleert wurde.
 */
void handleResetCashBox() {
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  cashBoxCents = 0;
  preferences.begin("hanimat", false);
  preferences.putInt("cashBox", 0);
  preferences.end();
  logMessage("Web: Kassenstand zurückgesetzt.");
  server.send(200, "text/html", "Kassenstand zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/#saleslog-section' />");
}

/**
 * @brief Speichert neue WLAN-Zugangsdaten in NVS und startet den ESP32 neu.
 *        In setup() werden die Daten ausgelesen und über WiFiManager gesetzt.
 */
void handlePaymentConfig() {
  server.sendHeader("Location", "/");
  server.send(302);
}
void handleSavePaymentConfig() {
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  // Denomination-centric coin mapping: coin_1, coin_2, coin_5, ..., coin_200
  const int  sCoinDenoms[]  = {1, 2, 5, 10, 20, 50, 100, 200};
  const char* sCoinFields[] = {"coin_1","coin_2","coin_5","coin_10","coin_20","coin_50","coin_100","coin_200"};
  memset(pulseValues, 0, sizeof(pulseValues));
  for (int d = 0; d < 8; d++) {
    if (server.hasArg(sCoinFields[d])) {
      int pulses = server.arg(sCoinFields[d]).toInt();
      if (pulses >= 1 && pulses <= 6) {
        pulseValues[pulses] = sCoinDenoms[d];
      }
    }
  }
  // Denomination-centric bill mapping: bill_5, bill_10, bill_20, bill_50, bill_100
  const int billDenoms[]    = {5, 10, 20, 50, 100};
  const char* billFlds[]    = {"bill_5", "bill_10", "bill_20", "bill_50", "bill_100"};
  memset(billValues, 0, sizeof(billValues)); // alle löschen, dann neu setzen
  for (int d = 0; d < 5; d++) {
    if (server.hasArg(billFlds[d])) {
      int pulses = server.arg(billFlds[d]).toInt();
      if (pulses >= 1 && pulses <= 16) {
        billValues[pulses] = billDenoms[d];
      }
    }
  }
  coinAcceptorEnabled = server.hasArg("coinEnabled");
  billAcceptorEnabled = server.hasArg("billEnabled");
  // Scheinprüfer-Inhibit sofort anwenden
  digitalWrite(BILL_INHIBIT_PIN, billAcceptorEnabled ? LOW : HIGH);
  preferences.begin("hanimat", false);
  preferences.putBytes("coinPulses", pulseValues, sizeof(pulseValues));
  preferences.putBytes("billPulses", billValues,  sizeof(billValues));
  preferences.putBool("coinEnabled", coinAcceptorEnabled);
  preferences.putBool("billEnabled", billAcceptorEnabled);
  preferences.end();
  logMessage("Web: Zahlungs-Pulse-Mapping gespeichert.");
  server.send(200, "text/html", "Gespeichert. <meta http-equiv='refresh' content='1;url=/#payment-config' />");
}

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
    displayOTAMessageTFT("Update gestartet", "Nicht ausschalten!");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      logf("OTA ERROR: Update.begin() failed. Error: %d", Update.getError());
      otaStatusMessage = "ERROR: Could not start update (Error: " + String(Update.getError()) + ")";
      displayOTAMessageTFT("Update Fehler!", "Start fehlgeschlagen", "Details im Log");
      otaUpdateInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      logf("OTA ERROR: Update.write() failed. Error: %d", Update.getError());
      otaStatusMessage = "ERROR: Failed to write firmware (Error: " + String(Update.getError()) + ")";
      displayOTAMessageTFT("Update Fehler!", "Schreibfehler", "Details im Log");
      otaUpdateInProgress = false;
      Update.end(false);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUpdateInProgress) {
        if (Update.end(true)) {
            otaStatusMessage = "Update successful! ESP32 is restarting...";
            logMessage("OTA: Update finished successfully. Restarting ESP32.");
            displayOTAMessageTFT("Update fertig.", "Automat startet neu");
            server.sendHeader("Location", "/otaupdate", true);
            server.send(302, "text/plain", "Update successful, restarting...");
            delay(3000);
            ESP.restart();
        } else {
            Update.printError(Serial);
            logf("OTA ERROR: Update.end() failed. Error: %d", Update.getError());
            otaStatusMessage = "ERROR: Update failed (Error: " + String(Update.getError()) + ")";
            displayOTAMessageTFT("Update Fehler!", "Abschluss fehlgeschl.", "Details im Log");
        }
    }
    otaUpdateInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
      logMessage("OTA: Upload aborted by client.");
      if(otaUpdateInProgress) Update.end(false);
      otaUpdateInProgress = false;
  }
}

void handleOTAFileUploadFs() {
  if (!isAuth()) return;
  lastActivityTimeWeb = millis();
  HTTPUpload& upload = server.upload();

  static const esp_partition_t* fsUploadPart = nullptr;
  static uint8_t* fsUploadBuf = nullptr;
  static size_t   fsUploadBufPos = 0;
  static size_t   fsUploadWritten = 0;
  static bool     fsUploadError = false;

  if (upload.status == UPLOAD_FILE_START) {
    otaUpdateInProgress = true;
    fsUploadError   = false;
    fsUploadBufPos  = 0;
    fsUploadWritten = 0;
    logf("OTA-FS: Upload gestartet: %s", upload.filename.c_str());
    displayOTAMessageTFT("WebIF Update", "Nicht ausschalten!");

    fsUploadPart = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (!fsUploadPart) {
      logMessage("OTA-FS ERROR: Partition nicht gefunden.");
      displayOTAMessageTFT("WebIF Fehler!", "Partition fehlt", "Details im Log");
      fsUploadError = true;
      otaUpdateInProgress = false;
      return;
    }
    LittleFS.end();
    if (esp_partition_erase_range(fsUploadPart, 0, fsUploadPart->size) != ESP_OK) {
      logMessage("OTA-FS ERROR: Partition loeschen fehlgeschlagen.");
      displayOTAMessageTFT("WebIF Fehler!", "Erase fehlgeschl.", "Details im Log");
      fsUploadError = true;
      otaUpdateInProgress = false;
      return;
    }
    fsUploadBuf = (uint8_t*)malloc(512);
    if (!fsUploadBuf) {
      logMessage("OTA-FS ERROR: malloc fehlgeschlagen.");
      fsUploadError = true;
      otaUpdateInProgress = false;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (fsUploadError || !fsUploadBuf) return;
    size_t remaining = upload.currentSize;
    uint8_t* src = upload.buf;
    while (remaining > 0) {
      size_t space = 512 - fsUploadBufPos;
      size_t copy  = min(remaining, space);
      memcpy(fsUploadBuf + fsUploadBufPos, src, copy);
      fsUploadBufPos += copy;
      src            += copy;
      remaining      -= copy;
      if (fsUploadBufPos == 512) {
        if (esp_partition_write(fsUploadPart, fsUploadWritten, fsUploadBuf, 512) != ESP_OK) {
          logMessage("OTA-FS ERROR: Schreibfehler.");
          displayOTAMessageTFT("WebIF Fehler!", "Schreibfehler", "Details im Log");
          fsUploadError = true;
          return;
        }
        fsUploadWritten += 512;
        fsUploadBufPos   = 0;
      }
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (!fsUploadError && fsUploadBuf) {
      if (fsUploadBufPos > 0) {
        size_t toWrite = (fsUploadBufPos + 3) & ~3u;
        memset(fsUploadBuf + fsUploadBufPos, 0xFF, toWrite - fsUploadBufPos);
        if (esp_partition_write(fsUploadPart, fsUploadWritten, fsUploadBuf, toWrite) != ESP_OK) {
          logMessage("OTA-FS ERROR: Letzter Schreibfehler.");
          fsUploadError = true;
        } else {
          fsUploadWritten += fsUploadBufPos;
        }
      }
    }
    if (fsUploadBuf) { free(fsUploadBuf); fsUploadBuf = nullptr; }
    if (!fsUploadError) {
      preferences.begin("hanimat", false);
      preferences.putString("fsFwVer", FS_VERSION);
      preferences.end();
      installedFsVersion = String(FS_VERSION);
      logMessage("OTA-FS: LittleFS erfolgreich geflasht. Neustart...");
      displayOTAMessageTFT("WebIF Update", "Erfolgreich!", "Neustart...");
      delay(2000);
      ESP.restart();
    }
    otaUpdateInProgress = false;

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (fsUploadBuf) { free(fsUploadBuf); fsUploadBuf = nullptr; }
    logMessage("OTA-FS: Upload abgebrochen.");
    otaUpdateInProgress = false;
    fsUploadError = false;
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
  displaySlogan    = newSlogan;
  displayFooter    = newFooter;
  displayWhiteMode = (server.arg("display_white_mode") == "1");

  // Im Speicher sichern
  preferences.begin("hanimat", false);
  preferences.putString("dispSlogan", displaySlogan);
  preferences.putString("dispFooter", displayFooter);
  preferences.putBool("dispWhite",    displayWhiteMode);
  preferences.end();

  applyLVGLTheme();
  lastActivityTimeWeb = millis();
  lastDrawnMode = DrawnMode::NONE;
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
body { height: 100vh; display: flex; flex-direction: column; align-items: center; justify-content: center; font-family: system-ui, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; box-sizing: border-box; }
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
 * @brief Serviert index.html direkt aus LittleFS via streamFile (gepuffert, schnell).
 *        Konfigurationsdaten werden per /api/config und /api/status nachgeladen (app.js).
 */
void showDashboard() {
  if (!LittleFS.exists("/index.html")) {
    server.send(500, "text/plain",
      "Web-Interface fehlt! LittleFS nicht geflasht? "
      "Bitte 'Upload Filesystem Image' ausfuehren.");
    return;
  }
  File f = LittleFS.open("/index.html", "r");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.streamFile(f, "text/html; charset=UTF-8");
  f.close();
}

// =================================================================
//  LEGACY showDashboard() — vollständiger HTML-String (als Fallback / Referenz)
//  Dieser Block wird nicht mehr aufgerufen; er bleibt für Referenzzwecke.
//  Auskommentiert um Speicher zu sparen.
// =================================================================
#if 0
void showDashboard_legacy() {
  String html;
  html.reserve(72000); // CSS extern (/style.css)
  html = R"HTML(
<!DOCTYPE html><html lang='de'><head><title>Hanimat Control</title>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>
<script src='/app.js' defer></script>
<link rel="stylesheet" href="/style.css">
</head><body>

<!-- Mobile UI -->
<div class='overlay' data-action='toggle-menu'></div>
<div class='mobile-header'>
  <div class='logo' style='font-size:1.4rem'>HANI<span>MAT</span></div>
  <button class='menu-toggle' data-action='toggle-menu'>&#9776;</button>
</div>

<!-- Sidebar -->
<nav class='sidebar'>
  <div class='brand-header'>
    <div class='logo'>HANI<span>MAT</span></div>
    <div class='logo-sub'>Thomas Schöpf</div>
  </div>
  <ul class='nav-list'>
    <li><button class='nav-btn active' data-go='dashboard'><span class='nav-icon'>&#128202;</span> Dashboard</button></li>
    <li><button class='nav-btn' data-go='slots-config'><span class='nav-icon'>&#9881;</span> Slot Config</button></li>
    <li><button class='nav-btn' data-go='display-config'><span class='nav-icon'>&#128187;</span> Anzeige</button></li>
    <li><button class='nav-btn' data-go='timing-config'><span class='nav-icon'>&#9201;</span> Zeitsteuerung</button></li>
    <li><button class='nav-btn' data-go='telegram-config'><span class='nav-icon'>&#9993;</span> Telegram</button></li>
    <li><button class='nav-btn' data-go='payment-config'><span class='nav-icon'>&#x1F4B1;</span> Zahlungs-Setup</button></li>
    <li><button class='nav-btn' data-go='sumup-config'><span class='nav-icon'>&#128179;</span> SumUp</button></li>
    <li><button class='nav-btn' data-go='network-config'><span class='nav-icon'>&#128423;</span> Netzwerk</button></li>
    <li><button class='nav-btn' data-go='password-config'><span class='nav-icon'>&#128274;</span> Sicherheit</button></li>
    <li><button class='nav-btn' data-go='saleslog-section'><span class='nav-icon'>&#128202;</span> Verkaufsstatistik</button></li>
    <li><button class='nav-btn' data-go='logs'><span class='nav-icon'>&#128466;</span> Logs</button></li>
    <li><button class='nav-btn' data-go='ota-update-section'><span class='nav-icon'>&#128229;</span> Update</button></li>
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
      <div class='stat-box' style='border:1px solid rgba(255,180,0,0.35); background:rgba(255,180,0,0.07);'>
        <div class='stat-val stat-highlight' style='font-size:1.25rem;'>)HTML" + centsToEurStr(cashBoxCents) + R"HTML( &euro;</div>
        <div class='stat-lbl'>&#x1F4B0; Kassenstand (Bar)</div>
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
      <form action='/resetcashbox' method='post' data-confirm='Kassenstand auf 0 setzen?' style='margin:0;'>
        <button type='submit' class='btn-sec' style='color:#f0a000; border-color:#f0a000;'>&#x1F4B0; Kasse geleert</button>
      </form>
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
      <button type='button' data-action='apply-bulk' class='btn-sec' style='height:46px; white-space:nowrap;'>&#128256; Alle übernehmen</button>
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
              <i class='info-icon' >i<span class='tip'>Wartezeit nach dem Einwurf einer Münze, bevor die Impulse ausgewertet werden. Bei Rauschproblemen oder Fehlzählungen erhöhen. Standardwert: 150 ms</span></i>
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
              <i class='info-icon' >i<span class='tip'>Mindestabstand zwischen zwei Impuls-Flanken des Scheinprüfers. Verhindert Doppelzählungen durch Prellen. Standardwert: 75 ms</span></i>
            </div>
            <input type='number' name='bill_isr_debounce' min='0' max='500' value=')HTML" + String(BILL_ISR_DEBOUNCE_MS) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Scheingruppen Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' >i<span class='tip'>Wartezeit nach dem letzten Impuls eines Scheins, bis der Gesamtwert verarbeitet wird. Muss größer sein als die Lücke zwischen den Impulsen eines Scheins. Standardwert: 1500 ms</span></i>
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
              <i class='info-icon' >i<span class='tip'>Wie lange das Ausgabe-Relais aktiviert bleibt (= Motorlaufzeit). Zu kurz &rarr; Produkt wird nicht ausgegeben. Standardwert: 5000 ms</span></i>
            </div>
            <input type='number' name='disp_time' min='500' max='30000' value=')HTML" + String(DISPENSE_RELAY_ON_TIME) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Tastatureingabe Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' >i<span class='tip'>Wartezeit auf eine zweite Ziffer nach dem ersten Tastendruck. Standardwert: 3000 ms</span></i>
            </div>
            <input type='number' name='keypad_time' min='500' max='15000' value=')HTML" + String(KEYPAD_INPUT_TIMEOUT) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Fachauswahl Timeout
              <span class='unit'>ms</span>
              <i class='info-icon' >i<span class='tip'>Wie lange ein gew&auml;hltes Fach aktiv bleibt, ohne dass der Kauf best&auml;tigt wird. Standardwert: 10000 ms</span></i>
            </div>
            <input type='number' name='slot_sel_time' min='2000' max='60000' value=')HTML" + String(SLOT_SELECTION_TIMEOUT) + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>
              Display Ruhemodus
              <span class='unit'>ms</span>
              <i class='info-icon' >i<span class='tip'>Nach dieser Inaktivit&auml;tsdauer kehrt das Display automatisch zum Startbildschirm zur&uuml;ck. Standardwert: 20000 ms</span></i>
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
              <i class='info-icon' >i<span class='tip'>Nach dieser Zeit ohne Aktivit&auml;t im Web-Interface wird die Anmeldung automatisch beendet. Standardwert: 600 s</span></i>
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
              <i class='info-icon' >i<span class='tip'>Den Token erh&auml;ltst du vom @BotFather auf Telegram.</span></i>
            </div>
            <input type='password' name='tg_token' value=')HTML" + telegramBotToken + R"HTML('>
          </div>
          <div class='input-group'>
            <div class='timing-label'>Chat ID
              <i class='info-icon' >i<span class='tip'>Die numerische ID deines Chats. Mit @userinfobot herausfinden.</span></i>
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
  // CONFIG SUMUP - vor network-config, damit der Block bei knappem Heap nicht abgeschnitten wird
  html += R"HTML(<section id='sumup-config' class='page' style='display:none;'>
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
  </section>)HTML";

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
          <button type='button' id='btnDhcp' data-action='set-net' data-mode='dhcp'
            style='flex:1; padding:0.75rem; border-radius:10px; border:2px solid; cursor:pointer; font-weight:700; font-size:0.9rem; transition:0.2s;)HTML"
            + String(staticIP_val.length() == 0 ? "background:var(--success);color:#000;border-color:var(--success);" : "background:transparent;color:var(--text-sec);border-color:var(--border);")
            + R"HTML('>
            &#127760; DHCP
          </button>
          <button type='button' id='btnStatic' data-action='set-net' data-mode='static'
            style='flex:1; padding:0.75rem; border-radius:10px; border:2px solid; cursor:pointer; font-weight:700; font-size:0.9rem; transition:0.2s;)HTML"
            + String(staticIP_val.length() > 0 ? "background:var(--brand);color:#000;border-color:var(--brand);" : "background:transparent;color:var(--text-sec);border-color:var(--border);")
            + R"HTML('>
            &#128204; Statische IP
          </button>
        </div>
        <div id='staticFields' style='display:)HTML" + String(staticIP_val.length() > 0 ? "" : "none") + R"HTML(;'>
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
      <form action='/setwifi' method='post' data-confirm='WLAN wirklich wechseln? Das Gerät startet neu.'>
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
            <button data-action='check-update' class='btn-sec' style='width:auto;'>Version Pr&uuml;fen</button>
            <form action='/start-full-update' method='post' id='update-form' style='display:none;'>
                <button type='submit' class='btn-main' style='width:auto;'>Update Starten</button>
            </form>
         </div>
       </div>
       <h2>Datei Upload</h2>
       <p style='color:var(--muted); margin-bottom:1rem; font-size:0.9rem;'>Firmware: <code>firmware.bin</code> &nbsp;|&nbsp; WebIF: <code>littlefs.bin</code></p>
       <div style='display:flex; gap:1.5rem; flex-wrap:wrap;'>
         <div style='flex:1; min-width:220px;'>
           <h3 style='margin-bottom:0.75rem;'>Firmware</h3>
           <form method='POST' action='/ota-upload' enctype='multipart/form-data'>
             <div class='input-group'>
               <input type='file' name='update' accept='.bin' required style='padding:1rem;'>
             </div>
             <button type='submit' class='btn-main'>Firmware flashen</button>
           </form>
         </div>
         <div style='flex:1; min-width:220px;'>
           <h3 style='margin-bottom:0.75rem;'>WebIF (LittleFS)</h3>
           <form method='POST' action='/ota-upload-fs' enctype='multipart/form-data'>
             <div class='input-group'>
               <input type='file' name='update' accept='.bin' required style='padding:1rem;'>
             </div>
             <button type='submit' class='btn-sec'>WebIF flashen</button>
           </form>
         </div>
       </div>
    </div>
  </section>

  <!-- VERKAUFSSTATISTIK -->
  <section id='saleslog-section' class='page' style='display:none;'>
    <div class='top-bar'><h1>&#128202; Verkaufsstatistik</h1></div>

    <!-- Summary Cards -->
    <div class='stats-grid' style='margin-bottom:1.5rem; grid-template-columns:repeat(auto-fit,minmax(130px,1fr));'>
      <div class='stat-box' style='text-align:center;'>
        <div class='stat-val stat-highlight' style='font-size:1.4rem;'>)HTML"; html += centsToEurStr(totalRevenueCents) + R"HTML( &euro;</div>
        <div class='stat-lbl'>Gesamtumsatz</div>
      </div>
      <div class='stat-box' style='text-align:center;'>
        <div class='stat-val' style='font-size:1.4rem;'>)HTML";
        { int total = 0; for(int i=0;i<activeSlots;i++) total+=slotSalesCount[i]; html += String(total); }
        html += R"HTML(</div>
        <div class='stat-lbl'>Verk&auml;ufe gesamt</div>
      </div>
      <div class='stat-box' style='text-align:center;'>
        <div class='stat-val' id='sl-cash' style='font-size:1.4rem; color:var(--brand);'>-</div>
        <div class='stat-lbl'>&#x1F4B5; Bar (letzte 50)</div>
      </div>
      <div class='stat-box' style='text-align:center;'>
        <div class='stat-val' id='sl-card' style='font-size:1.4rem; color:#00b478;'>-</div>
        <div class='stat-lbl'>&#x1F4B3; Karte (letzte 50)</div>
      </div>
      <div class='stat-box' style='text-align:center; border:1px solid rgba(255,180,0,0.35); background:rgba(255,180,0,0.07);'>
        <div class='stat-val stat-highlight' style='font-size:1.4rem;'>)HTML" + centsToEurStr(cashBoxCents) + R"HTML( &euro;</div>
        <div class='stat-lbl'>&#x1F4B0; Kassenstand</div>
      </div>
    </div>

    <!-- Action Buttons -->
    <div style='display:flex; gap:0.6rem; flex-wrap:wrap; margin-bottom:1.25rem;'>
      <form action='/resetcashbox' method='post' data-confirm='Kassenstand wirklich auf 0 setzen?' style='margin:0;'>
        <button type='submit' class='btn-sec' style='color:#f0a000; border-color:#f0a000; width:auto; padding:0.4rem 1rem; font-size:0.82rem;'>&#x1F4B0; Kasse geleert</button>
      </form>
      <form action='/resetsalesstats' method='post' data-confirm='Alle Verkaufsstatistiken wirklich löschen?' style='margin:0;'>
        <button type='submit' class='btn-sec' style='color:var(--danger); border-color:var(--danger); width:auto; padding:0.4rem 1rem; font-size:0.82rem;'>&#128465; Statistik l&ouml;schen</button>
      </form>
    </div>

    <!-- Sales Table -->
    <div class='stat-box'>
      <div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:0.75rem; flex-wrap:wrap; gap:0.5rem;'>
        <div style='font-weight:700; font-size:0.9rem;'>Letzte Verk&auml;ufe <span id='sl-count-label' style='font-weight:400; color:var(--text-sec); font-size:0.8rem;'></span></div>
        <div style='font-size:0.75rem; color:var(--text-sec); background:rgba(255,255,255,0.06); padding:3px 10px; border-radius:8px;'>max. 50 Eintr&auml;ge</div>
      </div>
      <div style='overflow-x:auto;'>
        <table id='sales-table' style='width:100%; border-collapse:collapse; font-size:0.87rem;'>
          <thead>
            <tr style='color:var(--text-sec); border-bottom:2px solid var(--border);'>
              <th style='text-align:left; padding:8px 10px; font-weight:600;'>Zeit</th>
              <th style='text-align:center; padding:8px 10px; font-weight:600;'>Fach</th>
              <th style='text-align:right; padding:8px 10px; font-weight:600;'>Preis</th>
              <th style='text-align:center; padding:8px 10px; font-weight:600;'>Zahlung</th>
            </tr>
          </thead>
          <tbody id='sales-tbody'>
            <tr><td colspan='4' style='padding:1.5rem; color:var(--text-sec); text-align:center;'>Lade Daten...</td></tr>
          </tbody>
        </table>
      </div>
      <div id='sales-empty' style='display:none; text-align:center; color:var(--text-sec); padding:2rem; font-size:0.9rem;'>&#128203; Noch keine Verk&auml;ufe</div>
    </div>
  </section>

  <!-- CONFIG ZAHLUNGS-PULSE -->
  <section id='payment-config' class='page' style='display:none;'>
    <div class='top-bar'><h1>&#x1F4B1; Zahlungs-Setup</h1></div>
    <form action='/savepaymentconfig' method='post'>

    <!-- Zahlungskanäle -->
    <div class='stat-box' style='margin-bottom:1rem;'>
      <h2 style='margin:0 0 1rem 0; font-size:1rem; color:var(--text-sec); font-weight:600; letter-spacing:0.04em; text-transform:uppercase;'>&#x26A1; Zahlungskan&auml;le</h2>
      <div style='display:grid; grid-template-columns:1fr 1fr; gap:1rem;'>
        <label style='display:flex; flex-direction:column; align-items:center; gap:0.5rem; background:var(--card-bg); border:2px solid )HTML" + String(coinAcceptorEnabled ? "var(--brand)" : "var(--border)") + R"HTML(; border-radius:12px; padding:1.25rem 1rem; cursor:pointer; transition:border-color 0.2s;'>
          <span style='font-size:2rem;'>&#x1FA99;</span>
          <span style='font-weight:700; font-size:0.95rem;'>M&uuml;nzpr&uuml;fer</span>
          <input type='checkbox' name='coinEnabled' )HTML" + String(coinAcceptorEnabled ? "checked" : "") + R"HTML( style='width:1.2rem; height:1.2rem; accent-color:var(--brand); margin:0;'>
          <span style='font-size:0.78rem; font-weight:600; color:)HTML" + String(coinAcceptorEnabled ? "var(--success)" : "var(--text-sec)") + R"HTML(;'>)HTML" + String(coinAcceptorEnabled ? "&#x2714; Aktiv" : "&#x25CB; Inaktiv") + R"HTML(</span>
        </label>
        <label style='display:flex; flex-direction:column; align-items:center; gap:0.5rem; background:var(--card-bg); border:2px solid )HTML" + String(billAcceptorEnabled ? "var(--brand)" : "var(--border)") + R"HTML(; border-radius:12px; padding:1.25rem 1rem; cursor:pointer; transition:border-color 0.2s;'>
          <span style='font-size:2rem;'>&#x1F4B5;</span>
          <span style='font-weight:700; font-size:0.95rem;'>Scheinpr&uuml;fer</span>
          <input type='checkbox' name='billEnabled' )HTML" + String(billAcceptorEnabled ? "checked" : "") + R"HTML( style='width:1.2rem; height:1.2rem; accent-color:var(--brand); margin:0;'>
          <span style='font-size:0.78rem; font-weight:600; color:)HTML" + String(billAcceptorEnabled ? "var(--success)" : "var(--text-sec)") + R"HTML(;'>)HTML" + String(billAcceptorEnabled ? "&#x2714; Aktiv" : "&#x25CB; Inaktiv") + R"HTML(</span>
        </label>
      </div>
    </div>

    <!-- Münzprüfer -->
    <div class='stat-box' style='margin-bottom:1rem;'>
      <h2 style='margin:0 0 0.25rem 0; font-size:1rem; color:var(--text-sec); font-weight:600; letter-spacing:0.04em; text-transform:uppercase;'>&#x1FA99; M&uuml;nzpr&uuml;fer &mdash; M&uuml;nze &rarr; Pulse</h2>
      <p style='color:var(--text-sec); font-size:0.8rem; margin:0 0 1rem 0;'>Pulse-Anzahl je M&uuml;nze. 0 = nicht aktiv.</p>
      <div style='display:flex; flex-direction:column; gap:0.6rem;'>
  )HTML";

  const int coinDenoms[]   = {1, 2, 5, 10, 20, 50, 100, 200};
  const char* coinLabels[] = {"1 Ct", "2 Ct", "5 Ct", "10 Ct", "20 Ct", "50 Ct", "1 €", "2 €"};
  const char* coinFields[] = {"coin_1","coin_2","coin_5","coin_10","coin_20","coin_50","coin_100","coin_200"};
  const char* coinColors[] = {"#b87333","#b87333","#b87333","#c8a951","#c8a951","#c8a951","#c0c0c0","#d4d4d4"};
  for (int d = 0; d < 8; d++) {
    int foundPulse = 0;
    for (int p = 1; p <= 6; p++) {
      if (pulseValues[p] == coinDenoms[d]) { foundPulse = p; break; }
    }
    bool active = foundPulse > 0;
    String badge = "<span style='background:" + String(coinColors[d]) + "; color:#111; font-weight:800; font-size:0.85rem; border-radius:8px; padding:0.3rem 0.6rem; min-width:3rem; text-align:center; display:inline-block;'>" + String(coinLabels[d]) + "</span>";
    String statusStr = active ? ("&#10003; " + String(foundPulse) + " Pulse") : "nicht aktiv";
    String statusColor = active ? "var(--success)" : "var(--text-sec)";
    html += "<div style='display:flex; align-items:center; gap:1rem; background:var(--card-bg); border:1px solid var(--border); border-radius:10px; padding:0.65rem 1rem;'>";
    html += badge;
    html += "<span style='font-size:0.82rem; color:var(--text-sec); flex:0 0 auto;'>M&uuml;nze</span>";
    html += "<input type='number' name='" + String(coinFields[d]) + "' value='" + String(foundPulse) + "' min='0' max='6' style='width:4.5rem; text-align:center; font-size:1rem; font-weight:700; padding:0.3rem 0.4rem; margin:0;' placeholder='0'>";
    html += "<span style='margin-left:auto; font-size:0.82rem; font-weight:600; color:" + statusColor + ";'>" + statusStr + "</span>";
    html += "</div>";
  }

  html += R"HTML(
      </div>
      <p style='color:var(--text-sec); font-size:0.75rem; margin:0.75rem 0 0 0;'>&#9432; Pulse = 0 bedeutet: diese M&uuml;nze wird nicht akzeptiert.</p>
    </div>


    <!-- Scheinprüfer -->
    <div class='stat-box' style='margin-bottom:1.5rem;'>
      <h2 style='margin:0 0 0.25rem 0; font-size:1rem; color:var(--text-sec); font-weight:600; letter-spacing:0.04em; text-transform:uppercase;'>&#x1F4B5; Scheinpr&uuml;fer &mdash; Schein &rarr; Pulse</h2>
      <p style='color:var(--text-sec); font-size:0.8rem; margin:0 0 1rem 0;'>Pulse-Anzahl je Schein. 0 = nicht aktiv.</p>
      <div style='display:flex; flex-direction:column; gap:0.6rem;'>
  )HTML";

  const int billDenoms[]   = {5, 10, 20, 50, 100};
  const char* billFields[] = {"bill_5", "bill_10", "bill_20", "bill_50", "bill_100"};
  const char* billColors[] = {"#4ade80","#34d399","#60a5fa","#f59e0b","#f87171"};
  for (int d = 0; d < 5; d++) {
    int foundPulse = 0;
    for (int p = 1; p <= 16; p++) {
      if (billValues[p] == billDenoms[d]) { foundPulse = p; break; }
    }
    bool active = foundPulse > 0;
    String badge = "<span style='background:" + String(billColors[d]) + "; color:#111; font-weight:800; font-size:0.9rem; border-radius:8px; padding:0.3rem 0.7rem; min-width:3.2rem; text-align:center; display:inline-block;'>" + String(billDenoms[d]) + " &euro;</span>";
    String statusStr = active ? ("&#10003; " + String(foundPulse) + " Pulse") : "nicht aktiv";
    String statusColor = active ? "var(--success)" : "var(--text-sec)";
    html += "<div style='display:flex; align-items:center; gap:1rem; background:var(--card-bg); border:1px solid var(--border); border-radius:10px; padding:0.65rem 1rem;'>";
    html += badge;
    html += "<span style='font-size:0.82rem; color:var(--text-sec); flex:0 0 auto;'>Schein</span>";
    html += "<input type='number' name='" + String(billFields[d]) + "' value='" + String(foundPulse) + "' min='0' max='16' style='width:4.5rem; text-align:center; font-size:1rem; font-weight:700; padding:0.3rem 0.4rem; margin:0;' placeholder='0'>";
    html += "<span style='margin-left:auto; font-size:0.82rem; font-weight:600; color:" + statusColor + ";'>" + statusStr + "</span>";
    html += "</div>";
  }

  html += R"HTML(
      </div>
      <p style='color:var(--text-sec); font-size:0.75rem; margin:0.75rem 0 0 0;'>&#9432; Pulse = 0 bedeutet: dieser Schein wird nicht akzeptiert.</p>
    </div>

    <button type='submit' class='btn-main' style='width:100%;'>&#x1F4BE; Einstellungen speichern</button>
    </form>
  </section>


</div> <!-- End Main -->

</body></html>
)HTML";
  server.send(200, "text/html; charset=UTF-8", html);
}
#endif // end legacy showDashboard
