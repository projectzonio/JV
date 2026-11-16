# JVmeteoFW2 – firmware 1.2-lp (low-power + recovery)

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
- Bez OTA, MQTT a XOR.

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
| 08 | uspání | 04 |
| 09 | `setup()` / `loop()` | všechny |

Blok 03 nemá žádnou Arduino ani IDF závislost. Proto se dá testovat nativně
a je připravený pro FW twin.

## Ověření

```
python test/host/run_host_test.py
```

Skript vyřízne BLOCK 03 přímo ze sketche, přeloží ho MSVC a spustí 46 kontrol.
Pak spustí dvě mutace a ověří, že na každé test spadne: (1) vrácení opravy
v5 (baseline) a (2) vypnutí ukládání RTC po bootu. Exit 0 = PASS, 1 = FAIL,
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
