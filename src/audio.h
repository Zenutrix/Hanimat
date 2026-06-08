#pragma once
// =================================================================
//  AUDIO MODULE
//  Melodie- und Buzzer-Logik für den HANIMAT.
//  Wird von main.cpp per #include eingebunden (single translation unit).
// =================================================================

/**
 * @brief Startet die "Danke"-Melodie (non-blocking).
 *        Die eigentliche Wiedergabe übernimmt processMelody() im Loop.
 */
void playThankYouMelody() {
  melodyActive      = true;
  melodyNoteIndex   = 0;
  melodyNotePlaying = true;
  melodyNoteStart   = millis();
  ledcWriteTone(0, MELODY_NOTES[0]); // Erste Note sofort starten
}

/**
 * @brief Verarbeitet die Melodie-Wiedergabe Note für Note.
 *        Muss jeden Loop-Durchlauf aufgerufen werden.
 */
void processMelody() {
  if (!melodyActive) return;
  unsigned long now = millis();

  if (melodyNotePlaying) {
    if (now - melodyNoteStart >= (unsigned long)MELODY_DURATIONS[melodyNoteIndex]) {
      ledcWriteTone(0, 0);       // Ton stoppen
      melodyNotePlaying = false;
      melodyNoteStart   = now;   // Timer für Pause starten
    }
  } else {
    if (now - melodyNoteStart >= (unsigned long)MELODY_NOTE_GAP) {
      melodyNoteIndex++;
      if (melodyNoteIndex >= MELODY_LENGTH) {
        melodyActive    = false; // Melodie vollständig abgespielt
        melodyNoteIndex = 0;
      } else {
        ledcWriteTone(0, MELODY_NOTES[melodyNoteIndex]);
        melodyNotePlaying = true;
        melodyNoteStart   = now;
      }
    }
  }
}

/**
 * @brief Non-blocking beep helper — stoppt den Ton wenn die Zeit abgelaufen ist.
 *        Muss in loop() aufgerufen werden.
 */
void processSingleBeep() {
  if (!singleBeep.active) return;
  if (millis() >= singleBeep.endTime) {
    if (singleBeep.hasNextTone) {
      ledcWriteTone(0, singleBeep.nextFreq);
      singleBeep.endTime     = millis() + singleBeep.nextDuration;
      singleBeep.hasNextTone = false;
    } else {
      ledcWriteTone(0, 0);
      singleBeep.active = false;
    }
  }
}

/**
 * @brief Startet einen nicht-blockierenden Einzel-Beep.
 * @param freq       Frequenz in Hz
 * @param durationMs Dauer in Millisekunden
 */
void startBeep(int freq, int durationMs) {
  melodyActive           = false; // Laufende Melodie stoppen
  singleBeep.active      = true;
  singleBeep.endTime     = millis() + durationMs;
  singleBeep.hasNextTone = false;
  ledcWriteTone(0, freq);
}

/**
 * @brief Zweistufiger Fehler-Sound (2500 Hz → 2000 Hz, non-blocking).
 */
void playErrorSound() {
  melodyActive            = false;
  singleBeep.active       = true;
  singleBeep.endTime      = millis() + 150;
  singleBeep.hasNextTone  = true;
  singleBeep.nextFreq     = 2000;
  singleBeep.nextDuration = 250;
  ledcWriteTone(0, 2500);
}

/**
 * @brief Kurzer Tastendruck-Beep.
 */
void playKeyPressBeep() {
  startBeep(2800, 50);
}
