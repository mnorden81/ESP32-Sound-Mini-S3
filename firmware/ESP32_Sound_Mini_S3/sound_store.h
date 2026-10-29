#ifndef SOUND_STORE_H
#define SOUND_STORE_H

/*
 * sound_store.h  -  Soundmodul Mini
 *
 * Die Sound-Dateien liegen im Flash (LittleFS, Partition "spiffs" aus
 * default.csv), Dateinamen /loop.wav, /sound1.wav ... Abspielen, Upload,
 * Loeschen und Dateistatus laufen ueber soundFS(). Das Mini hat keine
 * SD-Karte (bis v0.03 gab es die Auswahl Auto/Flash/SD).
 * Konfigurations-Backups liegen seit v0.04 ebenfalls im Flash (RC_Common
 * config_backup, Ordner /backup in derselben LittleFS-Partition).
 */

#include <Arduino.h>
#include <FS.h>

// Bindet den Flash-Speicher ein. Einmal in setup() aufrufen, vor dem
// ersten Laden einer WAV-Datei.
void soundStoreBegin();

fs::FS&     soundFS();            // Speicher fuer die Sound-Dateien
bool        soundStoreReady();    // false = Flash-Dateisystem nicht verfuegbar
const char* soundStoreName();     // "Flash"
uint64_t    soundStoreTotalBytes();
uint64_t    soundStoreUsedBytes();     // v0.08: aus den Dateien berechnet (4-KB-Bloecke)
uint64_t    soundStoreLibUsedBytes();  // LittleFS.usedBytes(), nur fuers Serial-Log

#endif
