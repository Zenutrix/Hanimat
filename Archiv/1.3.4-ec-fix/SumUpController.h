#ifndef SUMUP_CONTROLLER_H
#define SUMUP_CONTROLLER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Verknüpfung zur Hanimat-Log-Funktion herstellen
// Diese Funktion muss in der main.cpp existieren
extern void logMessage(const String& msg);

class SumUpController {
private:
    String apiKey;
    String merchantId;
    String readerId;
    
    // Leitet SumUp-Interne Logs an das Hanimat System weiter
    void log(String msg) {
        logMessage("[SumUp Internal] " + msg);
    }

public:
    SumUpController(String key, String mId, String rId = "") {
        apiKey = key;
        merchantId = mId;
        readerId = rId;
    }

    void setReaderId(String id) { readerId = id; }
    String getReaderId() { return readerId; }

    // --- PAIRING ---
    // Verbindet das physische Terminal mit diesem Controller
    String pairReader(String pairingCode) {
        if (apiKey.length() < 5 || merchantId.length() < 3) return "";
        WiFiClientSecure client; client.setInsecure(); HTTPClient http;
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

    // --- ZAHLUNG STARTEN ---
    // Sendet den Zahlungsbefehl an die SumUp Cloud
    bool startPayment(float amount, String &outId) {
        if (apiKey.length() < 5 || readerId.length() < 5) return false;
        WiFiClientSecure client; client.setInsecure(); HTTPClient http;
        String url = "https://api.sumup.com/v0.1/merchants/" + merchantId + "/readers/" + readerId + "/checkout";
        
        if (!http.begin(client, url)) return false;
        http.addHeader("Authorization", "Bearer " + apiKey);
        http.addHeader("Content-Type", "application/json");

        JsonDocument doc;
        // Betrag muss in Cent übermittelt werden (minor_unit 2)
        doc["total_amount"]["value"] = (long)(amount * 100);
        doc["total_amount"]["currency"] = "EUR";
        doc["total_amount"]["minor_unit"] = 2;
        doc["checkout_reference"] = "HANI-" + String(random(10000, 99999));
        doc["description"] = "Hanimat " + String(amount, 2) + " EUR";
        doc["return_url"] = "https://sumup.com"; 

        String body; serializeJson(doc, body);
        int httpCode = http.POST(body);
        String response = http.getString();
        bool success = false;

        if (httpCode == 200 || httpCode == 201) {
            JsonDocument res; deserializeJson(res, response);
            // ID extrahieren (Wichtig für History Scan)
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

    // --- STATUS PRÜFEN (History Scan - Robust) ---
    // Sucht die Transaktions-ID in der Historie des Händlers
    String checkStatus(String trackingId) {
        if (trackingId.length() < 5) return "UNKNOWN";
        WiFiClientSecure client; client.setInsecure(); HTTPClient http;
        String status = "PENDING"; 

        // WICHTIGE ÄNDERUNG: 
        // 1. 'order=descending' statt 'desc' (API-Konformität)
        // 2. 'limit=5' reduziert Speicherbedarf und beschleunigt Parsing
        String url = "https://api.sumup.com/v0.1/me/transactions/history?limit=5&order=descending";

        if (http.begin(client, url)) {
            http.addHeader("Authorization", "Bearer " + apiKey);
            int httpCode = http.GET();

            if (httpCode == 200) {
                String payload = http.getString();
                JsonDocument d; 
                
                // Speicher für JSON-Verarbeitung
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
    // Bricht den Vorgang am Terminal ab (Reset in Idle)
    void cancel() {
        if (readerId.length() < 5) return;
        WiFiClientSecure client; client.setInsecure(); HTTPClient http;
        // Zwingt Terminal in den Idle Modus
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