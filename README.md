# 🍯 HANIMAT – Der Open-Source Honigautomat (V1.4.0-ec)

> **🚀 RELEASE-UPDATE (V1.4.0-ec):**
> Dies ist der bisher größte Meilenstein! Der HANIMAT unterstützt nun offiziell **bargeldloses Bezahlen via SumUp**. 
> Zusätzlich wurden massive Optimierungen an der Hardware-Schonung, der Benutzeroberfläche und dem Update-Prozess vorgenommen.

Willkommen beim HANIMAT! Dieses Projekt wurde ins Leben gerufen, um Imkern und Direktvermarktern eine **einfache, moderne und kostengünstige Möglichkeit** zu bieten, Produkte rund um die Uhr selbstständig zu verkaufen – ganz ohne teure Industrielösungen.

## 🌟 Was ist neu in Version 1.4.0-ec?

| Feature | Details |
| :--- | :--- |
| **SumUp Integration** 💳 | Volle Unterstützung für bargeldlose Zahlungen via SumUp Terminal. |
| **Online Update (OTA)** ☁️ | Firmware-Updates jetzt kinderleicht per Klick im Webinterface installieren. |
| **Flash-Schonung** 💾 | Intelligentes Speichermanagement schont den ESP32 (Guthaben primär im RAM). |
| **Coin Acceptor 2.0** 💰 | Angepasste und stabilere Logik für die Münzerkennung. |
| **Modernes WebInterface** 🌐 | Optisches Redesign des Admin-Panels für bessere mobile Bedienung. |
| **Unified Display UI** 🖥️ | Vereinheitlichte Statusmeldungen und Symbole am TFT-Display. |

## ✨ Die vier Säulen des HANIMAT

* **Open Source** 📖: Alle Pläne, Codes und Anleitungen sind frei verfügbar.
* **DIY-freundlich** 🛠️: Gebaut mit Standard-Komponenten (ESP32, ILI9341).
* **Flexibel** 📐: Skalierbar von 6 bis zu 16 Fächern via I2C-Relaiskarte.
* **Community** 🤝: HANIMAT lebt vom Austausch. Deine Ideen sind willkommen!

## 🚀 Kernfunktionen & Technik

* **Hybrid-Zahlung**: Unterstützt Münzprüfer, Banknotenprüfer und SumUp (EC/Kreditkarte/Smartphone).
* **Hardware-Langlebigkeit**: Minimierte Schreibzyklen auf den Flash-Speicher erhöhen die Lebensdauer deines Automaten massiv.
* **Echtzeit-Infos**: Telegram-Benachrichtigungen bei Verkäufen, Bestandsänderungen oder Störungen.
* **WLAN-Konfiguration**: Einfacher WiFi-Manager mit eigenem Access Point für die Ersteinrichtung.
* **Offline-Modus**: Betrieb auch ohne Internetverbindung möglich (lokales Backup).

## 🔌 Hardware & Pinbelegung

| Komponente | Anschluss an ESP32 | Beschreibung |
| :--- | :--- | :--- |
| **TFT Display** | SPI (Pins 18, 23, 16, 4, 26) | Visualisierung der GUI |
| **SumUp (Serial)** | *Siehe SumUpController.h* | Anbindung des EC-Terminals |
| **Münzprüfer** | GPIO 5 | Impuls-Eingang Münzen |
| **Banknotenprüfer** | GPIO 32 | Impuls-Eingang Scheine |
| **I2C Relais** | GPIO 21 (SCL) / 22 (SDA) | Steuerung der Fach-Magnetventile |
| **Keypad** | Matrix (7 Pins) | Produktauswahl am Gerät |
| **Buzzer** | GPIO 25 | Akustisches Feedback |

## 📡 Community-Status & Transparenz

Um die Weiterentwicklung zu motivieren, sendet die Firmware einmal pro Stunde ein technisches "Lebenszeichen".
📊 **Live-Status ansehen:** [status.hanimat.at](https://status.hanimat.at)

* **Datenschutz:** Wir speichern nur die Hardware-ID (Zähler), Version und das Land. Keine IP-Adressen, keine Standorte, keine Umsätze.
* **Deaktivierung:** Wer das nicht möchte, setzt in der `main.cpp`: `bool statusEnabled = false;`.

## 🛠️ Installation & Online-Update

### Erstinstallation
1.  **Visual Studio Code + PlatformIO** installieren.
2.  Repository klonen, ESP32 per USB anschließen und auf **Upload** klicken.

### Zukünftige Updates
Dank der neuen **OTA-Funktion** (Over-the-Air) musst du den Automaten nie wieder aufschrauben. Sobald ein Update verfügbar ist, kannst du es mit einem Klick im Webinterface einspielen.

## 📽️ Video-Anleitungen & Support

Schau dir den Aufbau und die Funktionen im Detail an:
➡️ [YouTube-Kanal: Thomas Schöpf](https://www.youtube.com/@schoepf-tirol)
➡️ [Diskussionen & Hilfe auf GitHub](https://github.com/Zenutrix/Hanimat/discussions)

## 📜 Lizenz & Urheberrecht

* **Urheber:** Thomas Schöpf – [www.schoepf-tirol.at](https://www.schoepf-tirol.at)
* **Projektseite:** [www.hanimat.at](https://www.hanimat.at)
* **Lizenz:** [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

> **Namensnennung erforderlich:** Bitte gib bei Weitergabe "Thomas Schöpf – HANIMAT-Projekt" als Urheber an. Die kommerzielle Nutzung oder der Verkauf der Software/Pläne ist untersagt.

---
**Let's bring local products to the people – mit deinem eigenen HANIMAT!**