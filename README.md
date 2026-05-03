<p align="center">
  <img src="https://www.hanimat.at/img/Automat.jpg" alt="HANIMAT Automat" width="480"/>
</p>

<h1 align="center">🍯 HANIMAT</h1>
<h3 align="center">Der Open-Source Verkaufsautomat für Imker & Direktvermarkter</h3>

<p align="center">
  <a href="https://www.hanimat.at"><img src="https://img.shields.io/badge/Website-hanimat.at-orange?style=flat-square&logo=firefoxbrowser&logoColor=white" /></a>
  <a href="https://www.hanimat.at/installer/"><img src="https://img.shields.io/badge/⚡_Web--Installer-Jetzt_flashen-success?style=flat-square" /></a>
  <a href="https://t.me/+igwol5kmQGpiYWFk"><img src="https://img.shields.io/badge/Community-Telegram-2CA5E0?style=flat-square&logo=telegram&logoColor=white" /></a>
  <a href="https://status.hanimat.at"><img src="https://img.shields.io/badge/Live_Status-status.hanimat.at-brightgreen?style=flat-square" /></a>
  <img src="https://img.shields.io/badge/Firmware-V1.5.0-informational?style=flat-square" />
  <a href="https://creativecommons.org/licenses/by-nc-sa/4.0/"><img src="https://img.shields.io/badge/Lizenz-CC%20BY--NC--SA%204.0-lightgrey?style=flat-square" /></a>
</p>

---

## Inhaltsverzeichnis

- [Was ist HANIMAT?](#-was-ist-hanimat)
- [Demo & Screenshots](#-demo--screenshots)
- [Features](#-features)
- [Voraussetzungen](#-voraussetzungen)
- [Installation](#-installation)
- [Konfiguration](#️-konfiguration)
- [Hardware & Pinbelegung](#-hardware--pinbelegung)
- [SumUp-Einrichtung](#-sumup-einrichtung)
- [OTA-Updates](#-ota-updates)
- [Community-Status](#-community-status--datenschutz)
- [Beitragen](#-beitragen)
- [Support & Community](#-support--community)
- [Lizenz](#-lizenz)

---

## 🐝 Was ist HANIMAT?

HANIMAT ist eine **vollständige Open-Source-Lösung** für einen selbst gebauten Verkaufsautomaten auf Basis des ESP32-Mikrocontrollers. Das Projekt entstand aus dem Wunsch, Imkern, Bauernhöfen und Direktvermarktern eine **einfache, moderne und kostengünstige Möglichkeit** zu geben, Produkte rund um die Uhr selbstständig zu verkaufen – ganz ohne teure Industrieautomaten.

**Kernprinzipien:**
- 🔓 **Offen** – Vollständiger Quellcode, Schaltpläne und Anleitungen sind frei verfügbar
- 🛠️ **DIY-freundlich** – Aufgebaut mit handelsüblichen Komponenten (ESP32, ILI9341, Relais-Karte)
- 📐 **Flexibel** – Skalierbar von 6 bis 16 Fächern, anpassbar für verschiedene Produkte
- 🤝 **Community-getrieben** – Weiterentwickelt durch und für seine Nutzer

---

## 📸 Demo & Screenshots

<table>
  <tr>
    <td align="center">
      <b>TFT-Display beim Start</b><br/><br/>
      <img src="https://www.hanimat.at/img/Start.png" alt="Display Startbildschirm" width="260"/>
    </td>
    <td align="center">
      <b>Web-Interface (Admin-Panel)</b><br/><br/>
      <img src="https://www.hanimat.at/img/Startseite%20Web.png" alt="Web-Interface" width="260"/>
    </td>
  </tr>
</table>

📽️ Video-Anleitungen und Aufbauvideos: **[YouTube – Thomas Schöpf](https://www.youtube.com/@schoepf-tirol)**

---

## ✨ Features

| Bereich | Details |
| :--- | :--- |
| **Zahlungsarten** | Münzprüfer, Banknotenprüfer, SumUp EC-Terminal (Karte / NFC / Smartphone) |
| **Hybrid-Zahlung** | Bargeld + Kartenzahlung für den Restbetrag kombinierbar |
| **Display** | ILI9341 TFT 320×240, vollständige Farb-UI mit Statusanzeigen |
| **Web-Interface** | Responsives Admin-Panel, vollständig bedienbar vom Smartphone |
| **Fächer** | 6–16 Fächer via I2C-Relaiskarte, jedes Fach einzeln konfigurierbar |
| **Preise** | Individuelle Preise pro Fach, Bulk-Eingabe per Tabelle |
| **Benachrichtigungen** | Telegram-Bot: Verkäufe, Bestandswarnungen, Absturzberichte, Heap-Warnungen |
| **Firmware-Updates** | OTA per Web-Interface – kein USB-Kabel nach der Erstinstallation nötig |
| **Sicherheit** | Login mit Session-Token, Brute-Force-Schutz (automatische IP-Sperre) |
| **Statistik** | Verkaufslog (RAM), Umsatzübersicht, Absturzzähler mit Reset-Funktion |
| **Stabilität** | Non-blocking Loop, Heap-Monitoring, Absturzprotokoll via `esp_reset_reason()` |
| **Flash-Schonung** | Guthaben im RAM verwaltet, minimale NVS-Schreibzyklen |
| **Community-Status** | Anonymes Lebenszeichen an status.hanimat.at (jederzeit abschaltbar) |

---

## 📋 Voraussetzungen

### Software
- [Visual Studio Code](https://code.visualstudio.com/) mit [PlatformIO-Extension](https://platformio.org/install/ide?install=vscode) **– nur für Entwickler / eigene Builds**
- Oder: Direkt per **[Web-Installer](https://www.hanimat.at/installer/)** flashen (kein Software-Setup nötig)

### Hardware
| Komponente | Empfehlung |
| :--- | :--- |
| Mikrocontroller | ESP32 Dev Board (38-Pin) |
| Display | ILI9341 TFT 2,8" 320×240 SPI |
| Relais-Karte | PCF8574 I2C 8-Kanal (erweiterbar auf 16) |
| Münzprüfer | HX-616 oder kompatibel (Impuls-Ausgang) |
| Banknotenprüfer | ITL oder kompatibel (Impuls-Ausgang) |
| Keypad | 4×4 Matrix-Keypad |
| Buzzer | Aktiver Buzzer 5V |
| Netzteil | 5V / min. 2A |

### Accounts (optional)
- **SumUp** – für bargeldlose Zahlung: [sumup.com](https://sumup.com)
- **Telegram** – für Benachrichtigungen: Bot via [@BotFather](https://t.me/BotFather) erstellen

---

## 🚀 Installation

### ⚡ Option 1 – Web-Installer (empfohlen, kein Setup nötig)

Der einfachste Weg: Direkt im Browser flashen – ohne Software-Installation, ohne Kommandozeile.

> **➡️ [hanimat.at/installer](https://www.hanimat.at/installer/)**

1. ESP32 per USB mit dem Computer verbinden
2. Den Web-Installer im **Chrome- oder Edge-Browser** öffnen (Web Serial API erforderlich)
3. Port auswählen und auf **„Installieren"** klicken
4. Der Installer flasht Firmware und Partition-Schema automatisch
5. Nach dem Neustart erscheint der WLAN-Hotspot `HANIMAT-Setup` zur Erstkonfiguration

### 🛠️ Option 2 – PlatformIO (für Entwickler & eigene Builds)

```bash
# Repository klonen
git clone https://github.com/YOUR_USERNAME/hanimat.git
cd hanimat

# In VS Code öffnen
code .
```

1. PlatformIO-Extension in VS Code installieren
2. ESP32 per USB anschließen (Port `COM3` bzw. `/dev/ttyUSB0`)
3. In PlatformIO: **Upload** klicken (`platformio.ini` ist vorkonfiguriert)
4. Beim ersten Start: WLAN-Hotspot `HANIMAT-Setup` verbinden und WLAN-Daten eingeben

### Erstkonfiguration nach dem Flashen

1. Mit dem WLAN-Hotspot **`HANIMAT-Setup`** verbinden
2. Im Browser `192.168.4.1` aufrufen → WLAN-Zugangsdaten eingeben
3. ESP32 verbindet sich und zeigt die IP-Adresse am Display
4. Web-Interface unter der angezeigten IP aufrufen und einloggen
5. Passwort, Fachanzahl, Preise und weitere Einstellungen konfigurieren

---

## ⚙️ Konfiguration

Alle Einstellungen sind über das **Web-Interface** erreichbar – kein Editieren von Quellcode nötig.

| Bereich | Beschreibung |
| :--- | :--- |
| **System** | Passwort, Gerätename, Display-Timeout, Offline-Modus |
| **Fächer** | Anzahl, Preise, Verfügbarkeit, Sperren einzelner Fächer |
| **Zahlung** | Münzwerte, Banknotenwerte, Gutschrift-Funktion |
| **SumUp** | API-Key, Merchant-ID, Reader pairen/trennen/prüfen |
| **Telegram** | Bot-Token, Chat-ID, Benachrichtigungstypen aktivieren |
| **System / OTA** | Firmware-Update, Neustart, Community-Status an/aus |

### Wichtige `platformio.ini`-Flags

```ini
board_build.partitions = min_spiffs.csv   ; ~1,9 MB pro OTA-Slot (nötig für OTA bei >1,25 MB Firmware)
build_flags =
    -Os                    ; Größenoptimierung (~15–25% kleinere Binary)
    -DCORE_DEBUG_LEVEL=0   ; Arduino-Core Debug-Output deaktiviert (spart Flash + RAM)
```

---

## 🔌 Hardware & Pinbelegung

| Komponente | ESP32-Pin(s) | Hinweis |
| :--- | :--- | :--- |
| **TFT Display (ILI9341)** | CLK: 18 · MOSI: 23 · CS: 16 · DC: 4 · RST: 26 | SPI-Bus |
| **Münzprüfer** | GPIO 5 | Impuls-Eingang |
| **Banknotenprüfer** | GPIO 32 | Impuls-Eingang |
| **I2C Relais-Karte** | SDA: 21 · SCL: 22 | PCF8574, Adresse 0x20 |
| **Keypad** | Matrix, 7 Pins | Rows/Cols siehe `main.cpp` |
| **Buzzer** | GPIO 25 | Aktiver Buzzer |
| **Offline-Mode Jumper** | GPIO 34 | LOW = kein WiFi/SumUp |

---

## 💳 SumUp-Einrichtung

HANIMAT unterstützt bargeldloses Bezahlen über ein **SumUp Solo**-Terminal (Karte, NFC, Apple Pay, Google Pay).

**Benötigte Daten** (im [SumUp Dashboard](https://me.sumup.com) abrufbar):
- Personal API Key (`sup_sk_...`)
- Merchant Code (z. B. `MPCB1CS4`)

**Einrichtung Schritt für Schritt:**
1. Web-Interface → **SumUp** → API-Key und Merchant-ID eintragen → „Speichern"
2. SumUp Solo-Terminal in den Pairing-Modus versetzen (Terminal → Einstellungen → Koppeln)
3. Den angezeigten **8-stelligen Code** im Web-Interface unter „Pairing" eingeben → „Koppeln"
4. Button **„🔌 Reader prüfen"** klicken – bei Erfolg ist SumUp sofort einsatzbereit

**Zahlungsablauf:** Produkt wählen → SumUp-Taste drücken → Terminal zeigt Betrag → Karte/NFC → Ausgabe automatisch. Mischzahlung (Bargeld + Karte für den Restbetrag) wird vollautomatisch unterstützt.

---

## ☁️ OTA-Updates

Nach der Erstinstallation musst du den Automaten **nie wieder aufschrauben**:

1. Neue `firmware.bin` von [hanimat.at](https://www.hanimat.at) herunterladen
2. Web-Interface → **System** → **Firmware-Update** → Datei hochladen
3. ESP32 flasht sich selbst und startet automatisch neu (~30 Sekunden)

> Die Firmware verwendet das `min_spiffs`-Partitionsschema mit **~1,9 MB pro OTA-Slot** – damit funktionieren auch zukünftige, größere Firmware-Versionen zuverlässig per OTA.

---

## 📡 Community-Status & Datenschutz

Die Firmware sendet einmal pro Stunde ein anonymes Lebenszeichen an unsere Statusseite. Das zeigt wie viele HANIMATs aktiv sind und motiviert die Weiterentwicklung.

**📊 Live ansehen:** [status.hanimat.at](https://status.hanimat.at)

**Was wird übertragen:** Anonymisierte Hardware-ID, Firmware-Version, Land. **Keine** IP-Adressen, Standorte oder Umsatzdaten.

**Deaktivierung:** Web-Interface → System → **Community-Status** deaktivieren. Alternativ in `main.cpp`:
```cpp
bool statusEnabled = false;
```

---

## 🤝 Beitragen

Beiträge sind herzlich willkommen! So kannst du helfen:

1. **Fork** dieses Repository
2. Feature-Branch erstellen: `git checkout -b feature/MeinFeature`
3. Änderungen committen: `git commit -m 'Add MeinFeature'`
4. Branch pushen: `git push origin feature/MeinFeature`
5. **Pull Request** öffnen

**Bug gefunden?** Bitte ein [Issue](../../issues) mit Log-Output und Firmware-Version öffnen.

**Ideen & Verbesserungen?** Am besten direkt in der [Telegram-Community](https://t.me/+igwol5kmQGpiYWFk) diskutieren – viele Features entstanden aus Nutzer-Feedback.

---

## 💬 Support & Community

| Kanal | Link |
| :--- | :--- |
| 🌐 Projektseite | [hanimat.at](https://www.hanimat.at) |
| ⚡ Web-Installer | [hanimat.at/installer](https://www.hanimat.at/installer/) |
| 💬 Telegram-Gruppe | [t.me/+igwol5kmQGpiYWFk](https://t.me/+igwol5kmQGpiYWFk) |
| 📽️ YouTube | [Thomas Schöpf](https://www.youtube.com/@schoepf-tirol) |
| 📊 Live-Status | [status.hanimat.at](https://status.hanimat.at) |

---

## 📜 Lizenz

Dieses Projekt steht unter der **Creative Commons Lizenz CC BY-NC-SA 4.0**.

- ✅ Kostenlos nutzbar und veränderbar
- ✅ Weitergabe unter gleicher Lizenz erlaubt
- ❌ Kommerzielle Weiterverkauf ohne Genehmigung nicht erlaubt
- ℹ️ Namensnennung erforderlich: **Thomas Schöpf – [schoepf-tirol.at](https://www.schoepf-tirol.at)**

**[→ Vollständige Lizenzbedingungen](https://creativecommons.org/licenses/by-nc-sa/4.0/)**

---

<p align="center">
  Made with ❤️ in Tirol &nbsp;·&nbsp; <a href="https://www.schoepf-tirol.at">Thomas Schöpf</a> &nbsp;·&nbsp; <a href="https://www.hanimat.at">hanimat.at</a>
</p>
<p align="center">
  <i>Let's bring local products to the people – mit deinem eigenen HANIMAT! 🐝</i>
</p>
