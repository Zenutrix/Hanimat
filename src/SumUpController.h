#ifndef SUMUP_CONTROLLER_H
#define SUMUP_CONTROLLER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Verknüpfung zur Log-Funktion, die in main.cpp definiert ist
extern void logMessage(const String& msg);

class SumUpController {
private:
    String apiKey;
    String merchantId;
    String readerId;
    
    // Leitet interne SumUp-Logs ans Hanimat-System weiter
    void log(String msg) {
        logMessage("[SumUp Internal] " + msg);
    }

public:
    SumUpController(const String& key, const String& mId, const String& rId = "") {
        apiKey = key;
        merchantId = mId;
        readerId = rId;
    }

    void setReaderId(const String& id) { readerId = id; }
    String getReaderId() { return readerId; }

    // --- PAIRING ---
    /** @brief Verbindet das physische Terminal mit diesem Controller. */
    String pairReader(const String& pairingCode) {
        if (apiKey.length() < 5 || merchantId.length() < 3) return "";
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers";
        
        if (!http.begin(client, url)) return "";
        http.addHeader("Authorization", "Bearer " + apiKey);
        http.addHeader("Content-Type", "application/json");

        JsonDocument doc;
        doc["pairing_code"] = pairingCode;
        doc["name"] = "Hanimat-Terminal"; 
        String requestBody; serializeJson(doc, requestBody);

        int httpCode = http.POST(requestBody);
        String response = http.getString();
        String newId = "";

        if (httpCode == 200 || httpCode == 201) {
            JsonDocument res; deserializeJson(res, response);
            if (res["id"].is<String>()) {
                newId = res["id"].as<String>();
                readerId = newId;
                log("Pairing OK: " + newId);
            }
        } else {
            log("Pairing Error: " + String(httpCode));
        }
        http.end();
        return newId;
    }

// --- READER ENTKOPPELN (API DELETE) ---
/** @brief Löscht die Verknüpfung auf den SumUp-Servern und lokal. */
bool unpairReader() {
    // Ohne vorhandene ID gilt der Vorgang bereits als erledigt
    if (readerId.length() < 3) {
        log("Keine Reader-ID vorhanden. Überspringe API-Request.");
        readerId = "";
        return true; 
    }

    WiFiClientSecure client;
    client.setInsecure(); // Verbindung verschlüsselt, Zertifikatskette nicht geprüft
    HTTPClient http;

    // Endpunkt: DELETE /merchants/{mid}/readers/{rid}
    String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers/" + readerId;

    log("Sende Unpair Request (DELETE) an: " + url);

    if (http.begin(client, url)) {
        http.addHeader("Authorization", "Bearer " + apiKey);
        
        // WICHTIG: Manche APIs erwarten bei DELETE explizit Content-Length 0
        http.addHeader("Content-Length", "0");
        
        // Timeout erhöhen, da Cloud-Requests manchmal einen Moment dauern
        http.setTimeout(5000); 

        int httpCode = http.sendRequest("DELETE");
        String response = http.getString();
        
        http.end();

        // 204/200 = erfolgreich gelöscht, 404 = war schon gelöscht -> ebenfalls Erfolg
        if (httpCode == 204 || httpCode == 200 || httpCode == 404) {
            log("Unpair erfolgreich (Code: " + String(httpCode) + ")");
            readerId = ""; // Lokal löschen
            return true;
        } else {
            log("Unpair API Fehler: " + String(httpCode) + " Resp: " + response);
            
            // ID trotzdem lokal löschen, damit das System nicht blockiert
            readerId = ""; 
            return false; 
        }
    } else {
        log("Unpair: Verbindung zur API konnte nicht hergestellt werden.");
        readerId = ""; // Fallback Löschung
        return false;
    }
}
    // --- READER STATUS PRÜFEN ---
    /** @brief Prüft, ob die gespeicherte Reader-ID noch auf SumUp-Servern existiert. */
    bool checkReader() {
        if (apiKey.length() < 5 || merchantId.length() < 3 || readerId.length() < 5) {
            log("checkReader: Fehlende Konfiguration.");
            return false;
        }
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers/" + readerId;
        log("GET -> " + url);
        if (!http.begin(client, url)) { log("checkReader: http.begin() fehlgeschlagen"); return false; }
        http.addHeader("Authorization", "Bearer " + apiKey);
        int httpCode = http.GET();
        String response = http.getString();
        http.end();
        if (httpCode == 200) {
            log("checkReader: Reader ist aktiv (200 OK).");
            return true;
        } else {
            log("checkReader: Fehler " + String(httpCode) + " -> " + response.substring(0, 80));
            return false;
        }
    }

    // --- ZAHLUNG STARTEN ---
    /**
     * @brief Sendet den Zahlungsbefehl an die SumUp Cloud.
     * @param amountCents Zu zahlender Betrag in Cent (z.B. 510 = 5,10 EUR)
     */
    bool startPayment(int amountCents, String &outId) {
        // Pflichtfelder prüfen — leere merchantId erzeugt URL ".../merchants//readers/..." → 404
        if (apiKey.length() < 5 || merchantId.length() < 3 || readerId.length() < 5) {
            log("startPayment ABGEBROCHEN: Fehlende Konfiguration!"
                " apiKey=" + String(apiKey.length()) +
                " mid=" + String(merchantId.length()) +
                " rid=" + String(readerId.length()));
            return false;
        }
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers/" + readerId + "/checkout";

        // Diagnose-Log: zeigt die exakte URL und die ersten Zeichen des API-Keys
        log("POST -> " + url);
        log("Auth: Bearer " + apiKey.substring(0, 8) + "...");

        if (!http.begin(client, url)) { log("http.begin() fehlgeschlagen"); return false; }
        http.addHeader("Authorization", "Bearer " + apiKey);
        http.addHeader("Content-Type", "application/json");

        JsonDocument doc;
        // SumUp API erwartet den Betrag bereits in Cent (minor_unit = 2)
        doc["total_amount"]["value"] = amountCents;
        doc["total_amount"]["currency"] = "EUR";
        doc["total_amount"]["minor_unit"] = 2;
        doc["checkout_reference"] = "HANI-" + String(random(10000, 99999));
        doc["description"] = "Hanimat " + String(amountCents / 100) + "." + String(amountCents % 100 < 10 ? "0" : "") + String(amountCents % 100) + " EUR";
        doc["return_url"] = "https://sumup.com"; 

        String body; serializeJson(doc, body);
        int httpCode = http.POST(body);
        String response = http.getString();
        bool success = false;

        if (httpCode == 200 || httpCode == 201) {
            JsonDocument res; deserializeJson(res, response);
            // ID extrahieren (wichtig für spätere Verlaufs-Suche)
            if (res["data"]["client_transaction_id"].is<String>()) {
                outId = res["data"]["client_transaction_id"].as<String>();
                success = true;
            } else if (res["id"].is<String>()) {
                outId = res["id"].as<String>();
                success = true;
            }
            if(success) log("Checkout gestartet. ID: " + outId);
            else log("Keine ID in Antwort gefunden!");
        } else {
            log("Checkout Start Fehler: " + String(httpCode) + " Resp: " + response);
        }
        http.end();
        return success;
    }

    // --- STATUS PRÜFEN (robuste Verlaufs-Suche) ---
    /** @brief Sucht die Transaktions-ID in der Historie des Händlers. */
    String checkStatus(const String& trackingId) {
        if (trackingId.length() < 5) return "UNKNOWN";
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        String status = "PENDING"; 

        // 'order=descending' (API-konform) und 'limit=5' (spart Speicher, schnelleres Parsen)
        String url = "https://api.sumup.com/v0.1/me/transactions/history?limit=5&order=descending";

        if (http.begin(client, url)) {
            http.addHeader("Authorization", "Bearer " + apiKey);
            int httpCode = http.GET();

            if (httpCode == 200) {
                String payload = http.getString();
                JsonDocument d;

                // JSON-Antwort parsen
                DeserializationError err = deserializeJson(d, payload);

                if (!err) {
                    JsonArray items;
                    // Flexible Handhabung der JSON-Struktur
                    if (d["items"].is<JsonArray>()) items = d["items"].as<JsonArray>();
                    else if (d.is<JsonArray>()) items = d.as<JsonArray>();

                    if (!items.isNull()) {
                        bool found = false;
                        for (JsonObject t : items) {
                            String cId = "";
                            if (!t["client_transaction_id"].isNull()) cId = t["client_transaction_id"].as<String>();
                            
                            String id = t["id"].as<String>();
                            
                            // Abgleich mit unserer Tracking-ID
                            if (cId == trackingId || id == trackingId) {
                                String rawStatus = t["status"].as<String>();
                                log("Treffer! Status: " + rawStatus);
                                
                                if (rawStatus == "SUCCESSFUL") status = "SUCCESSFUL";
                                else if (rawStatus == "FAILED" || rawStatus == "CANCELLED") {
                                    status = "FAILED";
                                    if (!t["error_message"].isNull()) log("Grund: " + t["error_message"].as<String>());
                                }
                                found = true;
                                break; 
                            }
                        }
                        if (!found) {
                            // ID noch nicht in der History aufgetaucht (normal kurz nach Start)
                        }
                    } else {
                        log("History leer.");
                    }
                } else {
                    log("JSON Fehler in checkStatus. Länge: " + String(payload.length()));
                }
            } else {
                log("HTTP Fehler History: " + String(httpCode));
            }
            http.end();
        } else {
            log("Verbindungsfehler bei checkStatus");
        }
        return status;
    }

    // --- ABBRUCH / TIMEOUT ---
    /** @brief Bricht den Vorgang am Terminal ab (Reset in Idle). */
    void cancel() {
        if (readerId.length() < 5) return;
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        // Zwingt Terminal in den Idle-Modus
        String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers/" + readerId + "/terminate";
        log("Sende Terminate Befehl...");
        if (http.begin(client, url)) {
            http.addHeader("Authorization", "Bearer " + apiKey);
            int code = http.POST(""); 
            log("Terminate Code: " + String(code));
            http.end();
        }
    }
};
#endif