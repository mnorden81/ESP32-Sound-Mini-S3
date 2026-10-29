#ifndef MEM_INFO_H
#define MEM_INFO_H

/*
 * mem_info.h  -  Soundmodul Mini, Speicherbelegung (seit v0.06)
 *
 * Liest, wie voll Flash und RAM sind, fuer die Weboberflaeche (Tab Debug,
 * Karte "Speicherbelegung" ueber /api/debug) und das Lua-Menue (Ordner
 * "Speicher"):
 *   - Firmware: Groesse der laufenden Firmware / Groesse der App-Partition
 *     (default.csv: 2 x 1,25 MB fuer OTA)
 *   - Sounds:   belegter / gesamter Platz im Flash-Dateisystem (LittleFS,
 *     Sounds + Einstellungs-Backups), seit v0.08 aus den Dateien berechnet
 *     (siehe soundStoreUsedBytes())
 *   - RAM:      freier interner Heap, kleinster Wert seit dem Start
 *   - PSRAM:    freier PSRAM (S3FH4R2: 2 MB), 0 wenn keiner erkannt
 */

#include <Arduino.h>

struct MemInfo {
    uint32_t appUsed,  appSize;      // Firmware in der App-Partition
    uint32_t fsUsed,   fsSize;       // Flash-Dateisystem (Sounds, Backups)
    uint32_t heapFree, heapMin, heapSize;
    uint32_t psramFree, psramSize;
};

MemInfo memInfoRead();

// Belegung in Prozent (0-100), 0 wenn size 0 ist
uint8_t memPercent(uint32_t used, uint32_t size);

#endif
