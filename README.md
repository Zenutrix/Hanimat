# 🍯 HANIMAT – Der Open-Source Honigautomat (V1.4.0-ec)

> **🚀 RELEASE-UPDATE (V1.4.0-ec):**
> Dies ist der bisher größte Meilenstein! Der HANIMAT unterstützt nun offiziell **bargeldloses Bezahlen via SumUp**. 
> Zusätzlich wurden massive Optimierungen an der Hardware-Schonung und der Benutzeroberfläche vorgenommen.

Willkommen beim HANIMAT! Dieses Projekt bietet Imkern und Direktvermarktern eine **einfache, moderne und kostengünstige Möglichkeit**, Produkte rund um die Uhr selbstständig zu verkaufen.

## 🌟 Was ist neu in Version 1.4.0-ec?

| Feature | Details |
| :--- | :--- |
| **SumUp Integration** 💳 | Volle Unterstützung für bargeldlose Zahlungen via SumUp Air/Solo. |
| **Flash-Schonung** 💾 | Guthabenstände werden zur Hardware-Schonung prioritär im RAM verwaltet. |
| **Coin Acceptor 2.0** 💰 | Optimierte Logik für stabilere Münzerkennung und Kalibrierung. |
| **Modernes WebInterface** 🌐 | Optisch überarbeitetes Admin-Panel für bessere Übersicht auf Mobilgeräten. |
| **Unified Display UI** 🖥️ | Vereinheitlichte Fehlermeldungen und Statusanzeigen am TFT. |

## ✨ Die vier Säulen des HANIMAT

* **Open Source** 📖: Alle Pläne, Codes und Anleitungen sind frei verfügbar.
* **DIY-freundlich** 🛠️: Gebaut mit Standard-Komponenten (ESP32, ILI9341).
* **Flexibel** 📐: Skalierbar von 6 bis zu 16 Fächern via I2C.
* **Community** 🤝: HANIMAT lebt vom Austausch und deinen Ideen!

## 🚀 Einstieg & Hardware

In diesem Repository findest du alles für die **Hybrid-Version (Bargeld + EC)**:

📂 **GitHub Repository:** [https://github.com/Zenutrix/Hanimat/](https://github.com/Zenutrix/Hanimat/)

### ✅ Funktionsübersicht der Firmware
* **Zahlungsmethoden**: Münzprüfer, Banknotenprüfer & SumUp Terminal.
* **Hardware-Schutz**: Minimierte Schreibzyklen auf den internen Speicher für maximale Lebensdauer des ESP32.
* **Konnektivität**: WiFi-Manager, Telegram-Benachrichtigungen bei Verkauf/Fehler.
* **Admin-Panel**: Passwortgeschützte Verwaltung von Preisen und Beständen.
* **OTA-Updates**: Bequemes Update der Firmware über das Webinterface.

## 🔌 Hardware & Pinbelegung (Aktualisiert)

| Komponente | Anschluss an ESP32 | Beschreibung |
| :--- | :--- | :--- |
| **SumUp (Serial)** | *Siehe SumUpController.h* | Anbindung des Terminals |
| **Münzprüfer** | GPIO 5 | COIN_ACCEPTOR_PIN |
| **Banknotenprüfer** | GPIO 32 | BILL_ACCEPTOR_PIN |
| **TFT Display** | SPI (Standard) | Visualisierung & GUI |
| **I2C Relais** | GPIO 21 (SCL) / 22 (SDA) | Steuerung der Fach-Magnetventile |

## 📡 Community-Status & Transparenz

Um die Weiterentwicklung zu motivieren, sendet die Firmware einmal pro Stunde ein technisches "Lebenszeichen".
📊 **Live-Status:** [status.hanimat.at](https://status.hanimat.at)

* **Privatsphäre:** Es werden keine persönlichen Daten oder Umsätze übertragen. Nur Hardware-ID, Version und Land.
* **Deaktivierung:** In der `main.cpp` via `bool statusEnabled = false;` möglich.

## 🛠️ Installation & Setup

1.  **VS Code + PlatformIO** installieren.
2.  Repository klonen oder herunterladen.
3.  `platformio.ini` prüfen (Version 1.4.0-ec ist voreingestellt).
4.  ESP32 verbinden und auf **Upload** klicken.

## 🌐 WLAN-Ersteinrichtung

| Modus | SSID | Passwort |
| :--- | :--- | :--- |
| **Setup-Modus** | HANIMAT-Setup | `Honig1234` |
| **Offline-Modus** | HANIMAT-Offline | `Honig1234` |

*Nach der Verbindung öffnet sich automatisch das Portal zur Eingabe deiner WLAN-Daten.*

## 📜 Lizenz & Urheberrecht

* **Urheber:** Thomas Schöpf – [www.schoepf-tirol.at](https://www.schoepf-tirol.at)
* **PCB-Design:** Roland Rust
* **Projektseite:** [www.hanimat.at](https://www.hanimat.at)
* **Lizenz:** [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

> **Namensnennung erforderlich:** Bitte gib bei Veröffentlichung "Thomas Schöpf – HANIMAT-Projekt" als Urheber an. Kommerzielle Nutzung der Software/Pläne ist untersagt.