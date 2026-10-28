/*
   Soundmodul Mini (ESP32-S3) - Soundmodul-Firmware fuer RC-Modelle
   PiperPilot

   Abgeleitet von ESP32-RC-Sound v7.19 (github.com/mnorden81/
   mnorden81-ESP32-Sound-bidirectional_code), reduziert auf:
     - nur CRSF als RC-Bus (Port A), busbasiert: kein SBUS, keine PWM-/
       GPIO-Eingaenge, keine Hardware-Varianten V1/V2/V3
     - Motorsound (start/loop/shut) + 4 Zusatzsounds
   Unveraendert uebernommen: Motorlogik (Gasrampe, Standgas-Verzoegerung,
   Tonhoehe folgt Gas), Port B (zweiter UART, Rolle waehlbar), Port C
   (GPS), Weboberflaeche mit Sound-Upload und OTA, CRSF-/Lua-Menue,
   WLAN-Failsafe. Seit v0.03 kommen CRSF, UART-Rollen, S.Port, GPS,
   Lua-Menue-Tabelle, gemeinsame Einstellungen, WLAN-Failsafe, Status-LED
   und OTA aus RC_Common (gleiche Basis wie Multiswitch und Soundmodul V7).
   Seit v0.04 liegen Einstellungs-Backups im Flash (RC_Common 1.2.0,
   config_backup), das Mini hat keine SD-Karte.

   ── Quellen (Motor EIN und Sound 1-4) ─────────────────────────────────
   BUS Kanal Low/High (1-16), Einkanal (1-8, Schalter an der eigenen
   WM-Adresse), MKan (1-16, zwei frei waehlbare CRSF-Gruppenadressen),
   Dauerbetrieb, Deaktiviert.
   Rohwerte wie in V7 (0-15, 20-35, 70-77, 80-95, 200, 999), damit
   Konfigurationsdateien austauschbar bleiben.

   ── Pins ──────────────────────────────────────────────────────────────
   Siehe pins.h (vorlaeufig, wird in Schritt 5 am Board geprueft).

   ── Das Port-Modell ───────────────────────────────────────────────────
   Port A = CRSF (Serial2), fest.
   Port B = Hardware-UART1 (Serial1), Port C = Hardware-UART0 (Serial0).
            Rolle je Port waehlbar in Web UND CRSF/Lua-Menue (RC_Common,
            uart_roles.h): Aus, CRSF (Modulkette: der CRSF-Router reicht
            Frames zwischen Port A und Port B/C weiter), S.Port-Master
            (LiPo-Telemetrie), GPS (CRSF-GPS-Telemetrie) oder Raw-UART, je
            mit eigener Baudrate. S.Port, GPS und Raw nur auf einem Port;
            doppelt gewaehlt laeuft die Rolle auf Port B, Port C bleibt aus.
            CRSF darf auf beiden Ports laufen.

   ── WLAN & Weboberflaeche ─────────────────────────────────────────────
   Der Access Point laesst sich einschalten ueber:
     - "WLAN dauerhaft an" (Default AN)
     - BOOT-Taste: nach dem Einschalten innerhalb von 3 s kurz druecken
       (nur wenn "WLAN dauerhaft an" AUS ist, LED blinkt blau)
     - Schalter im Web-Tab "WiFi" bzw. Lua-Feld "WLAN jetzt" (sofort,
       nicht persistiert)
     - Auto-Failsafe: schaltet ein, wenn laenger als die eingestellte Zeit
       kein gueltiges CRSF-Signal anliegt; schaltet NIE automatisch aus
       (sonst bricht eine laufende Konfiguration/OTA ab).

   ── CRSF-/Lua-Parametermenue ──────────────────────────────────────────
   57 Feld-IDs, lueckenlos 1..57 (Tabelle in buildLuaMenu()), Liste direkt beim
   Parameter-System weiter unten. CRSF-Busadresse 0xC0+WM-Adresse und
   Telemetrie-Zeitslot nach dem Wilhelm-Meier-Schema.

   ── Module ────────────────────────────────────────────────────────────
   config.cpp/.h          Persistente Konfiguration (NVS)
   WebServerManager.cpp   Weboberflaeche, REST-API, OTA-Update
   XT_I2S_Audio.cpp/.h    WAV-Wiedergabe ueber I2S
   sound_store.cpp/.h     Sound-Speicher Flash (LittleFS)
   mem_info.cpp/.h        Speicherbelegung Flash/RAM fuer Web und Lua
   RC_Common/             Gemeinsame Basis aller ESP32-Module (identische
                          Kopie wie im Soundmodul V7 und im Multiswitch):
                          CRSF mit Router, UART-Rollen, S.Port, GPS,
                          Lua-Menue, Einstellungen, WLAN-Failsafe, LED, OTA
   pins.h                 Pinbelegung
*/

#include <Arduino.h>
#include "XT_I2S_Audio.h"
#include <WiFi.h>
#include "src/RC_Common/rc_common.h"
#include "src/RC_Common/CRSF_ESP32/crsf_esp32.h"
#include "config.h"
#include "pins.h"
#include "WebServerManager.h"
#include "src/RC_Common/uart_ports.h"     // Port B/C: Rollen starten, Telemetrie
#include "src/RC_Common/sport_lipo.h"
#include "src/RC_Common/lua_menu.h"       // CRSF-/Lua-Menue als Tabelle
#include "src/RC_Common/wifi_failsafe.h"
#include "src/RC_Common/status_led.h"
#include "sound_store.h"
#include "mem_info.h"

uint16_t Version = 7;  // Firmware-Version fuer Web-/Lua-Anzeige: major*100+minor (7 -> "0.07")
// v0.07: Web-Tab Debug: WAV-Liste zeigt, ob die Datei im Flash liegt (mit
//        Groesse) statt "gerade geoeffnet"; RAM-Zeile eindeutig (belegt/
//        frei/min frei); neue Karte "Dateien im Flash" mit allen Dateien.
// v0.06: Speicherbelegung (Firmware, Sounds, RAM, PSRAM) im Web-Tab Debug
//        und im Lua-Ordner "Speicher" (Felder 53-57).
// v0.05: WLAN-Taste statt WLAN-Bruecke: BOOT-Taste (GPIO0) nach dem
//        Einschalten innerhalb von 3 s druecken = WLAN an. GPIO9 ist frei.
// v0.04: Einstellungs-Backup im Flash (RC_Common 1.2.0) statt auf SD-Karte,
//        automatisch vor Import und Firmware-Update. Keine SD-Karte mehr:
//        Sounds nur im Flash, Lua-Feld 50 (Sound-Speicher) nur noch Anzeige.
// v0.02: Port C ist ein Hardware-UART mit waehlbarer Rolle wie Port B
//        (Aus/CRSF/S.Port/Raw/GPS), GPS ohne SoftwareSerial.
char versionString[6];
// Port C (GPS) kann beim Mini nicht mit Port A kollidieren (CRSF-only). Die
// Variable bleibt, weil die Weboberflaeche sie im Debug-JSON meldet.
bool gpsPinConflict = false;

// ── Zustand ───────────────────────────────────────────────────────────
bool Sound_on[RCSOUND_NUM_SLOTS]     = {};
bool Sound_play[RCSOUND_NUM_SLOTS]   = {};
bool Sound_on_web[RCSOUND_NUM_SLOTS] = {};

bool Sound_on_Motor       = false;
bool Sound_on_Motor_state = false;
bool engine_break         = false;
unsigned long shutdown_timer = 0;

enum Engine_State : uint8_t { OFF, STARTING, RUNNING, STOPPING };
volatile uint8_t engine_State = OFF;

// Boot-Snapshot: Rolle/Baudrate von Port B und Port C werden
// einmalig in setup() aus config uebernommen. Aendern ueber Web/Lua schreibt
// sofort in config, wirkt aber erst nach einem Neustart - die UARTs sind
// dann schon initialisiert und werden im Betrieb nicht umgeschaltet.
uint8_t  PortB_Role_boot        = UROLE_OFF;
uint32_t PortB_Baud_boot        = 420000;
uint8_t  PortC_Role_boot        = UROLE_OFF;
uint32_t PortC_Baud_boot        = 9600;
bool     PortC_Duplicate_boot   = false;   // Port C hatte dieselbe Rolle wie Port B -> aus
// Port A (CRSF) belegt Serial2, Port B Serial1, Port C Serial0 (UART0,
// frei, weil die Konsole ueber USB laeuft).
static HardwareSerial* const PORTA_SERIAL = &Serial2;
static HardwareSerial* const PORTB_SERIAL = &Serial1;
static HardwareSerial* const PORTC_SERIAL = &Serial0;
// Port B und C fuer RC_Common (uart_ports.h)
static UartPort uartPorts[2] = {
    {"Port B", PORTB_SERIAL, PIN_PORTB_RX, PIN_PORTB_TX, UROLE_OFF, 0, UROLE_OFF, false, false},
    {"Port C", PORTC_SERIAL, PIN_PORTC_RX, PIN_PORTC_TX, UROLE_OFF, 0, UROLE_OFF, false, false},
};
// RGB-LED des S3-Zero: Status von Port A (RC_Common, status_led.h)
static StatusLed statusLed;

uint16_t einkanal_Data = 0;   // 1 Bit/Schalter - Quelltyp "Einkanal" 70-77

// Je ein 8-Bit-Wert (1 Bit/Schalter) aus den 2 CRSF-Gruppenadressen sowie
// deren Kombination zu einem 16-Bit-Flachwert - Quelltyp "MKan" 80-95.
uint8_t  einkanal_Gruppe[RCSOUND_NUM_GROUPS] = {};
uint32_t einkanal_Flat16          = 0;

// ── Throttle / Rampe ─────────────────────────────────────────────────
unsigned long previousTime_ramp = 0;
float last_throttle = 0.0f;
float throttle      = 0.0f;

char ssid[32];
char password[64];
unsigned long currentTime = 0;

bool BUS_OK = false;

// ── CRSF ──────────────────────────────────────────────────────────────
CRSF crsf;       // Port A - der Haupt-RC-Bus; mit Port B/C = CRSF zugleich Router der Modulkette
#define MWset       0x01
#define MWprop      0x02
#define MWset4      0x07
#define MWset4m     0x09
#define Multiswitch 0xA1

static constexpr unsigned long CRSF_TIMEOUT_MS = 2000;
static unsigned long lastCrsfPacket = 0;

static void checkCrsfTimeout() {
    if (millis() - lastCrsfPacket > CRSF_TIMEOUT_MS) BUS_OK = false;
}

uint16_t channel_output[16] = {};

// ── I2S Audio ─────────────────────────────────────────────────────────
XT_I2S_Class I2SAudio(PIN_I2S_LRC, PIN_I2S_BCLK, PIN_I2S_DOUT, I2S_NUM_0);

XT_Wav_Class Sound_loop ("/loop.wav");
XT_Wav_Class Sound_shut ("/shut.wav");
XT_Wav_Class Sound_start("/start.wav");
XT_Wav_Class Sound1("/sound1.wav");   XT_Wav_Class Sound2("/sound2.wav");
XT_Wav_Class Sound3("/sound3.wav");   XT_Wav_Class Sound4("/sound4.wav");

XT_Wav_Class* Sounds[RCSOUND_NUM_SLOTS] = {
    nullptr, &Sound1, &Sound2, &Sound3, &Sound4
};

// ── Vorwaertsdeklarationen ────────────────────────────────────────────
void Config(); void LoadFiles();
void AllocateSoundBuffers(); // reserviert die Sample-Puffer erst in setup() NACH dem WLAN-AP-Start
void einkanalFunctionCRSF(); uint8_t compressSwitches(uint16_t state);
static void updateGroupFromAddress(uint8_t addr, uint8_t bits);
float ramp_throttle(float t); float floatMap(float x,float a,float b,float c,float d);
void handleSound(uint8_t idx, XT_Wav_Class* snd);

// ======== CRSF-Parametersystem ==========================================
//
//   0: Root  1: Info Version
//   2: Folder Motor {3..14}
//    3: Sel MotorMode  4: Sel EINModus  5: Sel QuelleEIN  6: U8 KanalEIN
//    7: Sel QuelleSpd  8: U8 KanalSpd   9: U8 Vol
//   10: U8 Drzmin  11: U8 Drzmax/2  12: U8 Standgas  13: U8 Rampe  14: U8 Totband
//
//   Sound s(1-4): fi=15+(s-1)*6  -> 15, 21, 27, 33 (endet bei 38)
//    fi+0:Folder fi+1:Sel Quelle fi+2:U8 Kanal fi+3:U8 Vol fi+4:Sel Mode fi+5:Sel Test
//
//  39: Folder Einstellungen {40..49}
//   40:U8 Modul Adresse
//   41:U8 Gruppenadr.1  42:U8 Gruppenadr.2  (255=aus)
//   43:Sel Port B Rolle  44:Sel Port B Baudrate  45:Sel Port C Rolle
//      (Rollen aus RC_Common: Aus;CRSF;S.Port;GPS;Raw, bis v0.02 Aus;CRSF;S.Port;Raw;GPS)
//   52:Sel Port C Baudrate (v0.02, hinten angehaengt, im Ordner nach 45)
//   46:Sel WLAN jetzt (sofort, nicht persistiert)
//   47:Sel Auto-WLAN bei Signalverlust  48:U8 Auto-WLAN Timeout (s)
//   49:Sel WLAN dauerhaft an
//   50:Info Sound-Speicher "Flash" (bis v0.03 Auswahl Auto/Flash/SD)
//  51: Info Geraetename
//  53: Folder Speicher {54..57} (v0.06, nur Anzeige)
//   54:Info Firmware  55:Info Sounds  56:Info RAM frei  57:Info PSRAM frei
static constexpr uint8_t SOUND_FIRST_FI   = 15;
static constexpr uint8_t FIELDS_PER_SOUND = 6;
static constexpr uint8_t SOUND_LAST_FI    = SOUND_FIRST_FI + RCSOUND_NUM_SOUNDS*FIELDS_PER_SOUND - 1;
static constexpr uint8_t SETTINGS_FI      = SOUND_LAST_FI + 1;
static constexpr uint8_t DEVNAME_FI       = SETTINGS_FI + 12;
static constexpr uint8_t PORTC_BAUD_FI    = DEVNAME_FI + 1;
static constexpr uint8_t MEMORY_FI        = PORTC_BAUD_FI + 1;
static constexpr uint8_t CRSF_PARAM_COUNT = MEMORY_FI + 4;
// Die festen IDs 40-50 in buildLuaMenu() setzen diese Lage voraus.
static_assert(SOUND_LAST_FI == 38 && SETTINGS_FI == 39 && DEVNAME_FI == 51 && CRSF_PARAM_COUNT == 57,
              "CRSF-Feld-IDs passen nicht mehr zu buildLuaMenu()");

// ── CRSF-Geraeteadresse + Ping-Slot aus der WM-Adresse (Wilhelm-Meier-Schema) ──
// Adresse = 0xC0 + WM-Adresse, Slot = WM-Adresse * 2. Die Ping-Antwort wird
// erst im eigenen Zeit-Slot gesendet, damit sich mehrere Module nicht
// ueberlappen.
#define CRSF_SLOT_MS 5
static inline uint8_t  crsfAddrFromWM()  { return 0xC0 + (uint8_t)constrain(config.modul_adress, 0, 15); }
static inline uint16_t crsfSlotDelayMs() { return (uint16_t)((uint8_t)constrain(config.modul_adress, 0, 15) * 2) * CRSF_SLOT_MS; }

// ── Typ-Mapping Quelle Motor/Sound EIN ───────────────────────────────
// Typ: 0=BUS-L(0-15) 1=BUS-H(20-35) 5=Einkanal(70-77) 6=MKan(80-95)
//      7=Dauerbetrieb(200) 8=Deaktiviert(999)
// (Typen 2-4 waren in V7 PWM/Pin und entfallen.)
static uint8_t einTyp(int c) {
    if(c==999)return 8; if(c==200)return 7;
    if(c>=80&&c<=95)return 6; if(c>=70&&c<=77)return 5;
    if(c>=20&&c<=35)return 1;
    if(c>=0&&c<=15)return 0; return 8;
}

// Prueft, ob ein "Quelle"-Rohwert gueltig ist. Bewusst NICHT static:
// config.cpp (loadConfig()) und WebServerManager.cpp (handleApiSound(),
// auch beim Konfigurations-Import) nutzen dieselbe Definition. Ein
// ungueltiger Rohwert (z.B. PWM/Pin aus einer V7-Konfiguration) wird auf
// 999 (Deaktiviert) zurueckgesetzt.
bool isValidEinSource(int c) {
    return c==999 || c==200 ||
           (c>=80&&c<=95) || (c>=70&&c<=77) || (c>=20&&c<=35) ||
           (c>=0&&c<=15);
}
static uint8_t einNr(int c) {
    if(c>=80&&c<=95)return c-80+1; if(c>=70&&c<=77)return c-70+1;
    if(c>=20&&c<=35)return c-20+1;
    if(c>=0&&c<=15)return c+1; return 1;
}
static int setEin(uint8_t t, uint8_t nr) {
    uint8_t i=(nr>0)?nr-1:0;
    if(t==0)return min(i,(uint8_t)15);
    if(t==1)return 20+min(i,(uint8_t)15);
    if(t==5)return 70+min(i,(uint8_t)7);
    if(t==6)return 80+min(i,(uint8_t)15);
    if(t==7)return 200; return 999;
}
static uint8_t einMaxNr(uint8_t t) {
    if(t==0||t==1)return 16;
    if(t==5)return 8; if(t==6)return 16; return 1;
}

// ── Quelle Speed: 0-15 = CRSF-Kanal, 999 = Deaktiviert ────────────────
static bool spdActive(int c) { return c>=0&&c<=15; }

static bool testSoundActive[RCSOUND_NUM_SLOTS]={};

// ── CRSF Quelle EIN: Auswahl L/H/EK/MKan/Dauer/Aus ────────────────────
static const char* EIN_SEL_STR = "L;H;EK;MKan;Dauer;Aus";
static constexpr uint8_t EIN_SEL_MAX = 5;
static uint8_t einSelVal(int cfg) {
    uint8_t t = einTyp(cfg);
    // Auswahl-Index: 0=L,1=H,2=EK,3=MKan,4=Dauer,5=Aus
    if (t==5) return 2; if (t==6) return 3; if (t==7) return 4; if (t>=8) return 5;
    return t; // 0,1
}
static int einSelWrite(uint8_t val, int oldCfg) {
    static const uint8_t selToTyp[] = {0,1,5,6,7,8};
    uint8_t t = (val < 6) ? selToTyp[val] : 8;
    return setEin(t, einNr(oldCfg));
}

// ======== CRSF-/Lua-Parametermenue als Tabelle (RC_Common LuaMenu) =====
// Seit v0.03 steht jedes Feld genau einmal in buildLuaMenu() (Feld-ID,
// Ordner, Typ, Name, Grenzen, Lese-/Schreibfunktion); Senden und Schreiben
// laufen generisch ueber RC_Common/lua_menu.h. Feld-IDs unveraendert (Liste
// oben). Die Kinderlisten der Ordner ergeben sich aus der Reihenfolge.
static LuaItem luaItems[CRSF_PARAM_COUNT + 1];   // + Root
static LuaMenu luaMenu(luaItems, sizeof(luaItems) / sizeof(luaItems[0]));

static void luaShowVersion(uint8_t, LuaView& v, char* buf) {
    snprintf(buf, LUA_VIEW_BUF, "%s Mini S3", versionString);
    v.text = buf;
}
static void luaShowDeviceName(uint8_t, LuaView& v, char* buf) {
    snprintf(buf, LUA_VIEW_BUF, "%s (Web aendern)", config.Device_Name);
    v.text = buf;
}
// Ordner "Speicher" (v0.06): arg 0 = Firmware, 1 = Sounds, 2 = RAM, 3 = PSRAM.
// Die Werte werden bei jedem Abruf des Feldes neu gelesen.
static void luaShowMemory(uint8_t a, LuaView& v, char* buf) {
    MemInfo m = memInfoRead();
    const float MB = 1048576.0f;
    if (a == 0)
        snprintf(buf, LUA_VIEW_BUF, "%.2f von %.2f MB (%u%%)", m.appUsed / MB, m.appSize / MB,
                 (unsigned)memPercent(m.appUsed, m.appSize));
    else if (a == 1 && m.fsSize == 0)
        snprintf(buf, LUA_VIEW_BUF, "nicht verfuegbar");
    else if (a == 1)
        snprintf(buf, LUA_VIEW_BUF, "%.2f von %.2f MB (%u%%)", m.fsUsed / MB, m.fsSize / MB,
                 (unsigned)memPercent(m.fsUsed, m.fsSize));
    else if (a == 2)
        snprintf(buf, LUA_VIEW_BUF, "%u KB (min %u KB)", (unsigned)(m.heapFree / 1024),
                 (unsigned)(m.heapMin / 1024));
    else if (m.psramSize == 0)
        snprintf(buf, LUA_VIEW_BUF, "kein PSRAM");
    else
        snprintf(buf, LUA_VIEW_BUF, "%.2f von %.2f MB", m.psramFree / MB, m.psramSize / MB);
    v.text = buf;
}

static void luaShowSoundFolder(uint8_t s, LuaView& v, char* buf) {
    snprintf(buf, LUA_VIEW_BUF, "Sound %d", s);
    v.name = buf;
}

// ── Motor ────────────────────────────────────────────────────────────────
static uint8_t luaGetThrottleMode(uint8_t)          { return (uint8_t)constrain(config.throttle_mode, 0, 1); }
static void    luaSetThrottleMode(uint8_t, uint8_t v){ config.throttle_mode = v; markDirty(); }
static uint8_t luaGetToggle(uint8_t)                { return (uint8_t)constrain(config.engine_on_toggle, 0, 1); }
static void    luaSetToggle(uint8_t, uint8_t v)     { config.engine_on_toggle = v; markDirty(); }
static uint8_t luaGetMinSpeed(uint8_t)              { return (uint8_t)constrain(config.Min_Speed_Sound_0, 0, 200); }
static void    luaSetMinSpeed(uint8_t, uint8_t v)   { config.Min_Speed_Sound_0 = v; markDirty(); }
static uint8_t luaGetMaxSpeed(uint8_t)              { return (uint8_t)constrain(config.Max_Speed_Sound_0 / 2, 50, 255); }
static void    luaSetMaxSpeed(uint8_t, uint8_t v)   { config.Max_Speed_Sound_0 = constrain((int)v * 2, 100, 510); markDirty(); }
static uint8_t luaGetShutdown(uint8_t)              { return (uint8_t)constrain(config.shutdowndelay, 0, 60); }
static void    luaSetShutdown(uint8_t, uint8_t v)   { config.shutdowndelay = v; markDirty(); }
static uint8_t luaGetRamp(uint8_t)                  { return (uint8_t)constrain(config.throttle_ramp, 0, 50); }
static void    luaSetRamp(uint8_t, uint8_t v)       { config.throttle_ramp = v; markDirty(); }
static uint8_t luaGetDeadBand(uint8_t)              { return (uint8_t)constrain(config.throttle_dead_band, 0, 50); }
static void    luaSetDeadBand(uint8_t, uint8_t v)   { config.throttle_dead_band = v; markDirty(); }

// Quelle Speed: 0 = BUS Kanal, 1 = Deaktiviert
static uint8_t luaGetSpdSrc(uint8_t)                { return spdActive(config.Source_Speed_Sound_0) ? 0 : 1; }
static void    luaSetSpdSrc(uint8_t, uint8_t v) {
    if (v == 0) { if (!spdActive(config.Source_Speed_Sound_0)) config.Source_Speed_Sound_0 = 0; }
    else config.Source_Speed_Sound_0 = 999;
    markDirty();
}
static void luaShowSpdChannel(uint8_t, LuaView& v, char*) {
    if (!spdActive(config.Source_Speed_Sound_0)) { v.type = LUA_INFO; v.name = "Kanal Speed"; v.text = "Deaktiviert"; }
}
static uint8_t luaGetSpdChannel(uint8_t)            { return (uint8_t)(config.Source_Speed_Sound_0 + 1); }
static void    luaSetSpdChannel(uint8_t, uint8_t v) {
    if (spdActive(config.Source_Speed_Sound_0)) { config.Source_Speed_Sound_0 = constrain((int)v - 1, 0, 15); markDirty(); }
}

// ── Motor (arg 0) und Sound 1-4 (arg = Sound-Nr.) ────────────────────────
static uint8_t luaGetSource(uint8_t s)              { return einSelVal(config.Source_Start_Sound[s]); }
static void    luaSetSource(uint8_t s, uint8_t v)   { config.Source_Start_Sound[s] = einSelWrite(v, config.Source_Start_Sound[s]); markDirty(); }
static void luaShowChannel(uint8_t s, LuaView& v, char*) {
    uint8_t t = einTyp(config.Source_Start_Sound[s]);
    if (t == 7 || t == 8) {
        v.type = LUA_INFO;
        if (s == 0) v.name = "Kanal EIN";
        v.text = (t == 7) ? "Dauerbetrieb" : "Deaktiviert";
    } else {
        v.max = einMaxNr(t);
    }
}
static uint8_t luaGetChannel(uint8_t s)             { return einNr(config.Source_Start_Sound[s]); }
static void    luaSetChannel(uint8_t s, uint8_t v)  { config.Source_Start_Sound[s] = setEin(einTyp(config.Source_Start_Sound[s]), v); markDirty(); }
static uint8_t luaGetVolume(uint8_t s)              { return (uint8_t)constrain(config.Volumen_Sound[s], 0, 200); }
static void    luaSetVolume(uint8_t s, uint8_t v)   { config.Volumen_Sound[s] = v; markDirty(); }
static uint8_t luaGetPlayMode(uint8_t s)            { return (uint8_t)constrain(config.Mode_Sound[s], 0, 2); }
static void    luaSetPlayMode(uint8_t s, uint8_t v) { config.Mode_Sound[s] = v; markDirty(); }
// "Test Sound" wirkt sofort und wird nicht gespeichert
static uint8_t luaGetTest(uint8_t s)                { return testSoundActive[s] ? 1 : 0; }
static void    luaSetTest(uint8_t s, uint8_t v)     { if (v == 1) Sound_on_web[s] = true; testSoundActive[s] = (v == 1); }

// ── Einstellungen ────────────────────────────────────────────────────────
static uint8_t luaGetModAdr(uint8_t)                { return (uint8_t)constrain(config.modul_adress, 0, 20); }
static void    luaSetModAdr(uint8_t, uint8_t v)     { config.modul_adress = v; crsf.setDeviceAddress(crsfAddrFromWM()); markDirty(); }
static uint8_t luaGetGroupAdr(uint8_t a)            { return (uint8_t)constrain(config.EK_Gruppen_Adresse[a], 0, 255); }
static void    luaSetGroupAdr(uint8_t a, uint8_t v) { config.EK_Gruppen_Adresse[a] = v; markDirty(); }
// Port B (arg 0) / Port C (arg 1) wirken erst nach einem Neustart
static uint8_t&  luaPortRole(uint8_t a)             { return a ? config.PortC_Role : config.PortB_Role; }
static uint32_t& luaPortBaud(uint8_t a)             { return a ? config.PortC_Baud : config.PortB_Baud; }
static uint8_t luaGetPortRole(uint8_t a)            { return uartRoleClamp(luaPortRole(a)); }
static void    luaSetPortRole(uint8_t a, uint8_t v) {
    if (v != luaPortRole(a)) {
        // Baudrate auf den Standard der Rolle vorbelegen (Aus/Raw: unveraendert)
        uint32_t def = uartRoleDefaultBaud(v);
        if (def != 0 && v != UROLE_RAW) luaPortBaud(a) = def;
    }
    luaPortRole(a) = v; markDirty();
    Serial.printf("!!! Port %c Rolle -> %s (wirkt erst nach Neustart) – NEUSTART ERFORDERLICH !!!\n",
                  a ? 'C' : 'B', uartRoleName(v));
}
static uint8_t luaGetPortBaud(uint8_t a)            { return uartBaudToIndex(luaPortBaud(a)); }
static void    luaSetPortBaud(uint8_t a, uint8_t v) {
    luaPortBaud(a) = uartBaudFromIndex(v); markDirty();
    Serial.printf("!!! Port %c Baudrate -> %u (wirkt erst nach Neustart) – NEUSTART ERFORDERLICH !!!\n",
                  a ? 'C' : 'B', (unsigned)luaPortBaud(a));
}
// 46 "WLAN jetzt" wirkt sofort und wird nicht gespeichert: der naechste
// Start haengt wieder von "WLAN dauerhaft an" bzw. der BOOT-Taste ab.
static uint8_t luaGetWifiNow(uint8_t)               { return WebServerManager::isApActive() ? 1 : 0; }
static void    luaSetWifiNow(uint8_t, uint8_t v)    { if (v) WebServerManager::enableAP(); else WebServerManager::disableAP(); }
static uint8_t luaGetWifiAuto(uint8_t)              { return config.WifiAutoEnable ? 1 : 0; }
static void    luaSetWifiAuto(uint8_t, uint8_t v)   { config.WifiAutoEnable = v; markDirty(); }
static uint8_t luaGetWifiTimeout(uint8_t)           { return (uint8_t)constrain(config.WifiAutoTimeoutSec, 5, 240); }
static void    luaSetWifiTimeout(uint8_t, uint8_t v){ config.WifiAutoTimeoutSec = v; markDirty(); }
// 49 wirkt erst ab dem naechsten Start (kein enableAP() aus dem Parameter-
// Handler heraus - WLAN-Start zur Laufzeit ist die fehleranfaelligste
// Operation, siehe V7)
static uint8_t luaGetWifiAlways(uint8_t)            { return config.WifiAlwaysOn ? 1 : 0; }
static void    luaSetWifiAlways(uint8_t, uint8_t v) { config.WifiAlwaysOn = v; markDirty(); }

static void buildLuaMenu() {
    static const char* AUS_EIN = "Aus;Ein";
    LuaMenu& m = luaMenu;
    m.clear();
    m.folder(0, 0, "");
    m.info(1, 0, "Version", "")->view = luaShowVersion;

    m.folder(2, 0, "Motor");
    m.select(3, 2, "Motor Mode", "Eine Richtung;Zwei Richtungen", luaGetThrottleMode, luaSetThrottleMode);
    m.select(4, 2, "Motor EIN Modus", "Normal;Toggle", luaGetToggle, luaSetToggle);
    m.select(5, 2, "Quelle Motor", EIN_SEL_STR, luaGetSource, luaSetSource, 0);
    m.u8(6, 2, "Kanal Nr EIN", 1, 16, "", luaGetChannel, luaSetChannel, 0)->view = luaShowChannel;
    m.select(7, 2, "Quelle Speed", "BUS Kanal;Deaktiviert", luaGetSpdSrc, luaSetSpdSrc);
    m.u8(8, 2, "Kanal Nr Speed", 1, 16, "", luaGetSpdChannel, luaSetSpdChannel)->view = luaShowSpdChannel;
    m.u8(9, 2, "Volumen Motor", 0, 200, "", luaGetVolume, luaSetVolume, 0);
    m.u8(10, 2, "Drehzahl min %", 0, 200, "%", luaGetMinSpeed, luaSetMinSpeed);
    m.u8(11, 2, "Drehzahl max /2", 50, 255, "%", luaGetMaxSpeed, luaSetMaxSpeed);
    m.u8(12, 2, "Motor aus Standgas", 0, 60, "s", luaGetShutdown, luaSetShutdown);
    m.u8(13, 2, "Motor Rampe %/s", 0, 50, "", luaGetRamp, luaSetRamp);
    m.u8(14, 2, "Standgas Totband", 0, 50, "%", luaGetDeadBand, luaSetDeadBand);

    for (uint8_t s = 1; s <= RCSOUND_NUM_SOUNDS; s++) {
        uint8_t fi = SOUND_FIRST_FI + (s - 1) * FIELDS_PER_SOUND;
        LuaItem* f = m.folder(fi, 0, "Sound");
        f->arg = s; f->view = luaShowSoundFolder;
        m.select(fi + 1, fi, "Quelle", EIN_SEL_STR, luaGetSource, luaSetSource, s);
        m.u8(fi + 2, fi, "Kanal Nr", 1, 16, "", luaGetChannel, luaSetChannel, s)->view = luaShowChannel;
        m.u8(fi + 3, fi, "Volumen", 0, 200, "", luaGetVolume, luaSetVolume, s);
        m.select(fi + 4, fi, "Wiedergabe Mode", "Normal;Loop;Tippbetrieb", luaGetPlayMode, luaSetPlayMode, s);
        m.select(fi + 5, fi, "Test Sound", AUS_EIN, luaGetTest, luaSetTest, s);
    }

    m.folder(SETTINGS_FI, 0, "Einstellungen");
    m.u8(40, SETTINGS_FI, "Modul Adresse", 0, 20, "", luaGetModAdr, luaSetModAdr);
    m.u8(41, SETTINGS_FI, "Gruppenadr. 1 (255=aus)", 0, 255, "", luaGetGroupAdr, luaSetGroupAdr, 0);
    m.u8(42, SETTINGS_FI, "Gruppenadr. 2 (255=aus)", 0, 255, "", luaGetGroupAdr, luaSetGroupAdr, 1);
    m.select(43, SETTINGS_FI, "Port B Rolle", uartRoleOptions(), luaGetPortRole, luaSetPortRole, 0);
    m.select(44, SETTINGS_FI, "Port B Baudrate", uartBaudOptions(), luaGetPortBaud, luaSetPortBaud, 0);
    m.select(45, SETTINGS_FI, "Port C Rolle", uartRoleOptions(), luaGetPortRole, luaSetPortRole, 1);
    m.select(PORTC_BAUD_FI, SETTINGS_FI, "Port C Baudrate", uartBaudOptions(), luaGetPortBaud, luaSetPortBaud, 1);
    m.select(46, SETTINGS_FI, "WLAN jetzt", AUS_EIN, luaGetWifiNow, luaSetWifiNow);
    m.select(47, SETTINGS_FI, "Auto-WLAN bei Signalverlust", AUS_EIN, luaGetWifiAuto, luaSetWifiAuto);
    m.u8(48, SETTINGS_FI, "Auto-WLAN Timeout (s)", 5, 240, "", luaGetWifiTimeout, luaSetWifiTimeout);
    m.select(49, SETTINGS_FI, "WLAN dauerhaft an", AUS_EIN, luaGetWifiAlways, luaSetWifiAlways);
    // v0.04: nur noch Anzeige (keine SD-Karte), Feld bleibt, damit die
    // Feldnummern ohne Luecke bleiben
    m.info(50, SETTINGS_FI, "Sound-Speicher", "Flash");
    m.info(DEVNAME_FI, 0, "Geraetename", "")->view = luaShowDeviceName;

    static const char* MEM_NAMES[4] = {"Firmware", "Sounds", "RAM frei", "PSRAM frei"};
    m.folder(MEMORY_FI, 0, "Speicher");
    for (uint8_t a = 0; a < 4; a++) {
        LuaItem* it = m.info(MEMORY_FI + 1 + a, MEMORY_FI, MEM_NAMES[a], "");
        it->arg = a; it->view = luaShowMemory;
    }
}

// WLAN-Taste: wartet nach dem Start WIFI_BUTTON_WINDOW_MS auf einen
// Druck der BOOT-Taste (GPIO0) und laesst dabei die RGB-LED blau blinken.
// Laeuft nur, wenn "WLAN dauerhaft an" AUS ist, und vor dem Reservieren
// der Sound-Puffer, damit der AP auf einem freien Heap startet. Wer BOOT
// schon beim Einschalten haelt, landet im Flash-Modus des ROM.
static bool waitForWifiButton() {
    Serial.printf("WLAN-Taste: %u s lang BOOT druecken = WLAN an\n",
                  (unsigned)(WIFI_BUTTON_WINDOW_MS / 1000));
    bool pressed = false;
    unsigned long start = millis();
    while (millis() - start < WIFI_BUTTON_WINDOW_MS) {
        if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
            delay(30);   // entprellen
            if (digitalRead(PIN_BOOT_BUTTON) == LOW) { pressed = true; break; }
        }
        neopixelWrite(PIN_STATUS_LED, 0, 0, ((millis() - start) / 250) % 2 ? 40 : 0);
        delay(10);
    }
    neopixelWrite(PIN_STATUS_LED, 0, 0, 0);
    return pressed;
}

// ======== Setup ======================================================
void setup() {
    Serial.begin(115200);
    loadConfig();

    strncpy(ssid,    config.WiFi_SSID,    sizeof(ssid)-1);    ssid[sizeof(ssid)-1]='\0';
    strncpy(password,config.WiFi_Password,sizeof(password)-1); password[sizeof(password)-1]='\0';

    // Port A: CRSF
    crsf.init_crsf(PORTA_SERIAL, PIN_CRSF_RX, PIN_CRSF_TX);
    crsf.setDeviceAddress(crsfAddrFromWM());   // eindeutige CRSF-Adresse aus WM-Adresse
    Serial.printf("CRSF (RX=%d, TX=%d)  Geraeteadresse 0x%02X  Slot %d\n",
                  PIN_CRSF_RX, PIN_CRSF_TX, crsfAddrFromWM(),
                  (uint8_t)constrain(config.modul_adress,0,15) * 2);

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    pinMode(PIN_MAX_GAIN, INPUT);   // GAIN offen = 9 dB

    soundStoreBegin();
    LoadFiles();

    // begin() registriert nur die HTTP-Routen und merkt sich SSID/Passwort.
    // Das Einschalten von AP + Webserver passiert getrennt ueber enableAP().
    WebServerManager::begin(ssid, password);

    if (config.WifiAlwaysOn) {
        Serial.println("WLAN dauerhaft aktiv (config.WifiAlwaysOn) - schalte AP ein...");
        WebServerManager::enableAP();
    } else if (waitForWifiButton()) {
        Serial.println("AP Modus (BOOT-Taste nach dem Start gedrueckt)...");
        WebServerManager::enableAP();
    }

    // SD-Lese-Task und Sample-Puffer erst NACH dem WLAN-AP-Start, damit
    // WiFi.softAP() auf einem moeglichst freien Heap laeuft (siehe
    // XT_I2S_Audio.h).
    I2SAudio.StartSdReaderTask();
    AllocateSoundBuffers();

    PortB_Role_boot         = config.PortB_Role;
    PortB_Baud_boot         = config.PortB_Baud;
    PortC_Role_boot         = config.PortC_Role;
    PortC_Baud_boot         = config.PortC_Baud;
    snprintf(versionString, sizeof(versionString), "%d.%02d", Version/100, Version%100);
    saveConfig();

    // Port B und Port C starten ueber RC_Common (uart_ports.h): CRSF =
    // Router der Modulkette, S.Port = LiPo-Sensoren, GPS, Raw = UART offen.
    // S.Port/GPS/Raw doppelt gewaehlt -> Port C bleibt aus.
    sportLipoUsePollIds(config.sport_poll_id);
    uartPorts[0].role = PortB_Role_boot; uartPorts[0].baud = PortB_Baud_boot;
    uartPorts[1].role = PortC_Role_boot; uartPorts[1].baud = PortC_Baud_boot;
    uartPortsBegin(uartPorts, 2, crsf, true);
    PortC_Duplicate_boot = uartPorts[1].duplicate;

    buildLuaMenu();
    statusLed.begin(PIN_STATUS_LED, true);
    Serial.printf("RC_Common %s, Lua-Menue %u Felder\n", RC_COMMON_VERSION, (unsigned)luaMenu.size());
    Serial.printf("Soundmodul Mini %s (ESP32-S3)\n", versionString);
}

// Auto-WLAN-Failsafe: schaltet das WLAN ein, wenn eine Zeit lang kein
// gueltiges CRSF-Signal anliegt (z.B. Modell auf der Werkbank, Sender aus).
// Schaltet NUR EIN, nie automatisch wieder aus - sonst wuerde eine laufende
// Konfiguration/ein OTA-Update abbrechen, sobald kurz wieder Signal kommt.
// Mit "WLAN dauerhaft an" ist der AP schon beim Booten an, dann greift diese
// Funktion nicht.
static WifiFailsafe wifiFailsafe;   // RC_Common, gleiche Logik in allen Modulen

void wifiFailsafeCheck() {
    if (wifiFailsafe.check(millis(), config.WifiAlwaysOn, config.WifiAutoEnable,
                           config.WifiAutoTimeoutSec, BUS_OK, WebServerManager::isApActive())) {
        Serial.printf("Auto-WLAN-Failsafe: %u s ohne gueltiges RC-Signal - schalte WLAN ein.\n",
                      (unsigned)config.WifiAutoTimeoutSec);
        WebServerManager::enableAP();
    }
}

// Status-LED (RC_Common): langsam = kein CRSF, schnell = Link schwach,
// aus = alles gut
static void updateLed() {
    unsigned long now = millis();
    const LinkStatistics& ls = crsf.getLinkStatistics();
    StatusLedInput in = {};
    in.crsf          = true;
    in.busOk         = BUS_OK;
    in.lastRcFrameMs = crsf.getLastRcFrameMs();
    in.linkStatsMs   = ls.lastUpdate;
    in.uplinkLQ      = ls.uplinkLQ;
    statusLed.update(statusLedMode(in, now), now);
}

// ======== Loop =======================================================
void loop() {
    currentTime = millis();
    if (WebServerManager::isApActive()) WebServerManager::Webpage();

    // Port B/C: S.Port und GPS einlesen (RC_Common, ohne aktive Rolle ohne Wirkung)
    uartPortsUpdate();

    // Port A: CRSF
    crsf.read_packets(0);
    crsf.route_packets();   // Modulkette Port A <-> Port B/C (nur bei Rolle CRSF aktiv)
    for (int i=0;i<16;i++) channel_output[i]=crsf.get_crfs_channels(i);
    // BUS_OK haengt daran, ob gerade ein gueltiger CRSF-Frame geparst wurde
    // (Zaehler validFrames), nicht an einem Kanalwert.
    static uint32_t lastValidFrameCount = 0;
    uint32_t vf = crsf.getValidFrames();
    if (vf != lastValidFrameCount) { lastValidFrameCount = vf; BUS_OK = true; lastCrsfPacket = millis(); }
    checkCrsfTimeout();

    // Alle 100 ms Zellspannungen (S.Port) und GPS als CRSF-Telemetrie, kein
    // Batterie-Frame (0x08) - sonst wuerde die RxBt des Empfaengers
    // ueberschrieben.
    uartPortsTelemetry(crsf);
    updateLed();
    if(crsf.getDeviceInfoReplyPending() && (millis() - crsf.getPingTime() >= crsfSlotDelayMs())){
        // Erst im eigenen Zeit-Slot auf den Broadcast-Ping antworten.
        crsf.setDeviceInfoReplyPending(false);
        char dn[24]; snprintf(dn,sizeof(dn),"%s@%d",config.Device_Name,config.modul_adress);
        // Firmware-Version als "Parameter Version Number", damit Konfiguratoren
        // eine geaenderte Parameterstruktur neu einlesen.
        crsf.send_device_info(dn,luaMenu.maxId(),(uint8_t)(Version & 0xFF));
    }
    if(crsf.getDeviceReadReplyPending()){
        uint8_t pi=crsf.getParamReadIndex();
        crsf.setDeviceReadReplyPending(false);
        luaMenu.send(crsf,pi);
    }
    if(crsf.getDeviceWriteReplyPending()){
        uint8_t wi=crsf.getParamWriteIndex(),wv=crsf.getParamWriteValue();
        crsf.setDeviceWriteReplyPending(false);
        luaMenu.write(wi,wv); luaMenu.send(crsf,wi);
    }
    if(crsf.getDeviceCommandReplyPending()){
        crsf.setDeviceCommandReplyPending(false);
        einkanalFunctionCRSF();
    }

    // NVS-Speicher: 500 ms nach der letzten Aenderung schreiben, und nicht
    // waehrend des Cooldowns nach einer SD-Schreibaktivitaet (Brownout-Schutz,
    // siehe WebServerManager::sdActivityCooldownActive()). Einzige Stelle, an
    // der im Betrieb gespeichert wird.
    if(configDirty&&(millis()-configDirtyMs)>=500UL&&!WebServerManager::sdActivityCooldownActive())
        saveConfigForce();

    wifiFailsafeCheck();

    Config();
    if(currentTime<5000) return;
    for(uint8_t i=1;i<=RCSOUND_NUM_SOUNDS;i++) handleSound(i,Sounds[i]);

    // Motor Toggle
    if(Sound_on[0]&&!Sound_on_Motor_state&&config.engine_on_toggle){Sound_on_Motor=!Sound_on_Motor;Sound_on_Motor_state=true;}
    else if(!Sound_on[0]&&Sound_on_Motor_state&&config.engine_on_toggle) Sound_on_Motor_state=false;
    else if(!config.engine_on_toggle) Sound_on_Motor=Sound_on[0];

    bool mw=Sound_on_Motor||Sound_on_web[0];
    if((mw&&!engine_break&&!Sound_play[0])||(throttle>0&&engine_break&&!Sound_play[0])||(throttle>0&&Sound_play[0])){Sound_play[0]=true;shutdown_timer=millis();}
    if(!mw){Sound_play[0]=false;engine_break=false;}
    if(config.shutdowndelay>0&&Sound_play[0]&&(millis()-shutdown_timer)>((unsigned long)config.shutdowndelay*1000UL)){Sound_play[0]=false;engine_break=true;}

    switch(engine_State){
        case OFF:    if(Sound_play[0]){Sound_start.Volume=config.Volumen_Sound[0];I2SAudio.Play(&Sound_start);engine_State=STARTING;} break;
        case STARTING: if(!Sound_start.Playing){throttle=0;last_throttle=0;Sound_loop.RepeatForever=true;Sound_loop.Volume=config.Volumen_Sound[0];I2SAudio.Play(&Sound_loop);engine_State=RUNNING;} break;
        case RUNNING:  Sound_loop.Volume=config.Volumen_Sound[0]; if(!Sound_play[0]){throttle=0;if(last_throttle==0){Sound_loop.RepeatForever=false;I2SAudio.Stop(&Sound_loop);engine_State=STOPPING;}} break;
        case STOPPING: if(!Sound_loop.Playing){Sound_shut.Volume=config.Volumen_Sound[0];I2SAudio.Play(&Sound_shut);engine_State=OFF;} break;
    }
    throttle=ramp_throttle(throttle);
    if(config.Min_Speed_Sound_0!=config.Max_Speed_Sound_0)
        Sound_loop.Speed=floatMap(throttle,0,100,config.Min_Speed_Sound_0,config.Max_Speed_Sound_0)/100.0f;
}

// ======== Sound ======================================================
void handleSound(uint8_t idx, XT_Wav_Class* snd) {
    if(!snd)return;
    if(Sound_on[idx]||Sound_on_web[idx]){
        snd->Volume=config.Volumen_Sound[idx];
        if(!Sound_play[idx]){Sound_play[idx]=true;snd->LoadWavFile();if(config.Mode_Sound[idx]==1)snd->RepeatForever=true;I2SAudio.Play(snd);Sound_on_web[idx]=false;}
    }else{
        snd->RepeatForever=false;
        // Lautstaerke auch waehrend des Nachspielens aktualisieren (z.B.
        // Web-Testbutton, der Sound_on_web sofort wieder zuruecksetzt).
        if(Sound_play[idx]) snd->Volume=config.Volumen_Sound[idx];
        if(config.Mode_Sound[idx]==2&&Sound_play[idx])I2SAudio.Stop(snd);
        // testSoundActive (Lua "Test Sound") zuruecksetzen, sobald die
        // Wiedergabe endet. Im Loop-Mode bleibt "Ein" bewusst stehen.
        if(!snd->Playing&&Sound_play[idx]){snd->UnLoadWavFile();Sound_play[idx]=false;testSoundActive[idx]=false;}
    }
}

// ======== Config: Quellen auswerten ==================================
void Config() {
    for (int x=0; x<RCSOUND_NUM_SLOTS; x++) {
        int src = config.Source_Start_Sound[x];
        if      (src>=0   && src<=15)  Sound_on[x] = BUS_OK && (channel_output[src] < 624);
        else if (src>=20  && src<=35)  Sound_on[x] = BUS_OK && (channel_output[src-20] > 1424);
        else if (src>=70  && src<=77)  Sound_on[x] = BUS_OK && bitRead(einkanal_Data, src-70);
        else if (src>=80  && src<=95) {
            // "MKan": 16 Einzelkanaele aus 2 CRSF-Gruppenadressen (je 8
            // Schalter). nr 0..15 -> Gruppe = nr/8, Bit = nr%8.
            uint8_t nr = (uint8_t)(src - 80);
            Sound_on[x] = BUS_OK && bitRead(einkanal_Flat16, nr);
        }
        else if (src==200)             Sound_on[x] = true;
        else                           Sound_on[x] = false;
    }

    // Motor Speed
    int spd = config.Source_Speed_Sound_0;
    if (spdActive(spd)) throttle = map(channel_output[spd], 82, 1900, 0, 100);
    else                throttle = 0;
    throttle = constrain(throttle, 0, 100);

    if (config.throttle_mode) {
        if      (throttle > 50+config.throttle_dead_band) throttle=map((long)throttle,50+config.throttle_dead_band,100,0,100);
        else if (throttle < 50-config.throttle_dead_band) throttle=map((long)throttle,50-config.throttle_dead_band,0,0,100);
        else throttle=0;
    } else {
        throttle=(throttle>config.throttle_dead_band)?map((long)throttle,config.throttle_dead_band,100,0,100):0;
    }
    throttle = constrain(throttle, 0, 100);
}

// ======== Hilfsfunktionen ============================================
float floatMap(float x,float a,float b,float c,float d){if(b==a)return c;return(x-a)*(d-c)/(b-a)+c;}
// max_files NICHT erhoehen (Erfahrung aus V7, v7.07..v7.10): ein groesserer
// Wert belegt beim Mounten dauerhaft einen grossen DRAM-Block und laesst den
// WLAN-Start an Fragmentierung scheitern. Der Deskriptor-Engpass wird beim
// Upload durch Freigeben ungenutzter Handles geloest (WebServerManager.cpp).
void LoadFiles(){Sound_loop.LoadWavFile();Sound_shut.LoadWavFile();Sound_start.LoadWavFile();}

// Holt die Sample-Puffer ALLER Sound-Objekte vom Heap - aufgerufen aus
// setup() NACH dem WLAN-AP-Start (siehe XT_I2S_Audio.h).
void AllocateSoundBuffers(){
    Sound_loop.AllocateBuffers();
    Sound_shut.AllocateBuffers();
    Sound_start.AllocateBuffers();
    for (int i = 1; i < RCSOUND_NUM_SLOTS; i++) {
        if (Sounds[i]) Sounds[i]->AllocateBuffers();
    }
}

float ramp_throttle(float tn){
    unsigned long rt=currentTime-previousTime_ramp; previousTime_ramp=currentTime;
    if(rt>1000)rt=1000; float step=(config.throttle_ramp/1000.0f)*(float)rt;
    if(tn>last_throttle){float s=last_throttle+step;tn=(s<tn)?s:tn;}
    else{float s=last_throttle-step;tn=(s>tn)?s:tn;}
    last_throttle=tn; return tn;
}

// ======== Einkanal CRSF ==============================================
// Jede eingehende Schalter-Adresse wird gegen die eigene Adresse
// (config.modul_adress, Quelltyp 70-77) UND gegen die 2 Gruppenadressen
// (config.EK_Gruppen_Adresse[], Quelltyp "MKan" 80-95) geprueft.
static void updateGroupFromAddress(uint8_t addr, uint8_t bits){
    for(uint8_t g=0; g<RCSOUND_NUM_GROUPS; g++){
        if(config.EK_Gruppen_Adresse[g]!=255 && addr==(uint8_t)config.EK_Gruppen_Adresse[g]){
            einkanal_Gruppe[g]=bits;
        }
    }
    // Bewusst per Hand (2 Gruppen -> 16 Bit); eine Aenderung von
    // RCSOUND_NUM_GROUPS muss diese Zeile mitziehen.
    einkanal_Flat16 = (uint32_t)einkanal_Gruppe[0]
                     | ((uint32_t)einkanal_Gruppe[1] << 8);
}
void einkanalFunctionCRSF(){
    uint8_t wmc=crsf.get_cmd_buffer(5),cmd=crsf.get_cmd_buffer(6);
    if(wmc!=Multiswitch)return;
    switch(cmd){
        // MWset4: 8 Schalter, 2 Bit/Schalter an einer Adresse - ausgewertet
        // wird 1 Bit/Schalter (compressSwitches()).
        case MWset4:{
            uint8_t a=crsf.get_cmd_buffer(7);
            uint16_t s=((uint16_t)crsf.get_cmd_buffer(8)<<8)|crsf.get_cmd_buffer(9);
            uint8_t bits=compressSwitches(s);
            if(a==(uint8_t)config.modul_adress) einkanal_Data=bits;
            updateGroupFromAddress(a,bits);
            break;
        }
        case MWset4m:{
            uint8_t c=min((uint8_t)crsf.get_cmd_buffer(7),(uint8_t)7);
            for(uint8_t i=0;i<c;i++){
                uint8_t a=crsf.get_cmd_buffer(8+(3*i));
                uint16_t s=((uint16_t)crsf.get_cmd_buffer(9+(3*i))<<8)|crsf.get_cmd_buffer(10+(3*i));
                uint8_t bits=compressSwitches(s);
                if(a==(uint8_t)config.modul_adress) einkanal_Data=bits;
                updateGroupFromAddress(a,bits);
            }
            break;
        }
        // MWset liefert bereits 1 Bit/Schalter direkt.
        case MWset:{
            uint8_t a=crsf.get_cmd_buffer(7);
            uint8_t b=crsf.get_cmd_buffer(8);
            if(a==(uint8_t)config.modul_adress) einkanal_Data=b;
            updateGroupFromAddress(a,b);
            break;
        }
        case MWprop:break;
    }
}
uint8_t compressSwitches(uint16_t s){uint8_t r=0;for(uint8_t i=0;i<8;i++)r|=((s>>(i*2))&0x1)<<i;return r;}
