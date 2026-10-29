/*
 * sound_store.cpp  -  Soundmodul Mini, siehe sound_store.h
 */

#include "sound_store.h"
#include <LittleFS.h>
#include <dirent.h>
#include <sys/stat.h>

static bool flashOk = false;

// LittleFS darf mehrere Dateien gleichzeitig offen halten: 3 Motor-Dateien
// + 4 Sounds + Upload passen ohne Freigabe-Trick hinein.
static constexpr uint8_t FLASH_MAX_FILES = 10;

void soundStoreBegin() {
    // formatOnFail=true: eine leere oder beschaedigte Partition (z. B. nach
    // dem ersten Flashen ohne uploadfs) wird einmal formatiert statt
    // dauerhaft "kein Speicher" zu melden.
    flashOk = LittleFS.begin(true, "/littlefs", FLASH_MAX_FILES);
    Serial.printf("Sound-Speicher: Flash %s\n", flashOk ? "ok" : "FEHLER");
    if (flashOk)
        Serial.printf("Sound-Speicher: belegt %llu B (Bibliothek: %llu B) von %llu B\n",
                      (unsigned long long)soundStoreUsedBytes(),
                      (unsigned long long)soundStoreLibUsedBytes(),
                      (unsigned long long)soundStoreTotalBytes());
}

fs::FS&     soundFS()         { return LittleFS; }
bool        soundStoreReady() { return flashOk; }
const char* soundStoreName()  { return "Flash"; }

uint64_t soundStoreTotalBytes() { return flashOk ? (uint64_t)LittleFS.totalBytes() : 0; }
uint64_t soundStoreLibUsedBytes() { return flashOk ? (uint64_t)LittleFS.usedBytes() : 0; }

// v0.08: Belegung selbst aus den Dateien berechnet. LittleFS.usedBytes()
// (lfs_fs_size) ist laut littlefs nur eine obere Schaetzung, esp_littlefs
// begrenzt sie auf die Partitionsgroesse, und ein interner Fehler ergibt
// dort ebenfalls "voll" - im Web stand so dauerhaft 100 %, obwohl noch
// Platz frei war.
// Zaehlt je Datei die belegten 4-KB-Bloecke (inkl. CTZ-Zeiger, 8 B je
// Block reichen sicher), je Ordner ein Metadaten-Paar (2 Bloecke) und das
// Wurzel-Paar mit dem Superblock. Ueber opendir()/stat() statt
// File::openNextFile(), damit dabei keine Datei geoeffnet wird (die
// Datei-Deskriptoren sind mit Sounds und Upload knapp, siehe oben). Nimmt
// keinen Mutex - darf deshalb auch innerhalb eines sdMutexTake() laufen.
static constexpr uint32_t FS_BLOCK = 4096;

static uint32_t blocksInDir(const String& path, uint8_t depth) {
    DIR* d = opendir(path.c_str());
    if (!d) return 0;
    uint32_t blocks = 2;                         // Metadaten-Paar dieses Ordners
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        String child = path + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (depth < 3) blocks += blocksInDir(child, depth + 1);
            continue;
        }
        struct stat st;
        if (stat(child.c_str(), &st) != 0 || st.st_size <= 0) continue;
        uint32_t size = (uint32_t)st.st_size;
        uint32_t n = (size + FS_BLOCK - 1) / FS_BLOCK;
        blocks += (size + n * 8 + FS_BLOCK - 1) / FS_BLOCK;
    }
    closedir(d);
    return blocks;
}

uint64_t soundStoreUsedBytes() {
    if (!flashOk) return 0;
    uint64_t used  = (uint64_t)blocksInDir("/littlefs", 0) * FS_BLOCK;
    uint64_t total = soundStoreTotalBytes();
    return used > total ? total : used;
}
