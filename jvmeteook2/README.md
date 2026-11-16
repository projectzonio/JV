# JVmeteoFW2 – firmware 1.3.0 (low-power + recovery + OTA)

Založeno na `jvmeteook2` (ESP32, ThingSpeak). Low-power a recovery principy
převzaty ze Zonio legacy `weather_sensory/deepsleep/v5_deepsleep_BME280`
(ESP8266) a přepsány pro rodinu ESP32 (C3 / S3 / klasik). Původní firmware je
beze změny v `original_basic/original_basic.ino` (samostatný sketch pro
srovnávací flash).

## Aktivace konfigurace (beze změny)

1. Připojte napájení ESP32.
2. Během prvních **1,2 sekundy** jednou krátce stiskněte a uvolněte tlačítko
   Reset/EN.
3. Po uvolnění tlačítka se zobrazí Wi-Fi síť `JVmeteo_SETUP`.
4. Připojte se heslem z `CONFIG_AP_PASSWORD` a otevřete `http://192.168.4.1`.

Rozdíl proti 1.1: okno 1,2 s se otevírá jen po zapnutí napájení nebo po
stisku EN, ne při každém probuzení ze spánku.

## Co zůstalo stejné

- ThingSpeak přes oficiální knihovnu, pole 1–7 (T, RH, p, ax, ay, az, MQ-9).
- Konfigurační AP, webový formulář, NVS namespace `meteo` a jeho klíče.
- Výchozí piny (I²C SDA 6 / SCL 7, DHT 5, MQ-9 2) a výchozí senzor (BME280).
- Perioda měření 90 s, limit připojení k Wi-Fi 7 s, timeout konfigurace 5 min.
- Bez MQTT a XOR. OTA je od 1.3.0 (sekce Aktualizace přes GitHub níž).

## Co je nové

| Oblast | Změna | Proč |
| --- | --- | --- |
| Odesílání | Wi-Fi jen při změně nad práh proti poslední **úspěšně odeslané** hodnotě, nebo jednou za 20 cyklů (30 min) | Wi-Fi je převážná část spotřeby. `REZIM_ODESILANI_PRI_ZMENE 0` vrátí odesílání v každém cyklu. |
| Konfigurační okno | Jen po zapnutí / EN | Dřív se při každém probuzení čekalo 1,2 s a dvakrát se zapisovalo do NVS (každých 90 s). |
| MPU6050 | Po přečtení se uspí | Jinak během deep sleep odebírá ~3,9 mA, víc než celé ESP32. |
| Wi-Fi | Rychlé připojení na uložený BSSID + kanál (fallback na plný scan), adaptivní TX výkon, CPU 80 MHz | Kratší doba s rádiem. |
| Perioda | Spánek = 90 s − doba bdění | Rytmus se neposouvá. |
| Bez blokování | Měřicí cyklus je stavový automat volaný z `loop()` a časování hlídá `millis()`. Ve sketchi nezůstalo žádné `delay()`. Konfigurační okno 1,2 s běží souběžně s měřením. MQ-9 se vzorkuje (5 vzorků po ≥ 20 ms) mezi I²C senzory. Restart po uložení konfigurace je naplánovaný v `loop()`. | Žádná část FW nečeká naprázdno. Blokovat zůstávají jen knihovny: `ThingSpeak.writeFields` (až ~5 s), `Adafruit_MPU6050::begin` (interní reset ~100 ms) a čtení DHT. |
| Žhavení MQ-9 | Doba od zapnutí napájení se počítá v RTC přes všechny deep sleep cykly. Do 3 min (`MQ_ZHAVENI_MS`, stejně jako legacy `fire_sensor` MQ135) se pole 7 jen neposílá a FW normálně usne. Nové zapnutí počítá od nuly. | Žhavení nesmí blokovat běh FW. Předpoklad: topení je napájené trvale, FW ho neřídí. |
| Ochrana RTC | Dva sloty v `RTC_NOINIT` se sekvenčním číslem, magic, verzí, velikostí a CRC32. Zápis jde vždy do staršího slotu, stav se ukládá hned po bootu a watchdog vychází jen z uloženého snapshotu. | Přerušený zápis nezničí poslední platný stav. Pád uprostřed cyklu se započítá, takže ochrana proti smyčce pádů skutečně sepne (ve verzi bez uložení po bootu nesepnula nikdy). |
| Recovery | Stav v RTC paměti s CRC, backoff po výpadku sítě (pokusy po 1, 2, 4, 8, 16 cyklech, strop 15 cyklů), při brownoutu nezapínat Wi-Fi a spát 10–60 min, po 3 pádech za sebou spát 10 min, awake watchdog 30 s, obnova zaseknuté I²C sběrnice, kontrola rozsahu hodnot | Zařízení na baterii se nesmí vybít v cyklu restartů. |

### Opravené chyby z v5 (nepřeneseny)

- **„Odesílá periodicky, ne přes trigger“:** v5 po odeslání kvůli triggeru
  neaktualizoval baseline, takže každý další vzorek znovu přesáhl práh a
  odesílal se při každém probuzení až do hodinového publish. Tady je baseline
  vždy to, co je skutečně na serveru. Regresní test T03: model v5 pošle 16 z 20
  cyklů, nový FW pošle 2.
- **Restart smyčka:** `checkWakeReason()` ve v5 běžel před `loadRTCData()`, takže
  se `errorCount` nikdy nevynuloval. Po 5 chybách senzoru za celou dobu běhu
  pak každá další chyba znamenala `ESP.restart()` místo spánku.

## Aktualizace přes GitHub (OTA, od 1.3.0)

Zařízení si samo stáhne novou verzi z GitHub Releases a před instalací ověří podpis.
**Zařízení s 1.0 / 1.1 / 1.2-lp OTA nemají: 1.3.0 se nahrává jednou přes USB**
(`jvmeteo-esp32c3-usb-full-<verze>.bin` z Releases, adresa 0x0).
**Pozor:** tento obraz má 4 MB a **maže NVS** (Wi-Fi, klíč, nastavení). Už nakonfigurovanou jednotku aktualizujte
přes `jvmeteo-esp32c3-app-<verze>.bin` na 0x10000 (nebo `arduino-cli upload`); `erase-flash` nepoužívat.
Konfigurace je jen v NVS (portál / ruční zápis) – kompletní postup včetně NTP, pozastavení při OTA a ověření:
[`doc_OTA/README.md`](doc_OTA/README.md).

| Co | Jak |
| --- | --- |
| Kdy se kontroluje | Jen po úspěšném odeslání na ThingSpeak (Wi-Fi už běží). Výchozí 24 h, nastavitelné 1–720 h. Po neúspěchu se interval prodlužuje ×2, ×4, ×8. |
| Nastavení | Portál `JVmeteo_SETUP`, karta *Aktualizace firmware*: zapnout/vypnout automatiku, interval, TalkBack ID + klíč. |
| Co se stahuje | `releases/latest/download/jvmeteo-esp32c3.version.txt` a `.bin`. „Latest" je nejnovější release, který **není** prerelease; tag `v1.4.0-rc1` zařízení nevidí. |
| Bezpečnost | `image ‖ podpis RSA-2048 ‖ délka`. Podepsaná zpráva obsahuje čip i verzi: obraz pro jiný čip ani starší podepsaný obraz neprojde. Transport je TLS bez ověření certifikátu, o aktivaci rozhoduje jen podpis (klíč ve firmware: `jv_ota_pubkey.h`). Starší nebo stejná verze se nikdy neinstaluje. |
| Pojistky | Nekontroluje se po brownoutu ani po pádech, nekontroluje se dokud se předchozí aktualizace nepotvrdila, stahování má strop 150 s. |
| Rollback | Nový obraz je „na zkoušku", dokud nedokončí 3 měřicí cykly. Spadne-li 3× po sobě dřív, vrátí se předchozí slot a ~8× delší interval chrání před okamžitým novým stažením. |
| Vzdálená kontrola | ThingSpeak **TalkBack**: příkaz `OTA_CHECK` ve frontě spustí kontrolu při nejbližším odeslání (nejpozději do heartbeatu, ~30 min), i když je automatika vypnutá. Příkaz nevybírá URL ani verzi, jen spustí tutéž kontrolu jako časovač. Minimální rozestup 10 min. |

### Vydání nové verze

1. Zvýšit `FW_VERSION` v `jvmeteook2.ino` (semver, např. `1.3.1`).
2. `git tag v1.3.1 && git push origin v1.3.1`. Workflow `.github/workflows/firmware.yml`
   spustí host test, přeloží, **podepíše** a publikuje release. Tag musí odpovídat `FW_VERSION`.
3. Soukromý klíč je jen v GitHubu jako secret `OTA_SIGNING_KEY` (nikdy v repu): buď v environmentu
   `release` (doporučeno, jde zapnout schvalování, ať tag sám klíč nepoužije), nebo jako repozitářový secret.
   Workflow podpis ověří proti klíči zabudovanému ve firmware; nesedí-li, release nevznikne a log řekne proč.
4. Výměna klíče = nový USB flash všech zařízení (stará zařízení jiný klíč nepřijmou).

### Vlastní GitHub účet (fork, kopie, převedený repozitář)

JVmeteo OTA nezávisí na tom, kdo firmware staví. Zdroj aktualizací je `JV_OTA_REPO` v `jv_ota_config.h`
a workflow ho před překladem přepíše na `github.repository`, takže build pod jakýmkoli účtem stahuje
jen z releasů toho účtu. Co musí udělat majitel nového repozitáře (jednou):

1. Repozitář musí být **veřejný** (zařízení stahuje bez přihlášení). Na forku povolit *Actions*.
2. `python jvmeteook2/tools/ota/sign_ota.py keygen --out-key private.pem --pub-header jvmeteook2/jv_ota_pubkey.h`
   vyrobí vlastní dvojici klíčů (RSA-2048). Veřejný klíč commitnout, `private.pem` uložit mimo repozitář a zálohovat.
3. Obsah `private.pem` vložit jako secret `OTA_SIGNING_KEY` (*Settings → Secrets and variables → Actions*,
   nebo environment `release`). `gh secret set OTA_SIGNING_KEY < private.pem` dělá totéž.
4. Zařízení se zabudovaným cizím klíčem vlastní releasy nepřijmou (a naopak) — první firmware z nového účtu se
   nahraje přes USB (`jvmeteo-esp32c3-usb-full-<verze>.bin`).

Chybí-li bod 2–3, workflow release nevydá a v logu uvede příkaz, který chybí. Lokální překlad (Arduino IDE)
bere repozitář z `jv_ota_config.h` — při vlastním forku tam přepište `JV_OTA_REPO`.
GitHub Enterprise Server se nepodporuje (adresa je pevně `https://github.com/`).

Lokální test proti vlastnímu serveru: soubor `jv_ota_local.h` vedle sketche (v `.gitignore`)
a v něm `#define JV_OTA_URL_ZAKLAD "https://<ip>:<port>/"`, `JV_TB_HOST`, `JV_TB_PORT`.

**Neověřeno na hardwaru** (stav k 2026-11-16): celý tok na desce, přesměrování GitHub → CDN
v `HTTPClient`, odpověď skutečného ThingSpeak TalkBack, rollback na desce. Hotovo a změřeno:
logika (host test), formát podpisu zařízení ↔ podepisovač, překlad (C3: 92 % flash).

## Prahy a ladění (BLOCK 01)

`PRAH_TEPLOTA_C 0.5`, `PRAH_VLHKOST_PCT 3`, `PRAH_TLAK_HPA 1`,
`PRAH_ZRYCHLENI_MS2 0.5`, `PRAH_MQ9_RAW 50`, `HEARTBEAT_KAZDYCH_N_CYKLU 20`.
v5 používal 1,5 °C / 10 % / 1,5 hPa. To šetří víc baterie, ale graf na
ThingSpeaku je pak schodovitý.

## Struktura (border marks `//-----------N-----------`)

| Blok | Obsah | Závisí na |
| --- | --- | --- |
| 01 | include, verze, ladicí konstanty | – |
| 02 | uživatelská konfigurace, piny, globální hodnoty (původní) | 01 |
| 03 | **power logic core**: rozhodnutí o odeslání, backoff, délka spánku, CRC (čisté C++) | **nic** |
| 04 | RTC stav, klasifikace bootu, awake watchdog | 03 |
| 05 | senzory, obnova I²C | 02 |
| 06 | Wi-Fi a ThingSpeak | 02, 03, 04 |
| 07 | konfigurační portál a NVS | 02, 04 |
| 08 | **OTA**: GitHub Releases, ověření podpisu, rollback, trigger přes ThingSpeak TalkBack | 02, 03, 04 |
| 09 | uspání | 04 |
| 10 | `setup()` / `loop()` | všechny |

Blok 03 nemá žádnou Arduino ani IDF závislost. Proto se dá testovat nativně
a je připravený pro FW twin.

## Ověření

```
python test/host/run_host_test.py
```

Skript vyřízne BLOCK 03 přímo ze sketche, přeloží ho (MSVC, jinak g++) a spustí
125 kontrol (T01–T23, z toho OTA T16–T22 a čas/NTP T23). Pak spustí deset mutací a ověří, že na
každé test spadne: vrácení opravy v5, vypnutí ukládání RTC po bootu a šest OTA
(stejná verze = novější, downgrade, rollback bez příznaku, nikdy nepotvrzeno, bez
započtení pokusu, bez pojistek). Navíc porovná prefix podpisu, který počítá
zařízení, bajt po bajtu s `tools/ota/sign_ota.py`. Exit 0 = PASS, 1 = FAIL,
2 = INVALID RUN (chybí border marks, překlad selhal nebo mutace nenašla svůj
řádek).

Stav k 2026-11-15:

- Host test: PASS 46/46, oba mutanty spadly (T03, resp. T14).
- `arduino-cli` překlad: ESP32-C3 (`nologo_esp32c3_super_mini`) 79 % flash,
  ESP32-S3 76 %, 0 varování ve sketchi. Core esp32 3.2.1, ThingSpeak 2.1.1,
  Adafruit MPU6050 2.2.9, DHT 1.4.7.
- **Neověřeno na hardwaru:** spotřeba, rychlé připojení, chování EN resetu na
  konkrétní desce.

## Na co pozor u hardwaru

- **MQ-9** má topení ~150 mA. Pokud je napájené trvale, provoz na baterii
  nedává smysl bez ohledu na firmware. Měření bez předehřátí je navíc jen
  orientační.
- **LED napájení a LDO na vývojové desce** mohou v deep sleep odebírat víc než
  samotné ESP32 (jednotky mA). U provozu na baterii LED odpájejte.
- Piny 6/7 pro I²C odpovídají ESP32-C3/S3. Na klasickém ESP32 jsou to piny
  flash paměti.
