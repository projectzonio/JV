//-----------1-----------
// BLOCK 01: HEADER, INCLUDES, BUILD-TIME TUNING
//
// JVmeteo COREON - firmware 1.3.0 (low-power + recovery + OTA)
// Zaklad: jvmeteook2 (ESP32, ThingSpeak, konfiguracni AP po kratkem resetu).
// Low-power a recovery principy prevzaty z Zonio legacy
// v5_deepsleep_BME280 (ESP8266) a prepsany pro ESP32 rodinu (C3/S3/klasik).
// OTA klient (BLOCK 08) je ESP32 vetev zonio_runtime/zrt_ota.cpp upravena pro
// JVmeteo: zdroj GitHub Releases, vlastni klic podpisu, trigger pres ThingSpeak.
//
// Zachovano beze zmeny: ThingSpeak komunikace (pole 1-7), konfiguracni AP
// JVmeteo_SETUP + web formular, NVS klice "meteo", vychozi piny a senzory.
// Zamerne NENI: MQTT, XOR.
//
// ============================ INDEX BLOKU ================================
// Blok | Obsah                                             | Zavisi na
// -----+---------------------------------------------------+---------------
//  01  | include, verze, ladici konstanty, LOG makra       | -
//  02  | uzivatelska konfigurace, piny, globalni hodnoty   | 01
//  03  | POWER LOGIC CORE: rozhodnuti o odeslani, backoff, | nic (cisty C++,
//      | delka spanku, RTC sloty + CRC, zhaveni MQ,        | host test/twin)
//      | OTA: verze, prefix podpisu, plan kontrol, rollback|
//  04  | RTC sloty v RTC_NOINIT, klasifikace bootu,        | 03
//      | awake watchdog (esp_timer)                        |
//  05  | senzory BME280/BMP280/MPU6050/DHT22, obnova I2C,  | 02, 03, 04
//      | neblokujici vzorkovani MQ-9                       |
//  06  | Wi-Fi (neblokujici, BSSID cache, TX) + ThingSpeak | 02, 03, 04
//  07  | konfiguracni portal, NVS "meteo", okno 1,2 s      | 02, 04
//  08  | OTA: GitHub Releases, podpis, rollback, trigger   | 02, 03, 04
//      | pres ThingSpeak TalkBack                          |
//  09  | uspani (radio off, ulozeni RTC, deep sleep)       | 04
//  10  | stavovy automat mericiho cyklu, setup(), loop()   | vse
//
// Tok mericiho cyklu (BLOCK 10, kazdy krok z loop(), zadne delay()):
//   F_SENZORY -> F_CEKEJ_MQ -> F_PLAN -> [F_WIFI -> F_ODESLI -> F_OTA] -> F_KONEC -> F_SPANEK
// F_OTA bezi jen po USPESNEM odeslani (Wi-Fi uz je zapnuta) a jen kdyz je kontrola
// splatna nebo prisel trigger; uvnitr blokuje (TLS + stazeni), horni mez drzi
// prodlouzeny awake watchdog.
// Ochrana RTC: 2 sloty se sekvenci + magic/verze/velikost/CRC32, zapis vzdy do
// starsiho slotu, ulozeni hned po bootu, watchdog pracuje jen se snapshotem.
// =========================================================================
//
// Opravene chyby v5 (nepreneseny): baseline se po trigger publishi
// neaktualizovala -> odesilani pri kazdem probuzeni; errorCount se nikdy
// nenuloval -> restart smycka po 5 chybach senzoru za celou dobu behu.

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include "ThingSpeak.h" // Oficialni knihovna pro bezpecny prenos dat

// OTA (BLOCK 08): HTTPS, flash, podpis. JV_BEZ_OTA nastavuje jen nativni twin
// (simulace/fw_twin_cpp), jehoz Arduino shim tyhle knihovny nema; na desce se nikdy nedefinuje.
#ifndef JV_BEZ_OTA
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#endif

// Knihovny pro senzory
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>

// Semver MAJOR.MINOR.PATCH (pripona za '-' se pri porovnani ignoruje). MUSI se
// shodovat s tagem vydani vMAJOR.MINOR.PATCH - workflow to kontroluje.
#define FW_VERSION "1.3.2"

// Casove konstanty v milisekundach (puvodni hodnoty zachovany)
#define INTERVAL_SPANKU_MS 90000       // perioda mereni (90 s, mereno od probuzeni k probuzeni)
#define WIFI_TIMEOUT_MS 7000           // maximalni doba pripojovani k Wi-Fi (celkem)
#define TIMEOUT_KONFIGURACE_MS 300000  // 5 minut v rezimu nastaveni pred auto-restartem
#define OKNO_RESETU_PRO_KONFIGURACI_MS 1200  // jeden kratky reset po zapnuti

// --- Low-power: kdy se vubec zapina Wi-Fi ---
// 1 = odesila jen pri zmene nad prah (deadband) nebo jednou za HEARTBEAT cyklu.
// 0 = puvodni chovani: odeslani kazdy cyklus (ostatni uspory plati dal).
#define REZIM_ODESILANI_PRI_ZMENE 1
#define HEARTBEAT_KAZDYCH_N_CYKLU 20   // 20 x 90 s = 30 min, i bez zmeny

// Prahy zmeny proti posledni USPESNE odeslane hodnote (pole ThingSpeak 1-7).
// Pro srovnani v5 pouzival T 1.5 C / H 10 % / P 1.5 hPa (na graf prilis hrube).
#define PRAH_TEPLOTA_C     0.5f
#define PRAH_VLHKOST_PCT   3.0f
#define PRAH_TLAK_HPA      1.0f
#define PRAH_ZRYCHLENI_MS2 0.5f
#define PRAH_MQ9_RAW       50.0f

// --- Low-power: Wi-Fi ---
#define CPU_FREKVENCE_MHZ 80           // 80 MHz je minimum, se kterym bezi Wi-Fi
#define RYCHLE_PRIPOJENI_TIMEOUT_MS 1500  // pokus s ulozenym BSSID+kanalem, pak plny scan
#define ADAPTIVNI_TX_VYKON 1           // snizit TX vykon pri silnem signalu z minula

// --- Recovery ---
#define MAX_PRESKOCENYCH_CYKLU 15      // strop backoffu po selhani site (15 x 90 s)
#define SPANEK_PO_BROWNOUT_MS 600000UL // 10 min x pocet brownoutu po sobe (max 6x)
#define SPANEK_PO_PADU_MS 600000UL     // 10 min po opakovanych padech
#define LIMIT_PADU_PO_SOBE 3
#define LIMIT_BDENI_MS 30000UL         // awake watchdog: max doba bdeni v merici rezimu
#define I2C_TIMEOUT_MS 100

// --- MQ-9 (topeni napajene trvale, FW ho neridi) ---
// Zhaveni se pocita od zapnuti napajeni pres vsechny deep sleep cykly (RTC),
// FW na nej nikdy neceka - do zahrati se pole 7 jen neposila.
#define MQ_ZHAVENI_MS 180000UL         // 3 min, stejne jako legacy fire_sensor (MQ135)
#define MQ_VZORKU 5
#define MQ_PERIODA_VZORKU_MS 20

// --- OTA (vychozi hodnoty; zivot je v portalu a v NVS) ---
#define OTA_VYCHOZI_ZAPNUTO 1          // automaticka kontrola novych verzi
#define OTA_VYCHOZI_INTERVAL_H 24      // jak casto kontrolovat (hodiny, 1..720)
#define OTA_MAX_BACKOFF 3              // po neuspechu interval x2, x4, x8 (strop)
#define OTA_POTVRZENI_CYKLU 3          // tolik dokoncenych cyklu potvrdi novy obraz
#define OTA_MIN_TRIGGER_MS 600000UL    // trigger se ignoruje, kdyz kontrola bezela pred < 10 min
#define OTA_LIMIT_BDENI_MS 180000UL    // awake watchdog behem OTA (TLS + ~1 MB stazeni)
#define OTA_LIMIT_STAZENI_MS 150000UL  // strop jednoho stazeni obrazu
#define OTA_MIN_HALDA 70000UL          // mene volne haldy nez tohle: TLS nezacinat
// Zdroj: GitHub Releases. Pro test proti vlastnimu serveru vytvorte jv_ota_local.h
// (je v .gitignore) a predefinujte JV_OTA_URL_ZAKLAD / JV_TB_HOST / JV_TB_PORT.
#define JV_OTA_TYP "jvmeteo-" CONFIG_IDF_TARGET   // napr. jvmeteo-esp32c3; v podepsane zprave

// 0 = ticho, 1 = chyby, 2 = souhrn cyklu
#define DEBUG_LEVEL 2
#define LOGE(...) do { if (DEBUG_LEVEL >= 1) { Serial.printf("[E] "); Serial.printf(__VA_ARGS__); Serial.println(); } } while (0)
#define LOGI(...) do { if (DEBUG_LEVEL >= 2) { Serial.printf("[I] "); Serial.printf(__VA_ARGS__); Serial.println(); } } while (0)

//-----------1-----------

//-----------2-----------
// BLOCK 02: USER CONFIG, PIN MAP, GLOBAL STATE (unchanged from jvmeteook2)

// Verejne sablony: pred nahranim doplnte vlastni hodnoty.
const unsigned long THINGSPEAK_CHANNEL_ID = 0000000UL;
const char *CONFIG_AP_NAME = "JVmeteo_SETUP";
const char *CONFIG_AP_PASSWORD = "ZMENIT_HESLO"; // nejmene 8 znaku; zmente pred verejnym pouzitim

// Globalni promenne nastaveni
// Doplňte před prvním nahráním firmware. Tyto hodnoty nejsou součástí projektu.
String wifi_ssid = "DOPLNTE_WIFI_SSID";
String wifi_pass = "DOPLNTE_WIFI_HESLO";
String thingspeak_api = "DOPLNTE_THINGSPEAK_WRITE_API";

// Stavy senzoru (zaskrtnuto/nezaskrtnuto)
bool en_bme280 = true, en_bmp280 = false, en_dht22 = false, en_mpu6050 = false, en_mq9 = false;

// OTA nastaveni (portal -> NVS "meteo")
bool ota_povoleno = OTA_VYCHOZI_ZAPNUTO != 0;  // automaticka kontrola; rucni trigger funguje i pri vypnuti
int ota_hodin = OTA_VYCHOZI_INTERVAL_H;        // interval automaticke kontroly
String tb_id = "";                             // ThingSpeak TalkBack ID (prazdne = trigger vypnut)
String tb_key = "";                            // TalkBack API klic (nikdy se nelogguje)

// Definice pinu pro kazdy senzor zvlast (nastavitelne pres web)
int bme_sda = 6, bme_scl = 7;
int bmp_sda = 6, bmp_scl = 7;
int mpu_sda = 6, mpu_scl = 7;
int dht_pin = 5;
int mq9_pin = 2;

// Globalni namerene veliciny
float teplota = NAN, vlhkost = NAN, tlak = NAN;
float mpu_ax = NAN, mpu_ay = NAN, mpu_az = NAN;
int mq9_raw = -1;

Preferences pref;
WebServer server(80);
WiFiClient client; // Klientsky objekt vyzadovany knihovnou ThingSpeak
bool rezim_konfigurace = false;

//-----------2-----------

//-----------3-----------
// BLOCK 03: POWER LOGIC CORE (pure C++, no Arduino/IDF - host test + FW twin)
//
// Vse, co rozhoduje o spotrebe a zotaveni, je tady a nezavisi na hardware:
// kdy zapnout Wi-Fi, kdy prestat zkouset, jak dlouho spat. Blok se da
// vyriznout podle border marks a prelozit nativne (test/host/).

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define JV_POLI 7                       // ThingSpeak pole 1..7
#define JV_RTC_MAGIC 0x4A564D32UL       // "JVM2"
#define JV_RTC_VERZE 3                  // 3: pribyla OTA pole (rezerva[3] nahrazena)

#define JV_OTA_DOMENA "JVMETEO-OTA-SIG-v1"   // domena podpisu; zrt_ota.cpp pouziva jinou
#define JV_OTA_MAX_VERZE 48
#define JV_OTA_MAX_TYP 64
#define JV_OTA_PREFIX_CAP (sizeof(JV_OTA_DOMENA) + JV_OTA_MAX_TYP + 1 + JV_OTA_MAX_VERZE + 1)
#define JV_OTA_SIG_LEN 256
#define JV_OTA_TRAILER (JV_OTA_SIG_LEN + 4)
#define JV_OTA_MS_NEZNAMO 0xFFFFFFFFUL  // kontrola jeste nikdy nebezela -> splatna hned
#define JV_OTA_MIN_HODIN 1
#define JV_OTA_MAX_HODIN 720            // 30 dni; vic by se nevlezlo do uint32 ms s backoffem
#define JV_OTA_PRIKAZ "OTA_CHECK"       // jediny prikaz, ktery TalkBack trigger zna

enum JvOtaVysledek : uint8_t {
  JV_OTA_AKTUALNI = 0,        // server nema novejsi verzi
  JV_OTA_CHYBA_SITE = 1,      // verzi se nepodarilo zjistit
  JV_OTA_CHYBA_STAZENI = 2,   // stazeni/zapis selhal, nic se neaktivovalo
  JV_OTA_PODPIS_ODMITNUT = 3, // obraz se nestal aktivni, bezi dal stary
  JV_OTA_NAINSTALOVANO = 4    // novy obraz je zapsany, nasleduje restart
};

enum JvBoot : uint8_t {
  JV_BOOT_COLD = 0,      // zapnuti napajeni / EN tlacitko
  JV_BOOT_DEEPSLEEP = 1, // radne probuzeni casovacem
  JV_BOOT_SOFT = 2,      // ESP.restart() (napr. po ulozeni konfigurace)
  JV_BOOT_BROWNOUT = 3,  // podpeti - typicky spicka Wi-Fi na slabe baterii
  JV_BOOT_CRASH = 4      // panic / watchdog
};

enum JvDuvod : uint8_t {
  JV_DUVOD_ZADNY = 0,
  JV_DUVOD_PRVNI = 1,     // jeste nic neodeslano (po cold startu)
  JV_DUVOD_ZMENA = 2,     // nektere pole se zmenilo o >= prah
  JV_DUVOD_HEARTBEAT = 4, // HEARTBEAT cyklu bez odeslani
  JV_DUVOD_NOVE_POLE = 8  // objevilo se pole, ktere posledni odeslani nemelo
};

// Stav prezivajici deep sleep i soft/WDT reset (RTC_NOINIT), chraneny CRC.
struct JvRtcState {
  uint32_t magic;
  uint16_t verze;
  uint16_t velikost;
  uint32_t pocetBootu;
  uint32_t pocetVzorku;
  uint32_t odeslanoOk;
  uint32_t odeslanoChyba;
  uint16_t cykluBezOdeslani;
  uint8_t  sitChybStreak;     // po sobe jdouci selhani Wi-Fi/ThingSpeak
  uint8_t  sitPreskocit;      // kolik cyklu jeste nezkouset sit (backoff)
  uint8_t  brownoutStreak;
  uint8_t  padStreak;
  uint8_t  senzorChybStreak;  // cykly bez jedine platne hodnoty
  uint8_t  posledniBoot;
  uint8_t  awakeTimeouty;
  int8_t   posledniRssi;      // 0 = neznamo
  uint8_t  wifiCachePlatna;
  uint8_t  wifiKanal;
  uint8_t  wifiBssid[6];
  uint32_t wifiSsidHash;
  uint8_t  odeslanoMaska;     // bit i = pole i bylo v poslednim odeslani
  uint8_t  otaChybStreak;     // po sobe jdouci neuspesne kontroly (backoff intervalu)
  uint8_t  otaNepotvrzeno;    // 1 = bezi obraz z OTA, jeste neprosel potvrzovacim obdobim
  uint8_t  otaCyklyPoInstalaci;
  uint8_t  otaPozadavek;      // 1 = vzdaleny trigger chce kontrolu hned
  uint32_t otaMsOdKontroly;   // reale ms od posledni kontroly (JV_OTA_MS_NEZNAMO = nikdy)
  float    odeslano[JV_POLI]; // posledni USPESNE odeslane hodnoty (baseline)
  uint32_t sekvence;          // poradi zapisu do RTC slotu (novejsi vyhrava)
  uint32_t msOdNapajeni;      // cas od zapnuti napajeni pres deep sleep (zhaveni MQ)
  uint32_t crc;
};

struct JvLadeni {
  float    prah[JV_POLI];
  uint16_t heartbeatCyklu;
  uint8_t  maxPreskocit;
  bool     rezimPriZmene;
  uint32_t intervalMs;
  uint32_t brownoutSpanekMs;
  uint32_t padSpanekMs;
  uint8_t  limitPadu;
  bool     otaPovoleno;        // automaticka kontrola (rucni trigger funguje i bez ni)
  uint32_t otaIntervalMs;      // jak casto kontrolovat
  uint8_t  otaMaxBackoff;      // po neuspechu interval << min(streak, tohle)
  uint8_t  otaPotvrzeniCyklu;  // kolik dokoncenych cyklu potvrdi novy obraz
  uint32_t otaMinTriggerMs;    // trigger ignorovat, kdyz posledni kontrola byla driv nez tohle
};

struct JvPlan {
  uint8_t duvody;   // JvDuvod bitmaska; 0 = neni co hlasit
  bool    sit;      // zapnout Wi-Fi v tomto cyklu
};

static inline uint8_t jvSatInc(uint8_t v) { return v < 255 ? (uint8_t)(v + 1) : v; }

// CRC-32 (IEEE 802.3, reflected) - nezavisle na platforme.
uint32_t jvCrc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320UL & (0u - (crc & 1u)));
  }
  return ~crc;
}

void jvStavInit(JvRtcState &s) {
  memset(&s, 0, sizeof(s));
  s.magic = JV_RTC_MAGIC;
  s.verze = JV_RTC_VERZE;
  s.velikost = (uint16_t)sizeof(JvRtcState);
  for (int i = 0; i < JV_POLI; i++) s.odeslano[i] = NAN;
  s.otaMsOdKontroly = JV_OTA_MS_NEZNAMO;  // prvni uspesne odeslani rovnou zkontroluje verzi
}

void jvStavZapecetit(JvRtcState &s) {
  s.crc = jvCrc32((const uint8_t *)&s, offsetof(JvRtcState, crc));
}

bool jvStavPlatny(const JvRtcState &s) {
  return s.magic == JV_RTC_MAGIC && s.verze == JV_RTC_VERZE &&
         s.velikost == sizeof(JvRtcState) &&
         s.crc == jvCrc32((const uint8_t *)&s, offsetof(JvRtcState, crc));
}

uint8_t jvMaskaPlatnych(const float hodnoty[JV_POLI]) {
  uint8_t m = 0;
  for (int i = 0; i < JV_POLI; i++) if (!isnan(hodnoty[i])) m |= (uint8_t)(1u << i);
  return m;
}

// Volat jednou na zacatku bootu (po nacteni/inicializaci stavu).
void jvPoBootu(JvRtcState &s, JvBoot boot) {
  s.pocetBootu++;
  s.posledniBoot = (uint8_t)boot;
  if (boot == JV_BOOT_BROWNOUT) s.brownoutStreak = jvSatInc(s.brownoutStreak);
  if (boot == JV_BOOT_CRASH) s.padStreak = jvSatInc(s.padStreak);
  if (boot == JV_BOOT_DEEPSLEEP) s.padStreak = 0;  // predchozi cyklus dobehl az do spanku
  if (boot == JV_BOOT_COLD) s.msOdNapajeni = 0;    // topeni MQ zacina od nuly
}

// --- Ochrana RTC: dva sloty se sekvenci ---
// Zapis jde vzdy do STARSIHO slotu, takze preruseny zapis (brownout, watchdog)
// nikdy nezkazi posledni platny stav. Nacita se platny slot s vyssi sekvenci.
static inline bool jvNovejsi(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

int jvNejnovejsiSlot(const JvRtcState sloty[2]) {
  const bool p0 = jvStavPlatny(sloty[0]), p1 = jvStavPlatny(sloty[1]);
  if (p0 && p1) return jvNovejsi(sloty[1].sekvence, sloty[0].sekvence) ? 1 : 0;
  if (p0) return 0;
  if (p1) return 1;
  return -1;
}

bool jvSlotyNacti(const JvRtcState sloty[2], JvRtcState &out) {
  const int i = jvNejnovejsiSlot(sloty);
  if (i < 0) return false;
  memcpy(&out, &sloty[i], sizeof(out));
  return true;
}

void jvSlotyUloz(JvRtcState sloty[2], JvRtcState &s) {
  const int n = jvNejnovejsiSlot(sloty);
  const int cil = (n == 0) ? 1 : 0;
  s.sekvence = (n >= 0 ? sloty[n].sekvence : s.sekvence) + 1u;
  jvStavZapecetit(s);
  memcpy(&sloty[cil], &s, sizeof(s));
}

// Boot: nacti posledni platny slot (jinak cisty start), zapocitej boot a HNED
// uloz. Bez okamziteho ulozeni by pad uprostred cyklu nikdy nebyl zapocitan
// (kazdy boot by nacetl stav z doby pred padem) a ochrana proti smycce padu
// by nikdy nesepnula. Vraci false pri cistem startu.
bool jvBootStav(JvRtcState sloty[2], JvRtcState &s, JvBoot boot) {
  const bool platny = jvSlotyNacti(sloty, s);
  if (!platny) jvStavInit(s);
  jvPoBootu(s, boot);
  jvSlotyUloz(sloty, s); // commit-at-boot
  return platny;
}

void jvPripoctiCas(JvRtcState &s, uint32_t ms) {
  const uint64_t v = (uint64_t)s.msOdNapajeni + ms;
  s.msOdNapajeni = v > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)v;
  const uint64_t o = (uint64_t)s.otaMsOdKontroly + ms;  // saturuje na JV_OTA_MS_NEZNAMO = "splatna"
  s.otaMsOdKontroly = o > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)o;
}

// Kolik ms jeste chybi do zahrati MQ (0 = zahraty). bdeniMs = millis() tohoto cyklu.
uint32_t jvMqZbyvaMs(const JvRtcState &s, uint32_t bdeniMs, uint32_t zhaveniMs) {
  const uint64_t uplynulo = (uint64_t)s.msOdNapajeni + bdeniMs;
  return uplynulo >= zhaveniMs ? 0u : (uint32_t)(zhaveniMs - uplynulo);
}

uint8_t jvDuvodyOdeslani(const JvRtcState &s, const float cur[JV_POLI], const JvLadeni &t) {
  const uint8_t platne = jvMaskaPlatnych(cur);
  if (platne == 0) return JV_DUVOD_ZADNY;            // neni co poslat (puvodni chovani)
  if (!t.rezimPriZmene) return JV_DUVOD_HEARTBEAT;   // puvodni rezim: kazdy cyklus
  if (s.odeslanoMaska == 0) return JV_DUVOD_PRVNI;

  uint8_t d = JV_DUVOD_ZADNY;
  for (int i = 0; i < JV_POLI; i++) {
    const uint8_t bit = (uint8_t)(1u << i);
    if (!(platne & bit)) continue;                   // vypadek pole netriggeruje
    if (!(s.odeslanoMaska & bit)) { d |= JV_DUVOD_NOVE_POLE; continue; }
    if (fabsf(cur[i] - s.odeslano[i]) >= t.prah[i]) d |= JV_DUVOD_ZMENA;
  }
  if ((uint32_t)s.cykluBezOdeslani + 1u >= t.heartbeatCyklu) d |= JV_DUVOD_HEARTBEAT;
  return d;
}

JvPlan jvNaplanuj(const JvRtcState &s, JvBoot boot, const float cur[JV_POLI],
                  bool komunikaceNastavena, const JvLadeni &t) {
  JvPlan p;
  p.duvody = jvDuvodyOdeslani(s, cur, t);
  // Brownout boot: Wi-Fi spicka by baterii shodila znovu -> jen merit a spat.
  p.sit = p.duvody != JV_DUVOD_ZADNY && komunikaceNastavena &&
          boot != JV_BOOT_BROWNOUT && s.sitPreskocit == 0;
  return p;
}

// Backoff: 1. selhani -> zkusit hned priste; pak 1, 3, 7, 15 ... cyklu pauza.
uint8_t jvBackoffCyklu(uint8_t streak, uint8_t maxPreskocit) {
  if (streak <= 1) return 0;
  const uint8_t sh = streak > 8 ? 8 : streak;
  const uint32_t v = (1UL << (sh - 1)) - 1UL;
  return (uint8_t)(v > maxPreskocit ? maxPreskocit : v);
}

void jvPoSiti(JvRtcState &s, bool ok, const float cur[JV_POLI], int8_t rssi, const JvLadeni &t) {
  if (ok) {
    // Oprava chyby v5: baseline = to, co je opravdu na serveru, VZDY po uspechu.
    for (int i = 0; i < JV_POLI; i++) s.odeslano[i] = cur[i];
    s.odeslanoMaska = jvMaskaPlatnych(cur);
    s.cykluBezOdeslani = 0;
    s.sitChybStreak = 0;
    s.sitPreskocit = 0;
    s.brownoutStreak = 0;
    s.padStreak = 0;
    s.odeslanoOk++;
    s.posledniRssi = rssi;
  } else {
    // Baseline zustava -> pending zmena se odesle pri pristim povolenem pokusu.
    s.odeslanoChyba++;
    s.sitChybStreak = jvSatInc(s.sitChybStreak);
    s.sitPreskocit = jvBackoffCyklu(s.sitChybStreak, t.maxPreskocit);
    s.posledniRssi = 0; // priste plny TX vykon
  }
}

// Volat na konci kazdeho mericiho cyklu.
void jvKonecCyklu(JvRtcState &s, bool sitZkousena, bool odeslanoOk, bool nejakaPlatnaHodnota) {
  s.pocetVzorku++;
  if (!odeslanoOk && s.cykluBezOdeslani < 0xFFFF) s.cykluBezOdeslani++;
  if (!sitZkousena && s.sitPreskocit > 0) s.sitPreskocit--;
  s.senzorChybStreak = nejakaPlatnaHodnota ? 0 : jvSatInc(s.senzorChybStreak);
}

// Awake watchdog vyprsel (vetsinou zaseknuta sit) - pocita se jako selhani site.
void jvPoAwakeTimeoutu(JvRtcState &s, const JvLadeni &t) {
  s.awakeTimeouty = jvSatInc(s.awakeTimeouty);
  s.sitChybStreak = jvSatInc(s.sitChybStreak);
  s.sitPreskocit = jvBackoffCyklu(s.sitChybStreak, t.maxPreskocit);
  s.posledniRssi = 0;
  s.pocetVzorku++;
  if (s.cykluBezOdeslani < 0xFFFF) s.cykluBezOdeslani++;
}

uint32_t jvDelkaSpankuMs(const JvRtcState &s, JvBoot boot, uint32_t bdeniMs, const JvLadeni &t) {
  uint32_t perioda = t.intervalMs;
  if (boot == JV_BOOT_BROWNOUT) {
    const uint32_t k = s.brownoutStreak == 0 ? 1 : (s.brownoutStreak > 6 ? 6 : s.brownoutStreak);
    if (t.brownoutSpanekMs * k > perioda) perioda = t.brownoutSpanekMs * k;
  }
  if (s.padStreak >= t.limitPadu && t.padSpanekMs > perioda) perioda = t.padSpanekMs;
  // Perioda se meri od probuzeni k probuzeni -> odecist dobu bdeni.
  if (perioda > bdeniMs + 1000UL) return perioda - bdeniMs;
  return 1000UL;
}

// 0 = plny TX vykon, 1 = stredni, 2 = nizky.
uint8_t jvTxUroven(int8_t rssi) {
  if (rssi == 0) return 0;
  if (rssi >= -55) return 2;
  if (rssi >= -67) return 1;
  return 0;
}

bool jvMamDetekovatKonfiguracniOkno(JvBoot boot) {
  // Jen po zapnuti / EN. Pri probuzeni ze spanku se 1,2 s ceka zbytecne.
  return boot == JV_BOOT_COLD;
}

// ---------------------------------------------------------------------------
// OTA: cisty rozhodovaci kod (bez site, bez flash). Sit a flash jsou v BLOCK 08.
// ---------------------------------------------------------------------------

// "v1.3.0", "1.3.0-rc1", "1.3" (patch = 0). Cokoli jineho (prazdne, "1.2-lp",
// "1.3.0.5", "abc", cislo delsi nez 6 cifer) je neplatna verze.
bool jvOtaParseVerze(const char *s, int v[3]) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == 'v' || *s == 'V') s++;
  v[0] = v[1] = v[2] = 0;
  int i = 0;
  while (i < 3) {
    if (*s < '0' || *s > '9') return false;
    long n = 0;
    int cifer = 0;
    while (*s >= '0' && *s <= '9') {
      n = n * 10 + (*s - '0');
      s++;
      if (++cifer > 6) return false;
    }
    v[i++] = (int)n;
    if (*s != '.') break;
    s++;
  }
  if (i < 2) return false;  // aspon MAJOR.MINOR
  return *s == '\0' || *s == '-' || *s == '+' || *s == ' ' || *s == '\r' || *s == '\n';
}

// true jen kdyz je serverova verze STRIKTNE vyssi. Stejna verze nebo starsi
// (downgrade) = false; neplatna verze na kterekoli strane = false.
bool jvOtaNovejsi(const char *server, const char *lokalni) {
  int a[3], b[3];
  if (!jvOtaParseVerze(server, a) || !jvOtaParseVerze(lokalni, b)) return false;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;  // stejna verze neni novejsi (jinak by se aktualizovalo porad dokola)
}

// DOMENA\0 typ\0 verze\0 - cast podepsane zpravy, kterou zarizeni zna pred
// stazenim. Vraci delku, 0 kdyz se typ/verze nevejdou nebo jsou prazdne.
size_t jvOtaPrefix(uint8_t *out, size_t cap, const char *typ, const char *ver) {
  const size_t d = sizeof(JV_OTA_DOMENA);  // vcetne NUL
  const size_t t = strlen(typ) + 1;
  const size_t v = strlen(ver) + 1;
  if (t < 2 || t > JV_OTA_MAX_TYP + 1 || v < 2 || v > JV_OTA_MAX_VERZE + 1 || d + t + v > cap) return 0;
  memcpy(out, JV_OTA_DOMENA, d);
  memcpy(out + d, typ, t);
  memcpy(out + d + t, ver, v);
  return d + t + v;
}

uint32_t jvOtaHodinyNaMs(int hodin) {
  if (hodin < JV_OTA_MIN_HODIN) hodin = JV_OTA_MIN_HODIN;
  if (hodin > JV_OTA_MAX_HODIN) hodin = JV_OTA_MAX_HODIN;
  return (uint32_t)hodin * 3600000UL;
}

// Po neuspesne kontrole se interval prodluzuje (x2, x4, x8), aby slaba sit nebo
// vypadly server nevyzerali baterii.
uint32_t jvOtaIntervalMs(const JvRtcState &s, const JvLadeni &t) {
  const uint8_t n = s.otaChybStreak > t.otaMaxBackoff ? t.otaMaxBackoff : s.otaChybStreak;
  const uint64_t v = (uint64_t)t.otaIntervalMs << n;
  return v > 0xFFFFFFFEULL ? 0xFFFFFFFEUL : (uint32_t)v;
}

// Smi se TENTO cyklus zkontrolovat verzi? sitOk = ThingSpeak odeslani prave
// uspelo (Wi-Fi bezi, signal je pouzitelny). Nikdy ne pri brownoutu/padech
// (slaba baterie, TLS spicka by ji shodila) a nikdy dokud novy obraz neprosel
// potvrzenim (zadne retezeni aktualizaci).
bool jvOtaMaKontrolovat(const JvRtcState &s, JvBoot boot, bool sitOk, const JvLadeni &t) {
  if (!sitOk) return false;
  if (boot == JV_BOOT_BROWNOUT || s.brownoutStreak > 0 || s.padStreak > 0) return false;
  if (s.otaNepotvrzeno) return false;
  if (s.otaPozadavek) return s.otaMsOdKontroly >= t.otaMinTriggerMs;  // rucni: i pri vypnute automatice
  if (!t.otaPovoleno) return false;
  return s.otaMsOdKontroly >= jvOtaIntervalMs(s, t);
}

void jvOtaZadostTrigger(JvRtcState &s) { s.otaPozadavek = 1; }

// Pesimisticky zapis PRED pokusem: kdyz se TLS/stazeni zasekne a probehne watchdog
// nebo brownout, pokus je uz zapocitany a backoff plati i bez dalsiho kodu.
void jvOtaPrijemZahajeni(JvRtcState &s) {
  s.otaPozadavek = 0;
  s.otaMsOdKontroly = 0;
  s.otaChybStreak = jvSatInc(s.otaChybStreak);
}

void jvOtaPoKontrole(JvRtcState &s, JvOtaVysledek v) {
  s.otaMsOdKontroly = 0;
  s.otaPozadavek = 0;
  if (v == JV_OTA_AKTUALNI) {
    s.otaChybStreak = 0;
  } else if (v == JV_OTA_NAINSTALOVANO) {
    s.otaChybStreak = 0;
    s.otaNepotvrzeno = 1;  // novy obraz bezi "na zkousku"
    s.otaCyklyPoInstalaci = 0;
  }
  // ostatni vysledky: streak zustava zvyseny z jvOtaPrijemZahajeni
}

// Volat na konci kazdeho DOKONCENEHO mericiho cyklu. Obraz, ktery padne driv
// nez cyklus dobehne, potvrzen nikdy nebude.
void jvOtaPoCyklu(JvRtcState &s, const JvLadeni &t) {
  if (!s.otaNepotvrzeno) return;
  if (s.otaCyklyPoInstalaci < 255) s.otaCyklyPoInstalaci++;
  if (s.otaCyklyPoInstalaci >= t.otaPotvrzeniCyklu) {
    s.otaNepotvrzeno = 0;
    s.otaCyklyPoInstalaci = 0;
  }
}

// Novy obraz padl (limitPadu po sobe) driv, nez se potvrdil -> vratit predchozi slot.
bool jvOtaRollbackNutny(const JvRtcState &s, const JvLadeni &t) {
  return s.otaNepotvrzeno && s.padStreak >= t.limitPadu;
}

// Po rollbacku se na vadnou verzi dlouho nesaha: nejdelsi backoff.
void jvOtaPoRollbacku(JvRtcState &s, const JvLadeni &t) {
  s.otaNepotvrzeno = 0;
  s.otaCyklyPoInstalaci = 0;
  s.padStreak = 0;
  s.otaChybStreak = t.otaMaxBackoff;
  s.otaMsOdKontroly = 0;
  s.otaPozadavek = 0;
}

// Telo odpovedi TalkBack "execute" je text prikazu (prazdne = fronta je prazdna).
// Poznava jedinou vec: JV_OTA_PRIKAZ (bez ohledu na velikost pismen a okolni
// mezery). Cokoli jineho se ignoruje - trigger nikdy nevybira URL ani verzi,
// jen odstartuje stejnou kontrolu, jakou by udelal casovac.
bool jvOtaPrikazJeKontrola(const char *telo) {
  if (!telo) return false;
  while (*telo == ' ' || *telo == '\t' || *telo == '\r' || *telo == '\n' || *telo == '"') telo++;
  const char *p = JV_OTA_PRIKAZ;
  while (*p) {
    char c = *telo++;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c != *p++) return false;
  }
  while (*telo == ' ' || *telo == '\t' || *telo == '\r' || *telo == '\n' || *telo == '"') telo++;
  return *telo == '\0';
}

// --- Cas (NTP) -------------------------------------------------------------
// JVmeteo nema bateriove zalohovane hodiny: po vypadnuti napajeni je cas 1970.
// Hodiny prezivaji deep sleep (RTC), takze staci synchronizovat zridka - jen kdyz
// uz je Wi-Fi pripojena (zadna samostatna relace jen kvuli casu).
static const int64_t JV_CAS_MIN_PLATNY = 1700000000LL;  // 2023-11-14; mene = hodiny nejsou nastavene

bool jvCasPlatny(int64_t now) { return now >= JV_CAS_MIN_PLATNY; }

// Synchronizovat, kdyz: hodiny nejsou platne, nikdy nebyly synchronizovane,
// sly zpet (nedulezive), nebo uplynul interval od posledniho uspesneho syncu.
bool jvCasPotrebaSync(int64_t now, int64_t posledniSync, uint32_t intervalS) {
  if (!jvCasPlatny(now)) return true;
  if (posledniSync <= 0) return true;
  if (now < posledniSync) return true;
  return (now - posledniSync) >= (int64_t)intervalS;
}

//-----------3-----------

//-----------4-----------
// BLOCK 04: RTC STATE, BOOT DIAGNOSTICS, AWAKE WATCHDOG (ESP32 family)

#include <atomic>

// Dva sloty v RTC pameti (preziji deep sleep, ESP.restart i WDT reset).
// FW pracuje s kopii v RAM; do RTC jde jen cely zapeceteny stav (BLOCK 03).
RTC_NOINIT_ATTR JvRtcState rtcSloty[2];
JvRtcState rtcStav;
std::atomic<bool> uspavaniZahajeno(false);  // uspava bud hlavni task, nebo watchdog - nikdy oba
JvBoot bootTyp = JV_BOOT_COLD;
JvLadeni ladeni;
esp_timer_handle_t awakeTimer = nullptr;

void nastav_ladeni() {
  const float prahy[JV_POLI] = { PRAH_TEPLOTA_C, PRAH_VLHKOST_PCT, PRAH_TLAK_HPA,
                                 PRAH_ZRYCHLENI_MS2, PRAH_ZRYCHLENI_MS2, PRAH_ZRYCHLENI_MS2,
                                 PRAH_MQ9_RAW };
  memcpy(ladeni.prah, prahy, sizeof(prahy));
  ladeni.heartbeatCyklu = HEARTBEAT_KAZDYCH_N_CYKLU;
  ladeni.maxPreskocit = MAX_PRESKOCENYCH_CYKLU;
  ladeni.rezimPriZmene = REZIM_ODESILANI_PRI_ZMENE != 0;
  ladeni.intervalMs = INTERVAL_SPANKU_MS;
  ladeni.brownoutSpanekMs = SPANEK_PO_BROWNOUT_MS;
  ladeni.padSpanekMs = SPANEK_PO_PADU_MS;
  ladeni.limitPadu = LIMIT_PADU_PO_SOBE;
  // OTA: hodnoty z portalu (NVS) - volat AZ po nacti_konfiguraci().
  ladeni.otaPovoleno = ota_povoleno;
  ladeni.otaIntervalMs = jvOtaHodinyNaMs(ota_hodin);
  ladeni.otaMaxBackoff = OTA_MAX_BACKOFF;
  ladeni.otaPotvrzeniCyklu = OTA_POTVRZENI_CYKLU;
  ladeni.otaMinTriggerMs = OTA_MIN_TRIGGER_MS;
}

JvBoot klasifikuj_boot() {
  switch (esp_reset_reason()) {
    case ESP_RST_DEEPSLEEP: return JV_BOOT_DEEPSLEEP;
    case ESP_RST_SW:        return JV_BOOT_SOFT;
    case ESP_RST_BROWNOUT:  return JV_BOOT_BROWNOUT;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return JV_BOOT_CRASH;
    default:                return JV_BOOT_COLD;  // POWERON, EXT (EN), USB, neznamy
  }
}

const char *nazev_bootu(JvBoot b) {
  switch (b) {
    case JV_BOOT_DEEPSLEEP: return "deepsleep";
    case JV_BOOT_SOFT: return "soft";
    case JV_BOOT_BROWNOUT: return "BROWNOUT";
    case JV_BOOT_CRASH: return "CRASH";
    default: return "cold";
  }
}

void nacti_rtc_stav() {
  // Po zapnuti napajeni je RTC_NOINIT pamet nahodna -> CRC nesedi v zadnem
  // slotu -> cisty start. Stav se ulozi hned po zapocitani bootu.
  if (!jvBootStav(rtcSloty, rtcStav, bootTyp)) { LOGI("RTC stav inicializovan"); }
}

void uloz_rtc_stav(uint32_t pridatMs) {
  jvPripoctiCas(rtcStav, pridatMs);
  jvSlotyUloz(rtcSloty, rtcStav);
}

void zneplatni_rtc_stav() { memset((void *)rtcSloty, 0, sizeof(rtcSloty)); }

// Pojistka: zadny merici cyklus nesmi baterii vybit zaseknutim (DHCP, TCP,
// ThingSpeak). Callback bezi v esp_timer tasku, nesaha na pracovni kopii
// (hlavni task ji muze prave menit) - vychazi z posledniho ULOZENEHO slotu.
static void awake_timeout_cb(void *) {
  if (uspavaniZahajeno.exchange(true)) return;  // hlavni task uz uspava sam
  JvRtcState s;
  if (!jvSlotyNacti(rtcSloty, s)) jvStavInit(s);
  jvPoAwakeTimeoutu(s, ladeni);
  jvPripoctiCas(s, LIMIT_BDENI_MS + ladeni.intervalMs);
  jvSlotyUloz(rtcSloty, s);
  esp_sleep_enable_timer_wakeup((uint64_t)ladeni.intervalMs * 1000ULL);
  esp_deep_sleep_start();
}

void spust_awake_watchdog() {
  esp_timer_create_args_t args = {};
  args.callback = &awake_timeout_cb;
  args.name = "jv_awake";
  if (esp_timer_create(&args, &awakeTimer) == ESP_OK) {
    esp_timer_start_once(awakeTimer, (uint64_t)LIMIT_BDENI_MS * 1000ULL);
  }
}

void zastav_awake_watchdog() {
  if (awakeTimer) { esp_timer_stop(awakeTimer); }
}

// OTA potrebuje vic nez 30 s (TLS + stazeni ~1 MB). Mez zustava: zaseknuty
// pokus skonci stejnym uspanim jako zaseknuta sit, jen pozdeji.
void prodluz_awake_watchdog(uint32_t ms) {
  if (!awakeTimer) return;
  esp_timer_stop(awakeTimer);
  esp_timer_start_once(awakeTimer, (uint64_t)ms * 1000ULL);
}

//-----------4-----------

//-----------5-----------
// BLOCK 05: SENSORS (I2C bus recovery, forced-mode reads, sleep after read)

// Uvolneni zaseknute I2C sbernice: slave drzi SDA v LOW po preruseni prenosu
// (reset/brownout uprostred cteni). 9 hodinovych pulzu + STOP.
void obnov_i2c_sbernici(int sda, int scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(5);
  if (digitalRead(sda) == HIGH) return;  // sbernice je volna
  LOGE("I2C SDA=%d drzena v LOW, obnovuji sbernici", sda);
  for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
    pinMode(scl, OUTPUT); digitalWrite(scl, LOW); delayMicroseconds(5);
    pinMode(scl, INPUT_PULLUP); delayMicroseconds(5);
  }
  // STOP: SDA LOW -> HIGH pri SCL HIGH
  pinMode(sda, OUTPUT); digitalWrite(sda, LOW); delayMicroseconds(5);
  pinMode(sda, INPUT_PULLUP); delayMicroseconds(5);
}

// Inicializace I2C sbernic na vyzadani
bool inicializuj_i2c(int sda, int scl) {
  Wire.end();
  obnov_i2c_sbernici(sda, scl);
  bool ok = Wire.begin(sda, scl);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  return ok;
}

static bool v_rozsahu(float v, float lo, float hi) { return !isnan(v) && v >= lo && v <= hi; }

// --- MQ-9: neblokujici vzorkovani (5 vzorku po >= 20 ms, jako puvodni FW) ---
struct MqVzorkovac { bool aktivni; uint8_t n; long suma; uint32_t posledni; };
MqVzorkovac mqVz = { false, 0, 0, 0 };

void mq_start() {
  mq9_raw = -1;
  mqVz.aktivni = false;
  if (!en_mq9) return;
  const uint32_t zbyva = jvMqZbyvaMs(rtcStav, millis(), MQ_ZHAVENI_MS);
  if (zbyva > 0) {
    // Nececka se: pole 7 se v tomto cyklu neposle, FW jde normalne spat.
    LOGI("MQ-9 zhaveni, zbyva %lu s", (unsigned long)(zbyva / 1000UL));
    return;
  }
  pinMode(mq9_pin, INPUT);
  mqVz.aktivni = true;
  mqVz.n = 0;
  mqVz.suma = 0;
  mqVz.posledni = millis() - MQ_PERIODA_VZORKU_MS;  // prvni vzorek hned
}

// Vraci true, kdyz je vzorkovani hotove (nebo nebezi). Vola se opakovane.
bool mq_krok() {
  if (!mqVz.aktivni) return true;
  const uint32_t ted = millis();
  if (ted - mqVz.posledni < MQ_PERIODA_VZORKU_MS) return false;
  mqVz.posledni = ted;
  mqVz.suma += analogRead(mq9_pin);
  if (++mqVz.n >= MQ_VZORKU) {
    mq9_raw = (int)(mqVz.suma / mqVz.n);
    mqVz.aktivni = false;
    return true;
  }
  return false;
}

// --- Nacteni dat ze vsech aktivovanych senzoru ---
// Poradi a priority poli jsou stejne jako v puvodnim FW. MQ-9 bezi soubezne
// (mq_krok mezi senzory), ne az na konci s delay().
void cti_vsechny_senzory() {
  // Neplatne nebo odpojene cidlo nesmi zapsat do ThingSpeak falesnou nulu.
  teplota = NAN; vlhkost = NAN; tlak = NAN;
  mpu_ax = NAN; mpu_ay = NAN; mpu_az = NAN;

  if (en_bme280) {
    inicializuj_i2c(bme_sda, bme_scl);
    Adafruit_BME280 bme;
    if (bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire)) {
      // Jedno presne mereni, pote se BME280 sam vrati do usporného sleep rezimu.
      bme.setSampling(Adafruit_BME280::MODE_FORCED,
        Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF);
      if (bme.takeForcedMeasurement()) {
        float t = bme.readTemperature();
        float h = bme.readHumidity();
        float p = bme.readPressure() / 100.0F;
        if (v_rozsahu(t, -40, 85) && v_rozsahu(h, 0, 100) && v_rozsahu(p, 300, 1100)) {
          teplota = t; vlhkost = h; tlak = p;
        } else {
          LOGE("BME280 mimo rozsah: T=%.2f H=%.1f P=%.1f", t, h, p);
        }
      }
    } else {
      LOGE("BME280 nenalezen (SDA=%d SCL=%d)", bme_sda, bme_scl);
    }
  }
  mq_krok();
  if (en_bmp280) {
    inicializuj_i2c(bmp_sda, bmp_scl);
    Adafruit_BMP280 bmp(&Wire);
    if (bmp.begin(0x76) || bmp.begin(0x77)) {
      bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
        Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X1,
        Adafruit_BMP280::FILTER_OFF);
      if (bmp.takeForcedMeasurement()) {
        float t = bmp.readTemperature();
        float p = bmp.readPressure() / 100.0F;
        if (v_rozsahu(t, -40, 85) && v_rozsahu(p, 300, 1100)) { teplota = t; tlak = p; }
        else { LOGE("BMP280 mimo rozsah: T=%.2f P=%.1f", t, p); }
      }
    } else {
      LOGE("BMP280 nenalezen (SDA=%d SCL=%d)", bmp_sda, bmp_scl);
    }
  }
  mq_krok();
  if (en_mpu6050) {
    inicializuj_i2c(mpu_sda, mpu_scl);
    Adafruit_MPU6050 mpu;
    if (mpu.begin(0x68, &Wire) || mpu.begin(0x69, &Wire)) {
      sensors_event_t a, g, temp;
      mpu.getEvent(&a, &g, &temp);
      if (v_rozsahu(a.acceleration.x, -160, 160) && v_rozsahu(a.acceleration.y, -160, 160) &&
          v_rozsahu(a.acceleration.z, -160, 160)) {
        mpu_ax = a.acceleration.x;
        mpu_ay = a.acceleration.y;
        mpu_az = a.acceleration.z;
        if (!en_bme280 && !en_bmp280) { teplota = temp.temperature; }
      }
      // Bez tohoto MPU6050 bezi i behem deep sleep (~3,9 mA) a vybiji baterii
      // vic nez cele ESP32. Pri pristim begin() se sam probudi.
      mpu.enableSleep(true);
    } else {
      LOGE("MPU6050 nenalezen (SDA=%d SCL=%d)", mpu_sda, mpu_scl);
    }
  }
  mq_krok();
  if (en_dht22) {
    DHT dht(dht_pin, DHT22);
    dht.begin();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (v_rozsahu(t, -40, 80)) { teplota = t; }
    if (v_rozsahu(h, 0, 100)) { vlhkost = h; }
  }
  mq_krok();
}

void hodnoty_do_pole(float out[JV_POLI]) {
  out[0] = teplota; out[1] = vlhkost; out[2] = tlak;
  out[3] = mpu_ax; out[4] = mpu_ay; out[5] = mpu_az;
  out[6] = mq9_raw >= 0 ? (float)mq9_raw : NAN;
}

//-----------5-----------

//-----------6-----------
// BLOCK 06: WI-FI (fast reconnect, adaptive TX) + THINGSPEAK (unchanged fields)

bool komunikace_nastavena() {
  // Cislo kanalu se k zapisu nepouziva (knihovna ThingSpeak posila jen API klic),
  // takze nesmi byt podminkou: jedna a ta samá release binarka musi fungovat pro kazdou
  // jednotku a vsechno jednotkove (Wi-Fi, klic) je jen v NVS.
  return wifi_ssid.length() > 0 && wifi_ssid != "DOPLNTE_WIFI_SSID" &&
         thingspeak_api.length() > 0 && thingspeak_api != "DOPLNTE_THINGSPEAK_WRITE_API";
}

static uint32_t hash_ssid() {
  return jvCrc32((const uint8_t *)wifi_ssid.c_str(), wifi_ssid.length());
}

static void nastav_tx_vykon() {
#if ADAPTIVNI_TX_VYKON
  switch (jvTxUroven(rtcStav.posledniRssi)) {
    case 2: WiFi.setTxPower(WIFI_POWER_11dBm); break;
    case 1: WiFi.setTxPower(WIFI_POWER_15dBm); break;
    default: break;  // vychozi plny vykon
  }
#endif
}

enum WifiStav : uint8_t { WS_NECINNA, WS_RYCHLE, WS_PREPNI, WS_PLNE, WS_OK, WS_CHYBA };
WifiStav wifiStav = WS_NECINNA;
WifiStav wifi_krok();  // explicitni prototyp: Arduino by ho jinak vlozil pred enum
uint32_t wifiStart = 0, wifiPrepnutiOd = 0;

// Pripojeni bez blokovani: wifi_start() ho spusti, wifi_krok() se vola z loop().
// Nejdriv primo na ulozeny BSSID+kanal (bez scanu), po RYCHLE_PRIPOJENI_TIMEOUT_MS
// plny scan. Celkovy limit zustava WIFI_TIMEOUT_MS jako v puvodnim FW.
void wifi_start() {
  WiFi.persistent(false);   // nezapisovat Wi-Fi konfiguraci do flash pri kazdem cyklu
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  nastav_tx_vykon();
  wifiStart = millis();
  if (rtcStav.wifiCachePlatna && rtcStav.wifiSsidHash == hash_ssid() && rtcStav.wifiKanal > 0) {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str(), rtcStav.wifiKanal, rtcStav.wifiBssid, true);
    wifiStav = WS_RYCHLE;
  } else {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    wifiStav = WS_PLNE;
  }
}

WifiStav wifi_krok() {
  const uint32_t ted = millis();
  if (wifiStav == WS_RYCHLE || wifiStav == WS_PLNE) {
    if (WiFi.status() == WL_CONNECTED) {
      const uint8_t *b = WiFi.BSSID();
      if (b) { memcpy(rtcStav.wifiBssid, b, 6); }
      rtcStav.wifiKanal = (uint8_t)WiFi.channel();
      rtcStav.wifiSsidHash = hash_ssid();
      rtcStav.wifiCachePlatna = b ? 1 : 0;
      wifiStav = WS_OK;
      LOGI("Wi-Fi OK za %lu ms, RSSI %d dBm, kanal %u",
           (unsigned long)(ted - wifiStart), (int)WiFi.RSSI(), rtcStav.wifiKanal);
    } else if (ted - wifiStart >= WIFI_TIMEOUT_MS) {
      rtcStav.wifiCachePlatna = 0;
      wifiStav = WS_CHYBA;
      LOGE("Wi-Fi se nepripojila za %d ms", WIFI_TIMEOUT_MS);
    } else if (wifiStav == WS_RYCHLE && ted - wifiStart >= RYCHLE_PRIPOJENI_TIMEOUT_MS) {
      LOGI("Rychle pripojeni selhalo, plny scan");
      rtcStav.wifiCachePlatna = 0;
      WiFi.disconnect();
      wifiPrepnutiOd = ted;
      wifiStav = WS_PREPNI;
    }
  } else if (wifiStav == WS_PREPNI && ted - wifiPrepnutiOd >= 20) {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    wifiStav = WS_PLNE;
  }
  return wifiStav;
}

// --- Odeslani platnych hodnot oficialni knihovnou ThingSpeak ---
bool odesli_na_thingspeak() {
  if (WiFi.status() != WL_CONNECTED) { return false; }

  // Prirazeni jen skutecne nactenych hodnot do jednotlivych poli na cloudu.
  bool ma_data = false;
  if (!isnan(teplota)) { ThingSpeak.setField(1, teplota); ma_data = true; }
  if (!isnan(vlhkost)) { ThingSpeak.setField(2, vlhkost); ma_data = true; }
  if (!isnan(tlak)) { ThingSpeak.setField(3, tlak); ma_data = true; }
  if (!isnan(mpu_ax)) { ThingSpeak.setField(4, mpu_ax); ma_data = true; }
  if (!isnan(mpu_ay)) { ThingSpeak.setField(5, mpu_ay); ma_data = true; }
  if (!isnan(mpu_az)) { ThingSpeak.setField(6, mpu_az); ma_data = true; }
  if (mq9_raw >= 0) { ThingSpeak.setField(7, mq9_raw); ma_data = true; }
  if (!ma_data) { return false; }

  int response = ThingSpeak.writeFields(THINGSPEAK_CHANNEL_ID, thingspeak_api.c_str());
  client.stop();
  if (response == 200) {
    Serial.println("Data uspesne zapsana na ThingSpeak skrze oficialni API.");
    return true;
  } else {
    Serial.print("Chyba odesilani ThingSpeak, kod: "); Serial.println(response);
    return false;
  }
}

//-----------6-----------

//-----------7-----------
// BLOCK 07: CONFIG PORTAL (unchanged UI, NVS namespace "meteo")

// --- Responzivni konfiguracni stranka, funguje na telefonu i pocitaci ---
const char KONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<title>JVmeteo COREON – nastavení</title>
<style>
:root{color-scheme:dark;--ink:#041940;--night:#061c4b;--card:rgba(8,35,82,.78);--line:rgba(184,218,255,.24);--text:#f4f8ff;--muted:#b4c7e8;--aqua:#68dff1;--violet:#a98aff;--good:#7ee4bb}*{box-sizing:border-box}body{min-height:100vh;margin:0;padding:26px 18px;background:radial-gradient(circle at 12% 10%,#1b579d 0,transparent 28%),radial-gradient(circle at 87% 20%,#443880 0,transparent 25%),linear-gradient(145deg,#020b29,#07326b 52%,#061a4a);color:var(--text);font-family:Arial,Helvetica,sans-serif;display:flex;align-items:center;justify-content:center}.sky{position:fixed;inset:0;pointer-events:none;opacity:.7;background-image:radial-gradient(circle at 8% 18%,#fff 0 1px,transparent 1.8px),radial-gradient(circle at 81% 10%,#dceaff 0 1px,transparent 1.8px),radial-gradient(circle at 70% 68%,#fff 0 1px,transparent 1.8px),radial-gradient(circle at 21% 79%,#dceaff 0 1px,transparent 1.8px)}.shell{width:min(100%,980px);position:relative}.brand{text-align:center;margin:4px 0 22px}.brand-mark{display:inline-flex;align-items:center;gap:10px;font-size:clamp(30px,6vw,44px);font-weight:800;letter-spacing:-1px}.sun{width:30px;height:30px;border:4px solid #ffd369;border-radius:50%;box-shadow:0 0 0 5px rgba(255,211,105,.16),0 0 20px rgba(255,211,105,.38)}.byline{margin:7px 0 0;color:var(--muted);font-size:14px;letter-spacing:.4px}.intro{text-align:center;margin:0 0 18px;color:#d7e5ff;font-size:15px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.card{background:linear-gradient(145deg,rgba(19,59,122,.88),var(--card));border:1px solid var(--line);border-radius:24px;padding:20px;box-shadow:0 16px 36px rgba(0,7,29,.23);backdrop-filter:blur(8px)}.wide{grid-column:1/-1}.card-title{display:flex;align-items:center;gap:10px;margin:0 0 16px;font-size:18px;font-weight:700}.symbol{width:30px;height:30px;border-radius:10px;display:grid;place-items:center;background:rgba(104,223,241,.14);color:var(--aqua);font-size:18px}.hint{margin:-8px 0 14px;color:var(--muted);font-size:13px;line-height:1.35}.sensor-list{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.toggle{display:flex;align-items:center;gap:10px;padding:12px;border:1px solid rgba(190,219,255,.15);border-radius:16px;background:rgba(0,13,47,.2);cursor:pointer;min-height:62px}.toggle:hover{border-color:rgba(104,223,241,.6)}.toggle input{appearance:none;width:23px;height:23px;margin:0;border:2px solid #8fa9d1;border-radius:8px;background:#071a42;display:grid;place-content:center;flex:0 0 auto}.toggle input:checked{background:linear-gradient(135deg,var(--aqua),var(--violet));border-color:#d5f8ff}.toggle input:checked:after{content:'✓';color:#061b46;font-weight:900;font-size:16px}.sensor-name{font-size:15px;font-weight:700}.sensor-detail{display:block;margin-top:3px;color:var(--muted);font-size:12px}.pin-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.pin{padding:12px;border-radius:16px;background:rgba(0,13,47,.23);border:1px solid rgba(190,219,255,.15)}.pin-name{display:block;margin-bottom:9px;font-weight:700;font-size:14px}.pin-fields{display:flex;gap:8px}.field{flex:1;min-width:0}.field label,.network label{display:block;color:var(--muted);font-size:12px;margin:0 0 5px}input[type=number],input[type=text],input[type=password]{width:100%;min-height:43px;padding:9px 10px;border:1px solid rgba(190,219,255,.28);border-radius:12px;background:#061a43;color:var(--text);font-size:16px;outline:none}input:focus{border-color:var(--aqua);box-shadow:0 0 0 3px rgba(104,223,241,.16)}.network{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}.save{width:100%;min-height:56px;margin-top:16px;border:0;border-radius:17px;background:linear-gradient(105deg,#39cce1,#608ff0 55%,#a178ea);color:#06173c;font-size:17px;font-weight:800;letter-spacing:.2px;cursor:pointer;box-shadow:0 10px 24px rgba(60,190,232,.25)}.save:active{transform:translateY(1px)}.footer{text-align:center;padding:26px 12px 6px}.coreon{font-size:clamp(34px,8vw,58px);font-weight:900;letter-spacing:3px;background:linear-gradient(100deg,#81e7f6,#d0c3ff);-webkit-background-clip:text;background-clip:text;color:transparent}.firmware{margin-top:7px;color:var(--muted);font-size:13px}.notice{margin:13px 0 0;color:#bdeedb;font-size:13px}@media(max-width:650px){body{padding:22px 12px;align-items:flex-start}.shell{padding-top:5px}.grid,.sensor-list,.pin-grid,.network{grid-template-columns:1fr}.card{border-radius:20px;padding:17px}.brand{margin-bottom:18px}.intro{line-height:1.4}.toggle{min-height:57px}.wide{grid-column:auto}}@media(min-width:651px){.sensors{grid-column:1/-1}}
</style></head><body><div class='sky'></div><main class='shell'>
<header class='brand'><div class='brand-mark'><span class='sun'></span>JVmeteo</div><p class='byline'>by Jan Vejvoda</p></header>
<p class='intro'>Nastavení měřicí stanice a bezpečného odesílání hodnot do ThingSpeak.</p>
<form method='POST' autocomplete='off'><section class='grid'>
<article class='card sensors'><h2 class='card-title'><span class='symbol'>⌁</span>Aktivní senzory</h2><p class='hint'>Zapněte jen připojené moduly. Šetříte tím energii akumulátoru.</p><div class='sensor-list'>
<label class='toggle'><input type='checkbox' name='en_bme' %BME_CH%><span><span class='sensor-name'>BME280</span><span class='sensor-detail'>teplota, vlhkost, tlak · I²C</span></span></label>
<label class='toggle'><input type='checkbox' name='en_bmp' %BMP_CH%><span><span class='sensor-name'>BMP280</span><span class='sensor-detail'>teplota, tlak · I²C</span></span></label>
<label class='toggle'><input type='checkbox' name='en_mpu' %MPU_CH%><span><span class='sensor-name'>MPU6050</span><span class='sensor-detail'>pohyb a zrychlení · I²C</span></span></label>
<label class='toggle'><input type='checkbox' name='en_dht' %DHT_CH%><span><span class='sensor-name'>DHT22</span><span class='sensor-detail'>teplota a vlhkost · digitální</span></span></label>
<label class='toggle'><input type='checkbox' name='en_mq9' %MQ9_CH%><span><span class='sensor-name'>MQ-9</span><span class='sensor-detail'>plynové čidlo · analogový vstup</span></span></label>
</div></article>
<article class='card'><h2 class='card-title'><span class='symbol'>↔</span>Piny připojení</h2><p class='hint'>Číslo pinu lze zadat nebo změnit šipkami v poli.</p><div class='pin-grid'>
<div class='pin'><span class='pin-name'>BME280</span><div class='pin-fields'><div class='field'><label>SDA</label><input type='number' name='bme_sda' value='%BME_SDA%' min='0' max='48'></div><div class='field'><label>SCL</label><input type='number' name='bme_scl' value='%BME_SCL%' min='0' max='48'></div></div></div>
<div class='pin'><span class='pin-name'>BMP280</span><div class='pin-fields'><div class='field'><label>SDA</label><input type='number' name='bmp_sda' value='%BMP_SDA%' min='0' max='48'></div><div class='field'><label>SCL</label><input type='number' name='bmp_scl' value='%BMP_SCL%' min='0' max='48'></div></div></div>
<div class='pin'><span class='pin-name'>MPU6050</span><div class='pin-fields'><div class='field'><label>SDA</label><input type='number' name='mpu_sda' value='%MPU_SDA%' min='0' max='48'></div><div class='field'><label>SCL</label><input type='number' name='mpu_scl' value='%MPU_SCL%' min='0' max='48'></div></div></div>
<div class='pin'><span class='pin-name'>DHT22</span><div class='field'><label>Datový pin</label><input type='number' name='dht_pin' value='%DHT_PIN%' min='0' max='48'></div></div>
<div class='pin'><span class='pin-name'>MQ-9</span><div class='field'><label>Analogový pin</label><input type='number' name='mq9_pin' value='%MQ9_PIN%' min='0' max='48'></div></div>
</div></article>
<article class='card wide'><h2 class='card-title'><span class='symbol'>⌁</span>Síť a ThingSpeak</h2><p class='hint'>Prázdné heslo Wi‑Fi nebo Write API klíč ponechá dosavadní uloženou hodnotu.</p><div class='network'><div><label>Název Wi‑Fi sítě (SSID)</label><input type='text' name='ssid' value='%SSID%' maxlength='32' required></div><div><label>Heslo Wi‑Fi</label><input type='password' name='pass' placeholder='Ponechat beze změny' maxlength='63'></div><div><label>ThingSpeak Write API klíč</label><input type='password' name='ts' placeholder='Ponechat beze změny' maxlength='32'></div></div></article>
<article class='card wide'><h2 class='card-title'><span class='symbol'>⇪</span>Aktualizace firmware</h2><p class='hint'>Nová verze se stahuje z GitHub Releases a před instalací se ověří její podpis. Kontrola probíhá jen při odesílání na ThingSpeak, kdy je Wi‑Fi stejně zapnutá. Po neúspěchu se interval prodlužuje. Zapadlá aktualizace, která se nespustí, se sama vrátí na předchozí verzi.</p><div class='network'><div><label class='toggle'><input type='checkbox' name='en_ota' %OTA_CH%><span><span class='sensor-name'>Automatické aktualizace</span><span class='sensor-detail'>pravidelná kontrola nové verze</span></span></label></div><div><label>Kontrolovat každých (hodin, 1–720)</label><input type='number' name='ota_hod' value='%OTA_HOD%' min='1' max='720'></div><div><label>Nainstalovaná verze</label><input type='text' value='%FW%' readonly></div></div><p class='hint' style='margin-top:14px'>Vzdálená kontrola (nepovinné): vytvořte na ThingSpeaku aplikaci TalkBack a vložte její ID a klíč. Příkaz <b>OTA_CHECK</b> ve frontě pak spustí kontrolu při nejbližším odeslání, i když jsou automatické aktualizace vypnuté. Příkaz nikdy nevybírá, co se nainstaluje.</p><div class='network'><div><label>TalkBack ID</label><input type='text' name='tb_id' value='%TB_ID%' maxlength='12' inputmode='numeric'></div><div><label>TalkBack API klíč</label><input type='password' name='tb_key' placeholder='Ponechat beze změny' maxlength='24'></div></div></article>
</section><button class='save' type='submit' name='ulozit' value='1'>Uložit nastavení a restartovat měření</button></form>
<footer class='footer'><div class='coreon'>COREON</div><div class='firmware'>Firmware %FW% · pro hardware 1.1 až 2.4</div><div class='notice'>Po uložení se stanice restartuje a spustí měřicí cyklus.</div></footer>
</main></body></html>
)rawliteral";

void nacti_konfiguraci() {
  pref.begin("meteo", true);
  wifi_ssid = pref.getString("ssid", wifi_ssid);
  wifi_pass = pref.getString("pass", wifi_pass);
  thingspeak_api = pref.getString("ts", thingspeak_api);
  en_bme280 = pref.getBool("en_bme", en_bme280);
  en_bmp280 = pref.getBool("en_bmp", en_bmp280);
  en_mpu6050 = pref.getBool("en_mpu", en_mpu6050);
  en_dht22 = pref.getBool("en_dht", en_dht22);
  en_mq9 = pref.getBool("en_mq9", en_mq9);
  bme_sda = pref.getInt("bme_sda", bme_sda);
  bme_scl = pref.getInt("bme_scl", bme_scl);
  bmp_sda = pref.getInt("bmp_sda", bmp_sda);
  bmp_scl = pref.getInt("bmp_scl", bmp_scl);
  mpu_sda = pref.getInt("mpu_sda", mpu_sda);
  mpu_scl = pref.getInt("mpu_scl", mpu_scl);
  dht_pin = pref.getInt("dht_pin", dht_pin);
  mq9_pin = pref.getInt("mq9_pin", mq9_pin);
  ota_povoleno = pref.getBool("en_ota", ota_povoleno);
  ota_hodin = pref.getInt("ota_hod", ota_hodin);
  tb_id = pref.getString("tb_id", tb_id);
  tb_key = pref.getString("tb_key", tb_key);
  pref.end();
}

// Prvni start na 1,2 s pripravi konfiguracni okno. Kratky reset v tomto
// okne spusti na dalsim bootu konfiguracni pristupovy bod. Jde o stejny,
// na teto desce osvedceny princip jako v puvodnim firmware - jen se uz
// neprovadi pri kazdem probuzeni ze spanku (1,2 s bdeni + 2 zapisy NVS / 90 s).
// Okno uz neblokuje: behem 1,2 s probiha mereni i Wi-Fi, usnout se smi az po nem.
bool oknoOtevreno = false;
uint32_t oknoOd = 0;
bool restartNaplanovan = false;
uint32_t restartOd = 0;

bool okno_start() {
  pref.begin("meteo", true);
  const bool konfiguracni_okno = pref.getBool("config_arm", false);
  pref.end();
  if (konfiguracni_okno) {
    pref.begin("meteo", false);
    pref.remove("config_arm");
    pref.end();
    return true;
  }
  pref.begin("meteo", false);
  pref.putBool("config_arm", true);
  pref.end();
  oknoOtevreno = true;
  oknoOd = millis();
  return false;
}

void okno_krok() {
  if (oknoOtevreno && millis() - oknoOd >= OKNO_RESETU_PRO_KONFIGURACI_MS) {
    pref.begin("meteo", false);
    pref.remove("config_arm");
    pref.end();
    oknoOtevreno = false;
  }
}

// --- HTML Stranika: Rezim Konfigurace ---
void obsluha_konfig_web() {
  if (server.hasArg("ulozit")) {
    en_bme280 = server.hasArg("en_bme");
    en_bmp280 = server.hasArg("en_bmp");
    en_mpu6050 = server.hasArg("en_mpu");
    en_dht22 = server.hasArg("en_dht");
    en_mq9 = server.hasArg("en_mq9");

    wifi_ssid = server.arg("ssid");
    if (server.arg("pass").length() > 0) { wifi_pass = server.arg("pass"); }
    if (server.arg("ts").length() > 0) { thingspeak_api = server.arg("ts"); }
    bme_sda = server.arg("bme_sda").toInt(); bme_scl = server.arg("bme_scl").toInt();
    bmp_sda = server.arg("bmp_sda").toInt(); bmp_scl = server.arg("bmp_scl").toInt();
    mpu_sda = server.arg("mpu_sda").toInt(); mpu_scl = server.arg("mpu_scl").toInt();
    dht_pin = server.arg("dht_pin").toInt();
    mq9_pin = server.arg("mq9_pin").toInt();

    ota_povoleno = server.hasArg("en_ota");
    ota_hodin = server.arg("ota_hod").toInt();
    if (ota_hodin < JV_OTA_MIN_HODIN || ota_hodin > JV_OTA_MAX_HODIN) { ota_hodin = OTA_VYCHOZI_INTERVAL_H; }
    String novy_tb_id = server.arg("tb_id");
    novy_tb_id.trim();
    tb_id = "";
    for (size_t i = 0; i < novy_tb_id.length(); i++) {  // jen cislice: ID jde do URL i do HTML
      if (novy_tb_id[i] >= '0' && novy_tb_id[i] <= '9') { tb_id += novy_tb_id[i]; }
    }
    if (server.arg("tb_key").length() > 0) { tb_key = server.arg("tb_key"); tb_key.trim(); }
    if (tb_id.length() == 0) { tb_key = ""; }  // bez ID se trigger nepouzije; klic nenechavat ladem

    pref.begin("meteo", false);
    pref.putString("ssid", wifi_ssid); pref.putString("pass", wifi_pass); pref.putString("ts", thingspeak_api);
    pref.putBool("en_bme", en_bme280); pref.putBool("en_bmp", en_bmp280); pref.putBool("en_mpu", en_mpu6050); pref.putBool("en_dht", en_dht22); pref.putBool("en_mq9", en_mq9);
    pref.putInt("bme_sda", bme_sda); pref.putInt("bme_scl", bme_scl);
    pref.putInt("bmp_sda", bmp_sda); pref.putInt("bmp_scl", bmp_scl);
    pref.putInt("mpu_sda", mpu_sda); pref.putInt("mpu_scl", mpu_scl);
    pref.putInt("dht_pin", dht_pin); pref.putInt("mq9_pin", mq9_pin);
    pref.putBool("en_ota", ota_povoleno); pref.putInt("ota_hod", ota_hodin);
    pref.putString("tb_id", tb_id); pref.putString("tb_key", tb_key);
    pref.end();

    // Nova sit / senzory: zahodit cache BSSID, baseline i backoff.
    zneplatni_rtc_stav();

    server.send(200, "text/html", "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:linear-gradient(145deg,#020b29,#07326b);color:#f4f8ff;font-family:Arial;text-align:center}.box{padding:32px;border:1px solid #4f85bf;border-radius:24px;background:rgba(8,35,82,.8)}b{color:#72e4f3}</style><div class='box'><h1>✓ Nastavení uloženo</h1><p>Stanice se nyní restartuje a zahájí měření.</p><b>JVmeteo COREON</b></div></html>");
    // Restart za 2 s z loop() - odpoved se mezitim odesle, nic neblokuje.
    restartNaplanovan = true;
    restartOd = millis();
    return;
  }

  String page = String(KONFIG_PAGE);
  page.replace("%BME_CH%", en_bme280 ? "checked" : "");
  page.replace("%BMP_CH%", en_bmp280 ? "checked" : "");
  page.replace("%MPU_CH%", en_mpu6050 ? "checked" : "");
  page.replace("%DHT_CH%", en_dht22 ? "checked" : "");
  page.replace("%MQ9_CH%", en_mq9 ? "checked" : "");
  page.replace("%BME_SDA%", String(bme_sda)); page.replace("%BME_SCL%", String(bme_scl));
  page.replace("%BMP_SDA%", String(bmp_sda)); page.replace("%BMP_SCL%", String(bmp_scl));
  page.replace("%MPU_SDA%", String(mpu_sda)); page.replace("%MPU_SCL%", String(mpu_scl));
  page.replace("%DHT_PIN%", String(dht_pin)); page.replace("%MQ9_PIN%", String(mq9_pin));
  page.replace("%SSID%", wifi_ssid);
  page.replace("%OTA_CH%", ota_povoleno ? "checked" : "");
  page.replace("%OTA_HOD%", String(ota_hodin));
  page.replace("%TB_ID%", tb_id);
  page.replace("%FW%", FW_VERSION);

  server.send(200, "text/html", page);
}

void spust_konfiguracni_ap() {
  // Fixni nastaveni IP adres pro AP
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_NAME, CONFIG_AP_PASSWORD);
  server.on("/", obsluha_konfig_web);
  server.begin();
}

//-----------7-----------

//-----------8-----------
// BLOCK 08: OTA - GitHub Releases, podepsany obraz, rollback, trigger pres ThingSpeak TalkBack
//
// Prevzato z zonio_runtime/zrt_ota.cpp (ESP32 vetev, na HW overena) a upraveno pro
// JVmeteo, ktery nema MQTT, Gate ani Zonio server:
//   zdroj    GitHub Releases: <JV_OTA_URL_ZAKLAD><typ>.version.txt a <typ>.bin
//            (github.com presmeruje 302 na CDN, bez basic auth)
//   podpis   vlastni klic (jv_ota_pubkey.h) a domena JVMETEO-OTA-SIG-v1; typ + verze
//            jsou v podepsane zprave -> obraz pro jiny cip ani starsi obraz neprojde
//   bez      MQTT triggeru, Gate provisioningu a "minimal OTA boot" (ESP32 ma na
//            TLS dost haldy i v aplikaci; ESP8266 varianta to resi restartem)
//   navic    rollback na predchozi slot a trigger pres ThingSpeak TalkBack
//
// TLS bezi se setInsecure(): transport nedokazuje, kdo obraz poslal. O tom, co se
// stane aktivnim, rozhoduje vyhradne podpis, overeny mezi poslednim zapisem a
// Update.end(). Pri neplatnem podpisu se boot partition neprepne.

#ifdef JV_BEZ_OTA
// Nativni twin: OTA se nekompiluje, faze F_OTA jen preskoci.
static volatile bool ota_probiha = false;
static void cas_nastav_tz() {}
static void cas_zaloguj(const char *) {}
static void cas_krok() {}
static void ota_krok() {}
static void ota_rollback() {}
#else  // JV_BEZ_OTA - cely zbytek bloku 08

#include "esp_sntp.h"
#include <time.h>

// Behem staziteni a zapisu obrazu je vsechna ostatni cinnost POZASTAVENA: nemeri se,
// neodesila se, nebezi konfiguracni portal a Wi-Fi nesmi uspavat radio (jinak
// stahovani trva dvakrat dele). Cyklus je jednovlaknovy, takze pozastaveni
// zajistuje uz to, ze ota_krok() blokuje; priznak navic brani znovuvstupu z loop()
// a ma jedno misto, ktere se vzdy uvolni (RAII), at ota_krok() skonci jakkoli.
static volatile bool ota_probiha = false;
struct OtaPauza {
  OtaPauza()  { ota_probiha = true;  WiFi.setSleep(false); LOGI("OTA: cinnost pozastavena (mereni, odesilani, portal)"); }
  ~OtaPauza() { ota_probiha = false; LOGI("OTA: cinnost obnovena"); }
};

// --- Cas pres NTP: jen kdyz je Wi-Fi uz pripojena, nejvyse 1x za CAS_SYNC_INTERVAL_S ---
#define CAS_TZ "CET-1CEST,M3.5.0,M10.5.0/3"   // stredni Evropa vcetne letniho casu
#define CAS_SYNC_INTERVAL_S 86400UL           // 24 h
#define CAS_SYNC_TIMEOUT_MS 4000UL            // vic radio nezapina; po neuspechu se zkusi pri dalsim odeslani
RTC_DATA_ATTR static int64_t rtcCasPosledniSync = 0;  // prezije deep sleep, po vypadnuti napajeni je 0 stejne jako hodiny

static void cas_nastav_tz() { setenv("TZ", CAS_TZ, 1); tzset(); }  // po resetu je TZ prazdne

static void cas_zaloguj(const char *zdroj) {
  const time_t t = time(nullptr);
  if (!jvCasPlatny((int64_t)t)) { LOGI("Cas: nenastaven (%s)", zdroj); return; }
  struct tm tmv; localtime_r(&t, &tmv);
  char b[24]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
  LOGI("Cas: %s (%s)", b, zdroj);
}

static void cas_krok() {
  if (!jvCasPotrebaSync((int64_t)time(nullptr), rtcCasPosledniSync, CAS_SYNC_INTERVAL_S)) { return; }
  if (WiFi.status() != WL_CONNECTED) { return; }
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(CAS_TZ, "pool.ntp.org", "time.nist.gov");
  const uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < CAS_SYNC_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    rtcCasPosledniSync = (int64_t)time(nullptr);
    cas_zaloguj("NTP");
  } else {
    LOGE("NTP: bez odpovedi do %lu ms, zkusi se pri dalsim odeslani", (unsigned long)CAS_SYNC_TIMEOUT_MS);
  }
}

#if __has_include("jv_ota_local.h")
#include "jv_ota_local.h"   // jen pro lokalni testy (v .gitignore): JV_OTA_URL_ZAKLAD, JV_TB_HOST, JV_TB_PORT
#endif
#include "jv_ota_pubkey.h"
#include "jv_ota_config.h"  // JV_OTA_REPO "<majitel>/<repozitar>"; CI ho prepise podle github.repository

#ifndef JV_OTA_URL_ZAKLAD
#define JV_OTA_URL_ZAKLAD "https://github.com/" JV_OTA_REPO "/releases/latest/download/"
#endif
#ifndef JV_TB_HOST
#define JV_TB_HOST "api.thingspeak.com"
#endif
#ifndef JV_TB_PORT
#define JV_TB_PORT 80
#endif

static bool ota_overit_podpis(const uint8_t hash[32], const uint8_t *sig) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  const char *pem = JV_OTA_PUBKEY_PEM;
  const bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, strlen(pem) + 1) == 0 &&
                  mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA &&
                  mbedtls_pk_get_bitlen(&pk) == 2048 &&
                  mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, JV_OTA_SIG_LEN) == 0;
  mbedtls_pk_free(&pk);
  return ok;
}

struct JvSha256 {
  mbedtls_sha256_context ctx;
  JvSha256()  { mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0); }
  ~JvSha256() { mbedtls_sha256_free(&ctx); }
  void add(const uint8_t *p, size_t n) { mbedtls_sha256_update(&ctx, p, n); }
  void finish(uint8_t out[32]) { mbedtls_sha256_finish(&ctx, out); }
};

// Proud firmware.bin do neaktivniho slotu: obraz se hashuje, trailer (podpis +
// delka) se odklada. Vraci true jen kdyz dorazil kazdy bajt v ocekavane delce.
static bool ota_stream(NetworkClient *in, size_t total, size_t imageSize, JvSha256 &sha,
                       uint8_t trailer[JV_OTA_TRAILER]) {
  uint8_t buf[1024];
  size_t got = 0, tgot = 0;
  const uint32_t start = millis();
  uint32_t lastData = start;
  int lastCtvrtina = -1;
  while (got + tgot < total) {
    if (millis() - start > OTA_LIMIT_STAZENI_MS) { LOGE("OTA: stazeni trva prilis dlouho"); return false; }
    const size_t avail = in->available();
    if (avail == 0) {
      if (!in->connected() || millis() - lastData > 20000UL) { return false; }
      vTaskDelay(1);
      continue;
    }
    size_t chci = total - got - tgot;
    if (chci > sizeof(buf)) chci = sizeof(buf);
    if (chci > avail) chci = avail;
    const int n = in->read(buf, chci);
    if (n <= 0) continue;
    lastData = millis();
    size_t off = 0;
    if (got < imageSize) {
      size_t vezmi = imageSize - got;
      if (vezmi > (size_t)n) vezmi = n;
      if (Update.write(buf, vezmi) != vezmi) { LOGE("OTA: zapis do flash selhal: %s", Update.errorString()); return false; }
      sha.add(buf, vezmi);
      got += vezmi;
      off = vezmi;
      const int ctvrtina = (int)((got * 4) / imageSize);
      if (ctvrtina != lastCtvrtina) { lastCtvrtina = ctvrtina; LOGI("OTA: stazeno %u/%u B", (unsigned)got, (unsigned)imageSize); }
    }
    if (off < (size_t)n) {
      memcpy(trailer + tgot, buf + off, n - off);
      tgot += n - off;
    }
  }
  return got == imageSize && tgot == JV_OTA_TRAILER;
}

// Zkontroluje verzi a, je-li novejsi a podpis plati, zapise novy obraz do
// neaktivniho slotu. Restart a stav RTC resi volajici (ota_krok).
static JvOtaVysledek ota_zkontroluj_a_instaluj() {
  const char *typ = JV_OTA_TYP;
  if (ESP.getFreeHeap() < OTA_MIN_HALDA) {
    LOGE("OTA: malo volne haldy (%lu B)", (unsigned long)ESP.getFreeHeap());
    return JV_OTA_CHYBA_STAZENI;
  }
  const String urlVerze = String(JV_OTA_URL_ZAKLAD) + typ + ".version.txt";
  const String urlObraz = String(JV_OTA_URL_ZAKLAD) + typ + ".bin";

  WiFiClientSecure ssl;
  ssl.setInsecure();  // viz hlavicka bloku: integritu drzi podpis, ne transport
  HTTPClient https;
  https.setReuse(false);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(8000);
  https.setTimeout(10000);
  https.setUserAgent(String("JVmeteo/") + FW_VERSION);

  // 1) verze (kratky text; velikost se overuje, aby podvrzeny server nenaplnil RAM)
  if (!https.begin(ssl, urlVerze)) { LOGE("OTA: https.begin(verze) selhal"); return JV_OTA_CHYBA_SITE; }
  const int kodVerze = https.GET();
  String serverVer;
  if (kodVerze == HTTP_CODE_OK) {
    const int n = https.getSize();
    if (n > 0 && n <= 64) { serverVer = https.getString(); serverVer.trim(); }
    else { LOGE("OTA: velikost version.txt %d neni v 1..64", n); }
  } else {
    LOGE("OTA: kontrola verze HTTP %d", kodVerze);
  }
  https.end();
  ssl.stop();
  if (serverVer.length() == 0) { return JV_OTA_CHYBA_SITE; }
  int v3[3];
  if (!jvOtaParseVerze(serverVer.c_str(), v3)) { LOGE("OTA: neplatna verze na serveru"); return JV_OTA_CHYBA_SITE; }
  LOGI("OTA: server %s, lokalni %s", serverVer.c_str(), FW_VERSION);
  if (!jvOtaNovejsi(serverVer.c_str(), FW_VERSION)) { return JV_OTA_AKTUALNI; }

  uint8_t prefix[JV_OTA_PREFIX_CAP];
  const size_t prefixLen = jvOtaPrefix(prefix, sizeof(prefix), typ, serverVer.c_str());
  if (prefixLen == 0) { LOGE("OTA: kontext podpisu je neplatny (typ/verze)"); return JV_OTA_PODPIS_ODMITNUT; }

  // 2) obraz: image || sig[256] || u32le(256)
  if (!https.begin(ssl, urlObraz)) { LOGE("OTA: https.begin(obraz) selhal"); return JV_OTA_CHYBA_STAZENI; }
  const int kodObrazu = https.GET();
  const int total = https.getSize();
  if (kodObrazu != HTTP_CODE_OK || total <= (int)JV_OTA_TRAILER) {
    LOGE("OTA: obraz HTTP %d, velikost %d", kodObrazu, total);
    https.end();
    // Zname delky kratsi nez trailer = nepodepsany obraz, odmitnout driv nez se cokoli zapise.
    return (kodObrazu == HTTP_CODE_OK && total >= 0) ? JV_OTA_PODPIS_ODMITNUT : JV_OTA_CHYBA_STAZENI;
  }
  const size_t imageSize = (size_t)total - JV_OTA_TRAILER;
  if (!Update.begin(imageSize, U_FLASH)) {
    LOGE("OTA: Update.begin selhal: %s", Update.errorString());
    https.end();
    return JV_OTA_CHYBA_STAZENI;
  }
  JvSha256 sha;
  sha.add(prefix, prefixLen);
  uint8_t trailer[JV_OTA_TRAILER];
  const bool kompletni = ota_stream(https.getStreamPtr(), (size_t)total, imageSize, sha, trailer);
  https.end();  // uvolni TLS pred RSA overenim
  if (!kompletni) {
    Update.abort();
    LOGE("OTA: stazeni neuplne, zruseno");
    return JV_OTA_CHYBA_STAZENI;
  }
  uint8_t hash[32];
  sha.finish(hash);
  const uint32_t sigLen = (uint32_t)trailer[JV_OTA_SIG_LEN] | ((uint32_t)trailer[JV_OTA_SIG_LEN + 1] << 8) |
                          ((uint32_t)trailer[JV_OTA_SIG_LEN + 2] << 16) | ((uint32_t)trailer[JV_OTA_SIG_LEN + 3] << 24);
  if (sigLen != JV_OTA_SIG_LEN || !ota_overit_podpis(hash, trailer)) {
    Update.abort();
    LOGE("OTA: PODPIS NEPLATI - obraz neaktivovan, bezi dal stary firmware");
    return JV_OTA_PODPIS_ODMITNUT;
  }
  if (!Update.end()) {
    LOGE("OTA: Update.end selhal: %s", Update.errorString());
    return JV_OTA_CHYBA_STAZENI;
  }
  LOGI("OTA: podpis OK, obraz %s zapsan", serverVer.c_str());
  return JV_OTA_NAINSTALOVANO;
}

// Trigger pres infrastrukturu JVmeteo: ThingSpeak TalkBack. Zarizeni se same
// ptaji (POST .../commands/execute), nic nemusi poslouchat. Prikaz se z fronty
// odebere pri precteni, takze se neopakuje. Plain HTTP stejne jako zapis na
// ThingSpeak - bez dalsi TLS relace, ktera by na baterii stala vic nez samotny trigger.
static bool ota_trigger_pozadan() {
  if (tb_id.length() == 0 || tb_key.length() == 0 || WiFi.status() != WL_CONNECTED) { return false; }
  WiFiClient tbClient;  // 'client' z BLOCK 02 patri knihovne ThingSpeak
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(tbClient, JV_TB_HOST, JV_TB_PORT, String("/talkbacks/") + tb_id + "/commands/execute")) { return false; }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int kod = http.POST(String("api_key=") + tb_key);
  String telo;
  if (kod == HTTP_CODE_OK) {
    const int n = http.getSize();
    if (n <= 64) { telo = http.getString(); }  // -1 = chunked; delka se kontroluje nize
  } else {
    LOGE("OTA: TalkBack HTTP %d", kod);
  }
  http.end();
  return telo.length() <= 64 && jvOtaPrikazJeKontrola(telo.c_str());
}

// Fáze F_OTA: trigger, plan, pokus. Blokuje (TLS + stazeni); mez drzi watchdog.
static void ota_krok() {
  if (ota_trigger_pozadan()) {
    jvOtaZadostTrigger(rtcStav);
    LOGI("OTA: prisel trigger (TalkBack)");
  }
  if (!jvOtaMaKontrolovat(rtcStav, bootTyp, true, ladeni)) { return; }

  LOGI("OTA: kontrola verze (fw %s, trigger=%d, streak=%u)", FW_VERSION, rtcStav.otaPozadavek, rtcStav.otaChybStreak);
  OtaPauza pauza;  // od teď do návratu je ostatní činnost pozastavena
  jvOtaPrijemZahajeni(rtcStav);
  jvSlotyUloz(rtcSloty, rtcStav);  // pokus je zapocitany, i kdyz ho zabije watchdog/brownout
  prodluz_awake_watchdog(OTA_LIMIT_BDENI_MS);
  setCpuFrequencyMhz(160);         // TLS + SHA-256 + RSA behem stazeni

  const JvOtaVysledek v = ota_zkontroluj_a_instaluj();
  jvOtaPoKontrole(rtcStav, v);
  if (v == JV_OTA_NAINSTALOVANO) {
    zastav_awake_watchdog();
    jvSlotyUloz(rtcSloty, rtcStav);  // otaNepotvrzeno = 1: rollback hlida novy obraz
    LOGI("OTA: restart do noveho firmware");
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP.restart();
  }
  setCpuFrequencyMhz(CPU_FREKVENCE_MHZ);
  LOGI("OTA: vysledek=%d streak=%u", (int)v, rtcStav.otaChybStreak);
}

// Novy obraz padl driv, nez se potvrdil: vratit predchozi slot. Vola se z setup()
// hned po nacteni RTC stavu.
static void ota_rollback() {
  LOGE("OTA: novy obraz opakovane padl, vracim predchozi firmware");
  const bool lze = Update.canRollBack();
  jvOtaPoRollbacku(rtcStav, ladeni);
  jvSlotyUloz(rtcSloty, rtcStav);
  if (lze && Update.rollBack()) {
    Serial.flush();
    ESP.restart();
  }
  LOGE("OTA: rollback nelze (predchozi slot neni platny), pokracuji");
}

#endif  // JV_BEZ_OTA

//-----------8-----------

//-----------9-----------
// BLOCK 09: SLEEP (clean shutdown of radio and buses, then timer deep sleep)

void uspani(uint32_t spanekMs, uint32_t bdeniMs) {
  if (uspavaniZahajeno.exchange(true)) return;  // awake watchdog uz uspava
  zastav_awake_watchdog();
  Wire.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  uloz_rtc_stav(bdeniMs + spanekMs);  // cas pro zhaveni MQ pokracuje pres spanek
  esp_sleep_enable_timer_wakeup((uint64_t)spanekMs * 1000ULL);
  if (DEBUG_LEVEL > 0) { Serial.flush(); }
  esp_deep_sleep_start();
}

//-----------9-----------

//-----------10-----------
// BLOCK 10: SETUP / LOOP ORCHESTRATION

// Merici cyklus jako stavovy automat volany z loop(): zadna faze neceka
// v delay(), casovani hlida millis(). Soubezne bezi konfiguracni okno po
// zapnuti a vzorkovani MQ-9. F_OTA (BLOCK 08) je vyjimka: blokuje, ale jen po
// uspesnem odeslani a jen kdyz je kontrola splatna nebo prisel trigger.
enum Faze : uint8_t { F_SENZORY, F_CEKEJ_MQ, F_PLAN, F_WIFI, F_ODESLI, F_OTA, F_KONEC, F_SPANEK };
Faze faze = F_SENZORY;
JvPlan plan = { 0, false };
float cur[JV_POLI];
bool odeslanoOk = false;

void merici_krok() {
  okno_krok();
  switch (faze) {
    case F_SENZORY:
      mq_start();
      cti_vsechny_senzory();
      faze = F_CEKEJ_MQ;
      break;
    case F_CEKEJ_MQ:
      if (mq_krok()) { faze = F_PLAN; }
      break;
    case F_PLAN:
      hodnoty_do_pole(cur);
      plan = jvNaplanuj(rtcStav, bootTyp, cur, komunikace_nastavena(), ladeni);
      if (plan.sit) { wifi_start(); faze = F_WIFI; } else { faze = F_KONEC; }
      break;
    case F_WIFI: {
      const WifiStav w = wifi_krok();
      if (w == WS_OK) { faze = F_ODESLI; }
      else if (w == WS_CHYBA) { jvPoSiti(rtcStav, false, cur, 0, ladeni); faze = F_KONEC; }
      break;
    }
    case F_ODESLI:
      // writeFields() blokuje uvnitr knihovny ThingSpeak (az ~5 s na odpoved).
      // Komunikace zustava puvodni; horni mez bdeni drzi awake watchdog.
      odeslanoOk = odesli_na_thingspeak();
      jvPoSiti(rtcStav, odeslanoOk, cur, odeslanoOk ? (int8_t)WiFi.RSSI() : (int8_t)0, ladeni);
      faze = odeslanoOk ? F_OTA : F_KONEC;
      break;
    case F_OTA:
      cas_krok();  // NTP jen kdyz je potreba (max 1x/24 h); Wi-Fi uz bezi
      ota_krok();  // restartuje, pokud nainstalovala novy firmware
      faze = F_KONEC;
      break;
    case F_KONEC:
      jvKonecCyklu(rtcStav, plan.sit, odeslanoOk, jvMaskaPlatnych(cur) != 0);
      jvOtaPoCyklu(rtcStav, ladeni);  // novy obraz se potvrdi po OTA_POTVRZENI_CYKLU dokoncenych cyklech
      faze = F_SPANEK;
      break;
    case F_SPANEK: {
      if (oknoOtevreno) { break; }  // konfiguracni okno po zapnuti jeste bezi
      const uint32_t bdeni = millis();
      const uint32_t spanek = jvDelkaSpankuMs(rtcStav, bootTyp, bdeni, ladeni);
      LOGI("boot=%s duvody=0x%02X sit=%d ok=%d T=%.2f H=%.1f P=%.1f MQ=%d | bez_odesl=%u chyb_site=%u preskocit=%u bdeni=%lums spanek=%lums",
           nazev_bootu(bootTyp), plan.duvody, plan.sit, odeslanoOk, teplota, vlhkost, tlak, mq9_raw,
           rtcStav.cykluBezOdeslani, rtcStav.sitChybStreak, rtcStav.sitPreskocit,
           (unsigned long)bdeni, (unsigned long)spanek);
      uspani(spanek, bdeni);
      break;
    }
  }
}

void setup() {
  setCpuFrequencyMhz(CPU_FREKVENCE_MHZ);
  Serial.begin(115200);

  bootTyp = klasifikuj_boot();
  nacti_konfiguraci();   // pred nastav_ladeni(): OTA volby jsou v NVS
  nastav_ladeni();
  nacti_rtc_stav();
  LOGI("JVmeteo %s, boot=%s #%lu, ok=%lu chyb=%lu, awake_timeout=%u ota=%s/%dh%s",
       FW_VERSION, nazev_bootu(bootTyp), (unsigned long)rtcStav.pocetBootu,
       (unsigned long)rtcStav.odeslanoOk, (unsigned long)rtcStav.odeslanoChyba,
       rtcStav.awakeTimeouty, ota_povoleno ? "auto" : "rucne", ota_hodin,
       rtcStav.otaNepotvrzeno ? " NEPOTVRZENO" : "");
  // Jednotkova konfigurace je jen v NVS (portal / rucni zapis); flash ani OTA ji neprepisuji.
  LOGI("konfigurace: ssid=%u zn, heslo=%u zn, ts_klic=%u zn, nastavena=%d",
       (unsigned)wifi_ssid.length(), (unsigned)wifi_pass.length(), (unsigned)thingspeak_api.length(),
       (int)komunikace_nastavena());
  cas_nastav_tz();
  cas_zaloguj("RTC");
  if (jvOtaRollbackNutny(rtcStav, ladeni)) { ota_rollback(); }

  // Inicializace ThingSpeak komunikacni vrstvy skrze WiFiClient objekt
  ThingSpeak.begin(client);

  if (jvMamDetekovatKonfiguracniOkno(bootTyp)) {
    rezim_konfigurace = okno_start();
  }

  if (rezim_konfigurace) {
    setCpuFrequencyMhz(160);  // web server + AP, napajeni od uzivatele
    spust_konfiguracni_ap();
  } else {
    spust_awake_watchdog();   // merici cyklus pobezi z loop() pres merici_krok()
  }
}

void loop() {
  if (ota_probiha) { vTaskDelay(1); return; }  // aktualizace bezi: nic jineho nespoustet
  if (rezim_konfigurace) {
    server.handleClient();
    if (restartNaplanovan && millis() - restartOd >= 2000UL) { ESP.restart(); }
    if (millis() > TIMEOUT_KONFIGURACE_MS) { ESP.restart(); }
  } else {
    merici_krok();
  }
  // Predani CPU planovaci FreeRTOS (idle task, usporny WAITI) - nejde o
  // casovani, to drzi millis(). Bez toho by loop() tocil CPU naprazdno.
  vTaskDelay(1);
}

//-----------10-----------
