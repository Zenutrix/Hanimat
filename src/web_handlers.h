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
        int next = allRelaysTest.currentSlot + 1;
        while (next < activeSlots && allRelaysTest.emptyOnly &&
               (slotAvailable[next] || slotLocked[next])) {
          next++; // Volle/gesperrte Faecher bei "nur leere" ueberspringen
        }
        allRelaysTest.currentSlot = next;
        if (allRelaysTest.currentSlot >= activeSlots) {
          allRelaysTest.active = false;
          logMessage("Web: Relais-Sequenz abgeschlossen.");
        } else {
          controlSlotRelay(allRelaysTest.currentSlot, true);
          allRelaysTest.relayOn = true;
          allRelaysTest.phaseStartTime = now;
        }
      }
    }
  }
}

/** @brief Liefert das Dashboard-JavaScript aus LittleFS (/app.js). */
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
               + "\"locked\":" + (slotLocked[i] ? "true" : "false") + ","
               + "\"pickup\":" + (slotIsPickup[i] ? "true" : "false") + "}";
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
  json += "\"autocredit_enabled\":" + jBool(autoCreditResetEnabled)             + ",";
  {
    char acrTimeBuf[6];
    snprintf(acrTimeBuf, sizeof(acrTimeBuf), "%02d:%02d", autoCreditResetHour, autoCreditResetMinute);
    json += "\"autocredit_time\":"  + jStr(String(acrTimeBuf))                  + ",";
  }
  json += "\"idle_credit_reset_enabled\":" + jBool(idleCreditResetEnabled)       + ",";
  json += "\"idle_credit_reset_min\":"     + String(idleCreditResetMinutes)      + ",";
  json += "\"max_credit\":"                + centsToEurStr(maxCreditCents)      + ",";
  json += "\"max_topup\":"                 + centsToEurStr(maxTopUpCents)       + ",";

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
  json += "\"notify_credit_threshold\":" + jBool(telegramNotifyCreditThreshold) + ",";
  json += "\"credit_warn_threshold\":"   + centsToEurStr(creditWarnThresholdCents) + ",";

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

  // --- Zahlungs-Pulse (je Nennwert) ---
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
  json += "],";
  json += "\"slotPickup\":[";
  for (int i = 0; i < activeSlots; i++) {
    if (i > 0) json += ",";
    json += jBool(slotIsPickup[i]);
  }
  json += "],";
  json += "\"slotPin\":[";
  for (int i = 0; i < activeSlots; i++) {
    if (i > 0) json += ",";
    json += jStr(slotPinCode[i]);
  }
  json += "]";  // kein Komma – letztes Feld

  json += "}";
  server.send(200, "application/json", json);
}

/** @brief Richtet alle Webserver-Endpunkte (Routen) ein. */
void handleResetCashBox();  // Vorwärtsdeklaration
void handlePaymentConfig();
void handleSavePaymentConfig();
void handleOTAFileUploadFs();
void handleOpenEmptySlotsWeb();
void handleEventLogDataRequest();
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
  server.on("/openemptyslots", HTTP_POST, handleOpenEmptySlotsWeb);
  server.on("/setstaticip", HTTP_POST, handleSetStaticIPWeb);
  server.on("/updateslots", HTTP_POST, handleUpdateSlotsWeb);
  server.on("/toggleslotlock", HTTP_POST, handleToggleSlotLockWeb);
  server.on("/logdata", HTTP_GET, handleLogDataRequest);
  server.on("/eventlogdata", HTTP_GET, handleEventLogDataRequest);
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

  // 404-Handler (Seite nicht gefunden)
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
  
  // Timeout von Sekunden in Millisekunden umrechnen
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
    logEventf("Web: SumUp Pairing erfolgreich. Reader ID: %s", newId.c_str());
    server.send(200, "text/html", "Erfolg! ID: " + newId + " <meta http-equiv='refresh' content='2;url=/' />");
  } else {
    logEvent("Web: SumUp Pairing fehlgeschlagen.");
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

/** @brief Verarbeitet Anfragen an die Root-URL ("/"): zeigt Login oder Dashboard. */
void handleRoot() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) {
    showLoginPage();
  } else {
    showDashboard();
  }
}

/** @brief Verarbeitet die Login-Formular-Übermittlung. */
void handleLogin() {
  lastActivityTimeWeb = millis();

  // --- Brute-Force: Sperre prüfen ---
  if (loginLockoutUntil > 0 && millis() < loginLockoutUntil) {
    unsigned long secsLeft = (loginLockoutUntil - millis()) / 1000;
    logEventf("Web: Login gesperrt – noch %lu s.", secsLeft);
    server.send(429, "text/plain",
      "Zu viele Fehlversuche. Bitte " + String(secsLeft) + " Sekunden warten.");
    return;
  }

  if (server.hasArg("password") && server.arg("password") == savedPassword) {
    // Erfolg → Zähler und Sperre zurücksetzen
    loginFailCount    = 0;
    loginLockoutUntil = 0;
    activeSessionToken = generateSessionToken();
    logEventf("Web: Login erfolgreich. Session: %s", activeSessionToken.c_str());
    server.sendHeader("Set-Cookie", "HANIMAT_SESSION=" + activeSessionToken + "; HttpOnly; Path=/");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  } else {
    loginFailCount++;
    logEventf("Web: Login fehlgeschlagen (%d/%d).", loginFailCount, LOGIN_MAX_FAILS);

    if (loginFailCount >= LOGIN_MAX_FAILS) {
      loginLockoutUntil = millis() + LOGIN_LOCKOUT_MS;
      loginFailCount    = 0; // Zurücksetzen – nach Ablauf der Sperre neu zählen
      logEvent("Web: Brute-Force erkannt! IP gesperrt für 5 Minuten.");
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
  // Münz-Mapping je Nennwert: coin_1, coin_2, coin_5, ..., coin_200
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
  // Schein-Mapping je Nennwert: bill_5, bill_10, bill_20, bill_50, bill_100
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

/** @brief Verarbeitet die Passwort-Ändern-Formular-Übermittlung. */
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
        logEvent("Web: Admin password changed.");
        server.send(200, "text/html", "Passwort geändert. <meta http-equiv='refresh' content='2;url=/' />");
    } else {
        server.send(400, "text/html", "Passwort zu kurz (min. 4 Zeichen). <meta http-equiv='refresh' content='2;url=/' />");
    }
  } else { server.send(400, "text/plain", "New password missing."); }
}

/**
 * @brief Aktualisiert alle Fachpreise in einer Formular-Übermittlung.
 *        Erwartet Parameter price_0 … price_N (als EUR-Float-String, z.B. "5.10").
 */
void handleUpdatePriceWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }

  int changed = 0;
  char argName[16], nvKey[16];

  preferences.begin("hanimat", false);
  for (int i = 0; i < activeSlots; i++) {
    snprintf(argName, sizeof(argName), "price_%d", i);
    if (server.hasArg(argName)) {
      int priceCents = (int)roundf(server.arg(argName).toFloat() * 100.0f);
      if (priceCents < 0) priceCents = 0;

      if (slotPriceCents[i] != priceCents) {
        slotPriceCents[i] = priceCents;
        snprintf(nvKey, sizeof(nvKey), "priceC%d", i);
        preferences.putInt(nvKey, priceCents);
        // Nur RAM-Log statt logEventf: bei "Alle uebernehmen" waeren das bis zu 128 Flash-Schreibzyklen in einem Request.
        logf("Web: Preis Fach %d -> %s EUR", i + 1, centsToEurStr(priceCents).c_str());
        changed++;
      }
    }

    // --- Abholfach-Konfiguration ---
    snprintf(argName, sizeof(argName), "pickup_%d", i);
    bool newIsPickup = server.hasArg(argName); // Checkbox: nur vorhanden wenn angehakt

    if (newIsPickup != slotIsPickup[i]) {
      slotIsPickup[i] = newIsPickup;
      snprintf(nvKey, sizeof(nvKey), "pickup%d", i);
      preferences.putBool(nvKey, newIsPickup);
      if (!newIsPickup && strlen(slotPinCode[i]) > 0) {
        // Beim Deaktivieren: verwaisten Code entfernen
        slotPinCode[i][0] = '\0';
        snprintf(nvKey, sizeof(nvKey), "pin%d", i);
        preferences.putString(nvKey, "");
      }
      logf("Web: Fach %d Abholfach-Modus: %s", i + 1, newIsPickup ? "an" : "aus");
      changed++;
    }

    snprintf(argName, sizeof(argName), "code_%d", i);
    if (newIsPickup && server.hasArg(argName)) {
      String newCode = server.arg(argName);
      bool validCode = (newCode.length() == 4);
      for (size_t c = 0; validCode && c < newCode.length(); c++) {
        if (!isdigit(newCode[c])) validCode = false;
      }
      if (validCode && newCode != slotPinCode[i]) {
        newCode.toCharArray(slotPinCode[i], sizeof(slotPinCode[i]));
        snprintf(nvKey, sizeof(nvKey), "pin%d", i);
        preferences.putString(nvKey, newCode);
        slotAvailable[i] = true; // neuer Code = frisch befuellt
        snprintf(nvKey, sizeof(nvKey), "avail%d", i);
        preferences.putBool(nvKey, true);
        logf("Web: Neuer Abhol-Code fuer Fach %d gesetzt, Fach wieder verfuegbar.", i + 1);
        changed++;
      }
    }
  }
  preferences.end();

  if (changed > 0) {
    logEventf("Web: Slot-Konfiguration gespeichert (%d Aenderungen).", changed);
  } else {
    logMessage("Web: Slot-Konfiguration gespeichert (keine Aenderungen).");
  }
  displayNeedsUpdate = true;
  server.sendHeader("Location", "/#slots-config", true);
  server.send(302, "text/plain", "");
}

/** @brief Füllt ein einzelnes Fach wieder auf. */
void handleRefillWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      if (isPickupSlotEmpty(slot)) {
          // Abholfach ohne Code: "verfuegbar" waere inkonsistent (Keypad/Display
          // melden weiterhin "leer") und wuerde Bestandswarnungen unterdruecken.
          server.send(400, "text/html", String("Fach ") + (slot+1) + " ist ein Abholfach - bitte in der Slot-Konfiguration einen neuen Code setzen. <meta http-equiv='refresh' content='3;url=/' />");
      } else if (!slotLocked[slot]) {
          slotAvailable[slot] = true;
          char nvKey[12]; snprintf(nvKey, sizeof(nvKey), "avail%d", slot);
          preferences.begin("hanimat", false);
          preferences.putBool(nvKey, true);
          preferences.end();
          logEventf("Web: Slot %d refilled.", slot + 1);
          checkOverallStockLevel();
          server.send(200, "text/html", "Fach aufgefuellt. <meta http-equiv='refresh' content='1;url=/' />");
          displayNeedsUpdate = true;
      } else { server.send(400, "text/html", String("Fach ") + (slot+1) + " ist gesperrt. <meta http-equiv='refresh' content='2;url=/' />");}
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Fügt manuell Guthaben hinzu oder entfernt es (z.B. um einem Kunden Guthaben aufzuladen).
 *        Max. 50 EUR pro Vorgang; das generelle 100-EUR-Sicherheitsnetz (enforceCreditCap) gilt zusaetzlich.
 */
void handleAddCreditWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("amount")) {
    float amountEur = server.arg("amount").toFloat();
    // Bereich VOR dem float->int-Cast pruefen: extreme Werte (z.B. 999999999999)
    // waeren im Cast undefiniertes Verhalten und koennten die abs()-Pruefung umgehen.
    if (isnan(amountEur) || amountEur < -1000.0f || amountEur > 1000.0f) {
        server.send(400, "text/html", "Ungueltiger Betrag. <meta http-equiv='refresh' content='2;url=/' />");
        return;
    }
    int amountCents = (int)roundf(amountEur * 100.0f);
    if (amountCents == 0) { server.send(400, "text/plain", "Amount is 0."); return; }
    if (abs(amountCents) > maxTopUpCents) {
        server.send(400, "text/html", "Maximal " + centsToEurStr(maxTopUpCents) + " Euro pro Vorgang erlaubt. <meta http-equiv='refresh' content='2;url=/' />");
        return;
    }
    addCredit(amountCents);
    saveCreditToNVS(true);
    logEventf("Web: Credit adjusted by %s EUR. New credit: %s EUR.", centsToEurStr(amountCents).c_str(), centsToEurStr(creditCents).c_str());
    server.send(200, "text/html", "Guthaben angepasst. <meta http-equiv='refresh' content='1;url=/' />");
  } else { server.send(400, "text/plain", "Amount missing."); }
}

/** @brief Setzt das Guthaben auf null zurück. */
void handleResetCreditWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  resetCredit("Web-Interface");
  server.send(200, "text/html", "Guthaben zurueckgesetzt. <meta http-equiv='refresh' content='1;url=/' />");
}

/** @brief Füllt alle verfügbaren (unverriegelten) Fächer wieder auf. */
void handleRefillAllWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  char nvKey[12];
  int skippedPickups = 0;
  preferences.begin("hanimat", false);
  for (int i = 0; i < activeSlots; i++) {
    if (isPickupSlotEmpty(i)) {
        skippedPickups++; // Abholfach ohne Code: braucht neuen Code statt "Auffuellen"
        continue;
    }
    if (!slotLocked[i]) {
        slotAvailable[i] = true;
        snprintf(nvKey, sizeof(nvKey), "avail%d", i);
        preferences.putBool(nvKey, true);
    }
  }
  preferences.end();
  if (skippedPickups > 0) {
    logEventf("Web: Alle Faecher aufgefuellt (%d Abholfaecher ohne Code uebersprungen).", skippedPickups);
  } else {
    logEvent("Web: All unlocked slots have been refilled.");
  }
  checkOverallStockLevel();
  server.send(200, "text/html", "Alle Faecher aufgefuellt. <meta http-equiv='refresh' content='1;url=/' />");
  displayNeedsUpdate = true;
}

/** @brief Löst ein einzelnes Relais zu Testzwecken aus. */
void handleTriggerRelayWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  // Schutz: Kein Test während eine echte Ausgabe läuft
  if (dispenseJob.active) { server.send(409, "text/plain", "Ausgabe laeuft gerade – bitte warten."); return; }
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < activeSlots) {
      logf("Web: Testing relay for slot %d", slot + 1);
      // Nicht blockierend: Relais einschalten, processRelayTestJobs() schaltet nach 1s ab
      controlSlotRelay(slot, true);
      singleRelayTest = { true, slot, millis() };
      server.send(200, "text/html", String("Relais Fach ") + (slot+1) + " ausgeloest. <meta http-equiv='refresh' content='1;url=/' />");
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/**
 * @brief Oeffnet nacheinander alle Faecher (Relais-Sequenz, z.B. zur Wartung).
 */
void handleTriggerAllRelaysWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  // Schutz: Kein Test während eine echte Ausgabe läuft
  if (dispenseJob.active) { server.send(409, "text/plain", "Ausgabe laeuft gerade – bitte warten."); return; }
  logMessage("Web: Starte Relais-Sequenz (alle Faecher)...");
  // Nicht blockierend: processRelayTestJobs() übernimmt die Sequenz
  controlSlotRelay(0, true);
  allRelaysTest = { true, 0, millis(), true, false };
  server.send(200, "text/html", "Alle Faecher werden geoeffnet (laeuft im Hintergrund). <meta http-equiv='refresh' content='1;url=/' />");
}

/**
 * @brief Oeffnet nacheinander nur die leeren, unverriegelten Faecher
 *        (z.B. zum gezielten Nachfuellen, ohne bereits volle Faecher zu oeffnen).
 */
void handleOpenEmptySlotsWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (dispenseJob.active) { server.send(409, "text/plain", "Ausgabe laeuft gerade – bitte warten."); return; }

  int firstEmpty = -1;
  for (int i = 0; i < activeSlots; i++) {
    if (!slotAvailable[i] && !slotLocked[i]) { firstEmpty = i; break; }
  }
  if (firstEmpty == -1) {
    server.send(200, "text/html", "Keine leeren Faecher vorhanden. <meta http-equiv='refresh' content='1;url=/' />");
    return;
  }

  logMessage("Web: Starte Relais-Sequenz (nur leere Faecher)...");
  controlSlotRelay(firstEmpty, true);
  allRelaysTest = { true, firstEmpty, millis(), true, true };
  server.send(200, "text/html", "Leere Faecher werden geoeffnet (laeuft im Hintergrund). <meta http-equiv='refresh' content='1;url=/' />");
}

/** @brief Setzt die statische IP-Konfiguration und startet neu. */
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

/** @brief Aktualisiert die Gesamtanzahl der aktiven Fächer. */
void handleUpdateSlotsWeb() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) { server.send(401, "text/plain", "Not authorized."); return; }
  if (server.hasArg("maxSlots")) {
    int newNumSlots = server.arg("maxSlots").toInt();
    if (newNumSlots > 0 && newNumSlots <= MAX_SLOTS) {
      int oldNumSlots = activeSlots;
      activeSlots = newNumSlots;
      char nvKey[12];
      preferences.begin("hanimat", false);
      preferences.putInt("activeSlots", activeSlots);
      // Neue Fächer initialisieren, falls in Preferences noch nicht vorhanden
      for(int i = 0; i < activeSlots; i++) {
          snprintf(nvKey, sizeof(nvKey), "avail%d", i);
          if(!preferences.isKey(nvKey)) {
              slotAvailable[i] = true;
              preferences.putBool(nvKey, true);
          }
          snprintf(nvKey, sizeof(nvKey), "priceC%d", i);
          if(!preferences.isKey(nvKey)) {
              slotPriceCents[i] = 500; // Standardwert 5,00 EUR
              preferences.putInt(nvKey, 500);
          }
      }

      // Abholfach-Arrays sind heap-allokiert auf Groesse oldNumSlots; beim Vergroessern muessen sie neu allokiert werden (sonst Pufferueberlauf).
      if (activeSlots > oldNumSlots) {
        bool  *newPickup  = new bool[activeSlots]();
        char (*newPin)[7] = new char[activeSlots][7]();
        for (int i = 0; i < oldNumSlots; i++) {
          newPickup[i] = slotIsPickup[i];
          memcpy(newPin[i], slotPinCode[i], sizeof(newPin[i]));
        }
        for (int i = oldNumSlots; i < activeSlots; i++) {
          snprintf(nvKey, sizeof(nvKey), "pickup%d", i);
          newPickup[i] = preferences.getBool(nvKey, false);
          snprintf(nvKey, sizeof(nvKey), "pin%d", i);
          preferences.getString(nvKey, newPin[i], sizeof(newPin[i]));
        }
        delete[] slotIsPickup;
        delete[] slotPinCode;
        slotIsPickup = newPickup;
        slotPinCode  = newPin;
        logf("Web: Abholfach-Arrays vergroessert von %d auf %d Faecher.", oldNumSlots, activeSlots);
      }

      preferences.end();
      logf("Web: Number of active slots set to %d", activeSlots);
      server.send(200, "text/html", "Anzahl Faecher aktualisiert. Neustart empfohlen. <meta http-equiv='refresh' content='2;url=/' />");
      displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", String("Invalid slot count (1-") + MAX_SLOTS + ")."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/** @brief Schaltet den Sperrzustand eines Fachs um. */
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
      logEventf("Web: Slot %d %s", slot + 1, slotLocked[slot] ? "locked." : "unlocked.");
      server.send(200, "text/html", "Fachstatus geaendert. <meta http-equiv='refresh' content='1;url=/' />");
      displayNeedsUpdate = true;
    } else { server.send(400, "text/plain", "Invalid slot."); }
  } else { server.send(400, "text/plain", "Missing parameters."); }
}

/** @brief Liefert Log-Daten als Klartext für die Web-Oberfläche. */
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
 * @brief Liefert das persistente Ereignis-Log (Zahlungen, Preisaenderungen,
 *        Fehler, Admin-Aktionen — ca. 3 Tage zurueckverfolgbar) als Text.
 */
void handleEventLogDataRequest() {
  lastActivityTimeWeb = millis();
  if (!isAuth()) {
    server.send(401, "text/plain", "Not authorized.");
    return;
  }

  String content = "";
  if (LittleFS.exists(EVENT_LOG_FILE_OLD)) {
    File fOld = LittleFS.open(EVENT_LOG_FILE_OLD, "r");
    if (fOld) { content += fOld.readString(); fOld.close(); }
  }
  if (LittleFS.exists(EVENT_LOG_FILE)) {
    File fCur = LittleFS.open(EVENT_LOG_FILE, "r");
    if (fCur) { content += fCur.readString(); fCur.close(); }
  }
  if (content.length() == 0) content = "Noch keine Ereignisse aufgezeichnet.";
  server.send(200, "text/plain", content);
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
    if      (e.method == PaymentMethod::SUMUP)  json += "SUMUP";
    else if (e.method == PaymentMethod::PICKUP) json += "PICKUP";
    else if (e.method == PaymentMethod::MIXED)  json += "MIXED";
    else                                        json += "CASH";
    json += "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}


// --- OTA Update Handlers ---

/** @brief Zeigt die OTA-Update-Seite an. */
void handleOTAUpdatePage() {
  if (!isAuth()) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  lastActivityTimeWeb = millis();
  // Das eigentliche HTML wird per showDashboard()-JS erzeugt, hier wird nur weitergeleitet.
  server.sendHeader("Location", "/#ota-update-section", true);
  server.send(302, "text/plain", "");
}

/** @brief Verarbeitet den binären Datei-Upload für OTA-Updates. */
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

/** @brief Verarbeitet die Zeiteinstellungen-Formular-Übermittlung. */
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

    autoCreditResetEnabled = server.hasArg("autocredit_enabled");
    String acrTime = server.arg("autocredit_time"); // Format "HH:MM" (aus <input type='time'>)
    int colonPos = acrTime.indexOf(':');
    if (colonPos > 0) {
      int h = acrTime.substring(0, colonPos).toInt();
      int m = acrTime.substring(colonPos + 1).toInt();
      if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
        autoCreditResetHour   = h;
        autoCreditResetMinute = m;
      }
    }

    idleCreditResetEnabled = server.hasArg("idle_credit_reset_enabled");
    int idleMin = server.arg("idle_credit_reset_min").toInt();
    if (idleMin >= 1 && idleMin <= 120) idleCreditResetMinutes = idleMin;

    float maxCreditEur = server.arg("max_credit").toFloat();
    if (maxCreditEur >= 1 && maxCreditEur <= 1000) maxCreditCents = (int)roundf(maxCreditEur * 100.0f);
    float maxTopUpEur = server.arg("max_topup").toFloat();
    if (maxTopUpEur >= 1 && maxTopUpEur <= 1000) maxTopUpCents = (int)roundf(maxTopUpEur * 100.0f);

    preferences.putULong("coinDelay",      COIN_PROCESSING_DELAY);
    preferences.putULong("billIsrDeb",     BILL_ISR_DEBOUNCE_MS);
    preferences.putULong("billGrpTout",    BILL_GROUP_PROCESSING_TIMEOUT_MS);
    preferences.putULong("dispTime",       DISPENSE_RELAY_ON_TIME);
    preferences.putULong("keypadTime",     KEYPAD_INPUT_TIMEOUT);
    preferences.putULong("slotSelTime",    SLOT_SELECTION_TIMEOUT);
    preferences.putULong("dispTimeout",    DISPLAY_TIMEOUT);
    preferences.putULong("webTout",        WEB_TIMEOUT);
    preferences.putBool("statusEnabled",   statusEnabled);
    preferences.putBool("acrEnabled",      autoCreditResetEnabled);
    preferences.putInt("acrHour",          autoCreditResetHour);
    preferences.putInt("acrMinute",        autoCreditResetMinute);
    preferences.putBool("idleCrEn",        idleCreditResetEnabled);
    preferences.putInt("idleCrMin",        idleCreditResetMinutes);
    preferences.putInt("maxCreditCts",     maxCreditCents);
    preferences.putInt("maxTopUpCts",      maxTopUpCents);
    preferences.end();

    logEventf("Web: Guthaben-Obergrenze %s EUR, max. Aufladung %s EUR.",
              centsToEurStr(maxCreditCents).c_str(), centsToEurStr(maxTopUpCents).c_str());
    logEventf("Web: Automatischer Guthaben-Reset %s (Uhrzeit %02d:%02d).",
              autoCreditResetEnabled ? "aktiviert" : "deaktiviert",
              autoCreditResetHour, autoCreditResetMinute);
    logEventf("Web: Idle-Guthaben-Reset %s (%d Minuten).",
              idleCreditResetEnabled ? "aktiviert" : "deaktiviert", idleCreditResetMinutes);
    logMessage("Web: Zeiteinstellungen gespeichert und sofort übernommen.");
    otaStatusMessage = "Zeiteinstellungen gespeichert! Neustart empfohlen.";
    server.sendHeader("Location", "/#timing-config", true);
    server.send(302, "text/plain", "");
}

/** @brief Verarbeitet die Formular-Übermittlung für Telegram- und Bestandsbenachrichtigungen. */
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

    telegramNotifyCreditThreshold = server.hasArg("notify_credit_threshold");
    float creditWarnEur = server.arg("credit_warn_threshold").toFloat();
    if (creditWarnEur >= 0 && creditWarnEur <= 1000) creditWarnThresholdCents = (int)roundf(creditWarnEur * 100.0f);

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
    preferences.putBool("tgNotifyCredit", telegramNotifyCreditThreshold);
    preferences.putInt("creditWarnCts",   creditWarnThresholdCents);
    preferences.end();

    logEventf("Web: Guthaben-Telegram-Warnung %s (Schwelle %s EUR).",
              telegramNotifyCreditThreshold ? "aktiviert" : "deaktiviert",
              centsToEurStr(creditWarnThresholdCents).c_str());

    bot.updateToken(telegramBotToken);

    logMessage("Web: Telegram & notification settings saved.");
    otaStatusMessage = "Einstellungen gespeichert!";
    server.sendHeader("Location", "/#telegram-config", true);
    server.send(302, "text/plain", "");
}

/** @brief Sendet eine Testnachricht an den konfigurierten Telegram-Chat. */
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

// Weiterleitungen für Einstellungsseiten (Inhalt wird per JS geladen)
void handleTimingConfigPage() { server.sendHeader("Location", "/#timing-config", true); server.send(302); }
void handleTelegramConfigPage() { server.sendHeader("Location", "/#telegram-config", true); server.send(302); }


// =================================================================
//                      HTML PAGE GENERATORS
// =================================================================

/** @brief Erzeugt und sendet das HTML für die Login-Seite. */
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
