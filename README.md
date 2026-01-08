# 🍯 HANIMAT – Der Open-Source Honigautomat (BETA)

> **⚠️ WICHTIGER HINWEIS ZUM RELEASE (V1.2.5-noec):**
> Dies ist die **erste BETA-Version** der HANIMAT Firmware.
> **Aktueller Status:** Diese Version unterstützt vollständig die **Barzahlung (Münzen & Geldscheine)**.
> Die Integration von EC-Terminals ist in diesem Release **nicht** enthalten. Das **Sigma Terminal von myPos** wurde bereits bestellt und wir warten derzeit auf den Erhalt, um die Integration finalisieren zu können (Stand: 01.12.2025).

Willkommen beim HANIMAT! Dieses Projekt wurde ins Leben gerufen, um Imkern und Direktvermarktern eine **einfache, moderne und kostengünstige Möglichkeit** zu bieten, Produkte rund um die Uhr selbstständig zu verkaufen – ganz ohne teure Automatenlösungen.

## 🌟 Was ist der HANIMAT?

Der HANIMAT ist ein modularer Selbstbedienungsautomat, ideal für den Verkauf von:

* Honig 🍯
* Eiern 🥚
* Marmelade 🍓
* Nudeln 🍝
* Käse 🧀
  und vielen weiteren regionalen Produkten.

Das Projekt basiert vollständig auf **Open Source** und ist **DIY-freundlich**, **flexibel anpassbar** und **community-getrieben**.

## ✨ Die vier Säulen des HANIMAT

| Säule | Beschreibung |
| :--- | :--- |
| **Open Source** 📖 | Alle Pläne, Codes und Anleitungen sind frei verfügbar. |
| **DIY-freundlich** 🛠️ | Gebaut mit Standard-Komponenten und Werkzeugen. |
| **Flexibel** 📐 | Von 6 großen Gläsern bis zu 16 kleinen Fächern – passt sich deinen Bedürfnissen an. |
| **Community** 🤝 | HANIMAT lebt vom Austausch. Deine Ideen sind willkommen! |

## 🚀 Los geht's – Bau deinen eigenen HANIMAT!

In diesem Repository findest du alles, was du für die **Stand-Alone Bargeld-Version** brauchst:

📂 **GitHub Repository:** [https://github.com/Zenutrix/Hanimat/](https://github.com/Zenutrix/Hanimat/)

* ⚡ **Elektronik & Software**: Schaltpläne, Pinout, Materialliste, Firmware

## ⚙️ Technische Details (Beta V1.2.5)

### ✅ Funktionsübersicht der Firmware

* 🖥️ **TFT-Oberfläche** (ILI9341): Bedienfreundliche GUI
* 🔢 **Keypad-Steuerung** (4x3 Matrix): Produktauswahl
* 💰 **Zahlungsabwicklung**: Münz- & Banknotenprüfer (Impuls-basiert)
* 🔌 **Relaisansteuerung**: Bis zu 16 Fächer über I2C-Relaiskarte
* 📶 **WiFi-Manager**: WLAN-Konfiguration über Webportal
* 🌐 **Webinterface**: Verwaltung per Passwort-geschütztem Admin-Panel
* 📲 **OTA-Updates**: Firmware aktualisieren über Web
* 📢 **Telegram-Benachrichtigung**: Statusmeldungen in Echtzeit (Verkäufe, Bestand)
* 🌐 **Offline-Modus**: Betrieb auch ohne Internet (lokaler Access Point)

## 🔌 Hardware & Pinbelegung

| Komponente | Anschluss an ESP32 | Beschreibung |
| :--- | :--- | :--- |
| TFT_CS | GPIO 26 | TFT Chip Select |
| TFT_DC | GPIO 4 | TFT Data/Command |
| TFT_RST | GPIO 16 | TFT Reset |
| TFT_SCK | GPIO 18 | SPI Clock |
| TFT_MOSI | GPIO 23 | SPI Datenleitung |
| I2C SDA (Relais) | GPIO 22 | I2C Datenleitung |
| I2C SCL (Relais) | GPIO 21 | I2C Taktleitung |
| Münzprüfer | GPIO 5 | COIN_ACCEPTOR_PIN |
| Banknotenprüfer | GPIO 32 | BILL_ACCEPTOR_PIN |
| Banknoten-Sperre | GPIO 33 | BILL_INHIBIT_PIN |
| Reset-Taster | GPIO 34 | WIFI_RESET_BUTTON |
| Offline-Modus Schalter | GPIO 27 | OFFLINE_MODE_PIN |
| Piezo-Buzzer | GPIO 25 | BUZZER_PIN |
| Keypad Zeilen | GPIO 15, 14, 12, 17 | KEYPAD_ROWS |
| Keypad Spalten | GPIO 2, 19, 13 | KEYPAD_COLS |

## 🐛 Bekannte Probleme & Bugfixes

### Derzeit Fehlerfrei :)

## 🛠️ Firmware Installation

**Empfohlen: Visual Studio Code + PlatformIO**

### 1. Setup

* Installiere [Visual Studio Code](https://code.visualstudio.com/)
* Installiere die Erweiterung **PlatformIO IDE**

### 2. Projekt öffnen

* Öffne diesen Repository-Ordner in VS Code
* PlatformIO erkennt automatisch die `platformio.ini`

### 3. Firmware aufspielen

* Verbinde deinen ESP32 per USB
* In PlatformIO unten auf **→ Upload** klicken

## 🌐 WLAN-Ersteinrichtung

| Modus | Auslöser (GPIO 27) | SSID | Passwort |
| :--- | :--- | :--- | :--- |
| **Offline-Modus** | GND verbunden | HANIMAT-Offline | `Honig1234` |
| **Online-Modus** | GND nicht verbunden | HANIMAT-Setup | `Honig1234` |

* Verbinde dich mit dem WLAN
* Das Konfigurationsportal öffnet sich im Browser (meist automatisch, sonst 192.168.4.1)
* Nach Verbindung mit deinem Heim-WLAN zeigt das Display die IP-Adresse an
* Zugriff auf das Web-Panel über diese IP (Standard-Login: `admin`)

## 📽️ Video-Anleitungen

Schau dir den Aufbau auf meinem YouTube-Kanal an:

➡️ [Zum Kanal von Thomas Schöpf](https://www.youtube.com/@schoepf-tirol)

## 🧠 Community & Austausch

Stell Fragen, teile deine Ideen und zeig deine eigene HANIMAT-Version:

➡️ [GitHub Diskussionen](https://github.com/Zenutrix/Hanimat/discussions)

## 📜 Lizenz & Urheberrecht

* **Urheber:** Thomas Schöpf – [www.schoepf-tirol.at](https://www.schoepf-tirol.at)
* **PCB-Desing:** Roland Rust
* **Projektseite:** [www.hanimat.at](https://www.hanimat.at)
* **Lizenz:** [CC BY-NC-SA 4](https://creativecommons.org/licenses/by-nc-sa/4.0/)

### Was du darfst ✅

* HANIMAT-Pläne & Software frei nutzen und anpassen
* Produkte (z. B. Honig, Eier) über deinen Automaten verkaufen

### Was du nicht darfst ❌

* Die Software/Pläne oder direkte Ableitungen **verkaufen**
* Kommerzielle Automatenlösungen basierend auf HANIMAT vertreiben

**Namensnennung erforderlich** bei Weitergabe/Veröffentlichung:

Bitte gib *Thomas Schöpf – HANIMAT-Projekt* als Urheber an.

> **Let's bring local products to the people – mit deinem eigenen HANIMAT!**
        