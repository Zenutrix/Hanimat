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
  <img src="https://img.shields.io/badge/Firmware-V1.5.2-informational?style=flat-square" />
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
- [Zahlungs-Mapping](#-zahlungs-mapping-münzen--banknoten)
- [SumUp-Einrichtung](#-sumup-einrichtung)
- [OTA-Updates](#️-ota-updates)
- [Community-Status](#-community-status--datenschutz)
- [Beitragen](#-beitragen)
- [Support & Community](#-support--community)
- [Lizenz](#-lizenz)

---

## 🐝 Was ist HANIMAT?

HANIMAT ist eine **vollständige Open-Source-Lösung** für einen selbst gebauten Verkaufsautomaten auf Basis des ESP32-Mikrocontrollers. Das Projekt entstand aus dem Wunsch, Imkern, Bauernhöfen und Direktvermarktern eine **einfache, moderne und kostengünstige Möglichkeit** zu geben, Produkte rund um die Uhr selbstständig zu verkaufen – ganz ohne teure Industrieautomaten.

**Kernprinzipien:**
- 🔓 **Offen** – Vollständiger Quellcode, Schaltpläne und Anleitungen sind frei verfügbar
- 🛠️ **DIY-freundlich** – Aufgebaut auf einer eigenen PCB mit MOSFETs sowie einer optionalen Erweiterungsplatine
- 📐 **Massiv skalierbar** – Standard 16 Fächer, mit Erweiterungsplatinen bis zu 128 Fächer möglich
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
| **Fächer** | Standard 16 Fächer, mit Erweiterungsplatine bis zu 128 Fächer skalierbar |
| **Preise** | Individuelle Preise pro Fach, Bulk-Eingabe per Tabelle |
| **Benachrichtigungen** | Telegram-Bot: Verkäufe, Bestandswarnungen, Absturzberichte, Heap-Warnungen |
| **Firmware-Updates** | OTA direkt im Admin-Panel – kein USB-Kabel nach der Erstinstallation nötig |
| **Sicherheit** | Login mit Session-Token, Brute-Force-Schutz (automatische IP-Sperre) |
| **Statistik** | Verkaufslog, Umsatzübersicht, Absturzzähler mit Reset-Funktion |
| **Stabilität** | Non-blocking Loop, Heap-Monitoring, Absturzprotokoll via `esp_reset_reason()` |

---

## 📋 Voraussetzungen

### Software
- Direkt per **[Web-Installer](https://www.hanimat.at/installer/)** flashen – kein Software-Setup nötig
- Oder: [Visual Studio Code](https://code.visualstudio.com/) + [PlatformIO](https://platformio.org/install/ide?install=vscode) für eigene Builds

### Hardware
Alle Infos zur eigenen Platine (PCB, Schaltpläne, Stückliste) findest du hier:
**➡️ [github.com/Zenutrix/Hanimat/tree/main/Hardware](https://github.com/Zenutrix/Hanimat/tree/main/Hardware)**

Sowie auf der Projektseite: **[hanimat.at](https://www.hanimat.at)**

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
git clone https://github.com/Zenutrix/Hanimat.git
cd Hanimat
code .
```

1. PlatformIO-Extension in VS Code installieren
2. ESP32 per USB anschließen
3. In PlatformIO: **Upload** klicken – `platformio.ini` ist vorkonfiguriert

### Erstkonfiguration nach dem Flashen

1. Mit dem WLAN-Hotspot **`HANIMAT-Setup`** verbinden
2. Im Browser `192.168.4.1` aufrufen → WLAN-Zugangsdaten eingeben
3. ESP32 verbindet sich und zeigt die IP-Adresse am Display an
4. Web-Interface unter der angezeigten IP öffnen und einloggen (Standard-Passwort: `admin`)
5. Passwort sofort ändern, Fachanzahl, Preise und weitere Einstellungen konfigurieren

---

## ⚙️ Konfiguration

Alle Einstellungen sind über das **Web-Interface** erreichbar – kein Editieren von Quellcode nötig.

| Bereich | Beschreibung |
| :--- | :--- |
| **System** | Passwort, Display-Timeout, Offline-Modus |
| **Fächer** | Anzahl, Preise, Verfügbarkeit, Sperren einzelner Fächer |
| **Zahlung** | Münzwerte, Banknotenwerte, Gutschrift-Funktion |
| **SumUp** | API-Key, Merchant-ID, Reader pairen / trennen / prüfen |
| **Telegram** | Bot-Token, Chat-ID, Benachrichtigungstypen aktivieren |
| **OTA** | Firmware automatisch prüfen & installieren oder manuell `.bin` hochladen |

---

## 🔌 Hardware & Pinbelegung

Alle Pinbelegungen direkt aus der Firmware (`src/main.cpp`):

### TFT Display (ILI9341, SPI)

| Funktion | ESP32-Pin |
| :--- | :--- |
| CS (Chip Select) | GPIO 26 |
| DC (Data/Command) | GPIO 4 |
| RST (Reset) | GPIO 16 |
| SCK (Clock) | GPIO 18 |
| MOSI (Data) | GPIO 23 |
| MISO | nicht verwendet |

### Eingaben & Peripherie

| Komponente | ESP32-Pin | Hinweis |
| :--- | :--- | :--- |
| **Münzprüfer** | GPIO 5 | Impuls-Eingang (ISR) |
| **Banknotenprüfer** | GPIO 32 | Impuls-Eingang (ISR, mit Debounce) |
| **Banknoten-Inhibit** | GPIO 33 | Sperrt Banknoteneinzug |
| **Buzzer** | GPIO 25 | Aktiver Buzzer |
| **SumUp-Taste** | GPIO 0 | Kartenzahlung auslösen (BOOT-Taster) |
| **Offline-Mode Jumper** | GPIO 27 | LOW = kein WiFi, kein SumUp |

### Keypad (4×3 Matrix)

```
Layout:   1  2  3
          4  5  6
          7  8  9
          *  0  #
```

| | Pin |
| :--- | :--- |
| **Rows** (R1–R4) | GPIO 15, 14, 12, 17 |
| **Cols** (C1–C3) | GPIO 2, 19, 13 |

### I2C (Erweiterungsplatine / PCB)

| Funktion | ESP32-Pin |
| :--- | :--- |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| I2C-Adresse | 0x20 |

---

## 💶 Zahlungs-Mapping (Münzen & Banknoten)

Die Impulsanzahl des Prüfers wird direkt auf einen Betrag gemappt. Werte direkt aus der Firmware:

### Münzen – `pulseValues[]`

| Impulse | Betrag |
| :--- | :--- |
| 2 | 0,10 € |
| 3 | 0,20 € |
| 4 | 0,50 € |
| 5 | 1,00 € |
| 6 | 2,00 € |

### Banknoten – `billValues[]`

| Impulse | Betrag |
| :--- | :--- |
| 4 | 5 € |
| 8 | 10 € |
| 16 | 20 € |

> Die Impulsbelegung hängt vom verwendeten Münz-/Banknotenprüfer ab und kann in `main.cpp` in den Arrays `pulseValues[]` und `billValues[]` angepasst werden.

---

## 💳 SumUp-Einrichtung

HANIMAT unterstützt bargeldloses Bezahlen über ein **SumUp Solo**-Terminal (Karte, NFC, Apple Pay, Google Pay).

**Benötigte Daten** (im [SumUp Dashboard](https://me.sumup.com) abrufbar):
- Personal API Key (`sup_sk_...`)
- Merchant Code (z. B. `MPCB1CS4`)

**Einrichtung:**
1. Web-Interface → **SumUp** → API-Key und Merchant-ID eintragen → „Speichern"
2. SumUp Solo-Terminal in den Pairing-Modus versetzen (Terminal → Einstellungen → Koppeln)
3. Den angezeigten **8-stelligen Code** im Web-Interface unter „Pairing" eingeben → „Koppeln"
4. Button **„🔌 Reader prüfen"** klicken – bei Erfolg ist SumUp sofort einsatzbereit

**Zahlungsablauf:** Produkt wählen → SumUp-Taste drücken → Terminal zeigt Betrag → Karte/NFC → Ausgabe automatisch. Mischzahlung (Bargeld + Karte für den Restbetrag) wird vollautomatisch unterstützt.

---

## 🛰️ OTA-Updates

Nach der Erstinstallation musst du den Automaten **nie wieder aufschrauben**. Updates werden direkt im Admin-Panel verwaltet:

**Automatisch (empfohlen):** Web-Interface → **System** → **Firmware-Update** → HANIMAT prüft auf neue Version und installiert Firmware + Web-Interface mit einem Klick. Nach dem Neustart ist alles aktuell.

**Manuell:** Im selben Bereich können Firmware (`firmware.bin`) und Web-Interface (`littlefs.bin`) auch separat als Datei hochgeladen werden – der ESP32 flasht sich selbst und startet automatisch neu.

---

## 📡 Community-Status & Datenschutz

Die Firmware sendet einmal pro Stunde ein anonymes Lebenszeichen an unsere Statusseite. Das zeigt wie viele HANIMATs aktiv sind und motiviert die Weiterentwicklung.

📊 **Live ansehen:** [status.hanimat.at](https://status.hanimat.at)

**Was wird übertragen:** Ausschließlich Firmware-Version und Herkunftsland – sonst nichts.

**Deaktivierung:** Web-Interface → System → **Community-Status** deaktivieren.

---

## 🤝 Beitragen

Beiträge sind herzlich willkommen! So kannst du helfen:

1. **Fork** dieses Repository
2. Feature-Branch erstellen: `git checkout -b feature/MeinFeature`
3. Änderungen committen: `git commit -m 'Add MeinFeature'`
4. Branch pushen: `git push origin feature/MeinFeature`
5. **Pull Request** öffnen

**Bug gefunden oder Frage?** Melde dich direkt in unserer [Telegram-Community](https://t.me/+igwol5kmQGpiYWFk) – dort sind wir aktiv und helfen schnell weiter.

---

## 💬 Support & Community

| Kanal | Link |
| :--- | :--- |
| 🌐 Projektseite | [hanimat.at](https://www.hanimat.at) |
| ⚡ Web-Installer | [hanimat.at/installer](https://www.hanimat.at/installer/) |
| 🔧 Hardware / PCB | [github.com/Zenutrix/Hanimat/Hardware](https://github.com/Zenutrix/Hanimat/tree/main/Hardware) |
| 💬 Telegram-Gruppe | [t.me/+igwol5kmQGpiYWFk](https://t.me/+igwol5kmQGpiYWFk) |
| 📽️ YouTube | [Thomas Schöpf](https://www.youtube.com/@schoepf-tirol) |
| 📊 Live-Status | [status.hanimat.at](https://status.hanimat.at) |

---

## 📜 Lizenz

Dieses Projekt steht unter der **Creative Commons Lizenz CC BY-NC-SA 4.0**.

- ✅ Kostenlos nutzbar und veränderbar
- ✅ Weitergabe unter gleicher Lizenz erlaubt
- ❌ Kommerzieller Weiterverkauf ohne Genehmigung nicht erlaubt
- ℹ️ Namensnennung erforderlich: **Thomas Schöpf – [schoepf-tirol.at](https://www.schoepf-tirol.at)**

**[→ Vollständige Lizenzbedingungen](https://creativecommons.org/licenses/by-nc-sa/4.0/)**

---

<p align="center">
  Made with ❤️ in Tirol &nbsp;·&nbsp; <a href="https://www.schoepf-tirol.at">Thomas Schöpf</a> &nbsp;·&nbsp; <a href="https://www.hanimat.at">hanimat.at</a>
</p>
<p align="center">
  <i>Let's bring local products to the people – mit deinem eigenen HANIMAT! 🐝</i>
</p>
