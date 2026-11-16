# JVmeteo – OTA přes GitHub Releases: kompletní postup

Stav: ověřeno na desce 2026-11-16 (ESP32-C3, firmware 1.3.0 → 1.3.1 z GitHub Actions: stažení,
podpis, instalace, restart, potvrzení po 3 cyklech). Co ověřeno **není**, je v posledním oddílu.

JVmeteo je samostatný projekt. OTA nezávisí na Zonio ani na jednom konkrétním GitHub účtu:
každý, kdo repozitář forkne nebo zkopíruje, vydává a podepisuje **své** verze a jeho zařízení
se aktualizují jen od něj.

## 1. Jak to funguje

```
git tag vX.Y.Z  ->  GitHub Actions  ->  Release (podepsaný obraz)  ->  zařízení ho stáhne
                    test -> build -> release                          a ověří podpis
```

| Co | Detail |
| --- | --- |
| Zdroj | `https://github.com/<majitel>/<repo>/releases/latest/download/jvmeteo-esp32c3.{version.txt,bin}` |
| „latest“ | nejnovější release, který **není** prerelease. Tag `v1.4.0-rc1` je prerelease → zařízení ho nevidí (řízený rollout) |
| `<majitel>/<repo>` | `JV_OTA_REPO` v `jv_ota_config.h`; workflow ho před překladem přepíše na `github.repository`, takže build pod jakýmkoli účtem míří sám na sebe |
| Obraz | `image ‖ podpis RSA-2048 (256 B) ‖ uint32le(256)` |
| Podpis | RSA-2048 PKCS#1 v1.5 nad SHA-256(`JVMETEO-OTA-SIG-v1\0 ‖ typ ‖ \0 ‖ verze ‖ \0 ‖ image`). Typ (čip) i verze jsou v podepsané zprávě: obraz pro jiný čip ani starší podepsaný obraz neprojde |
| Klíč | veřejný ve firmware (`jv_ota_pubkey.h`), soukromý jen v GitHub secretu `OTA_SIGNING_KEY` |
| TLS | `setInsecure()` – transport nedokazuje, kdo obraz poslal; o aktivaci rozhoduje výhradně podpis, ověřený mezi posledním zápisem a `Update.end()`. Proto OTA **nepotřebuje správný čas** |
| Rollback | nový obraz je „na zkoušku“ 3 dokončené cykly; 3 pády dřív → předchozí slot |

## 2. Jednorázová příprava (nový GitHub účet)

1. Repozitář musí být **veřejný** (zařízení stahuje bez přihlášení). Na forku povolit *Actions*.
2. Vlastní klíč: `python jvmeteook2/tools/ota/sign_ota.py keygen --out-key private.pem --pub-header jvmeteook2/jv_ota_pubkey.h`
   (potřebuje `openssl`; `OPENSSL=cesta` když není v PATH). Veřejný klíč commitnout. `private.pem` uložit
   **mimo repozitář** a zálohovat – bez něj nejde vydat další verzi a výměna klíče = nový USB flash všech zařízení.
3. Secret `OTA_SIGNING_KEY` = celý obsah `private.pem` včetně řádků BEGIN/END
   (*Settings → Secrets and variables → Actions*, nebo environment `release` – doporučeno, jde zapnout schvalování).
4. Kontrola: `python jvmeteook2/tools/ota/sign_ota.py keycheck --key private.pem --pub-header jvmeteook2/jv_ota_pubkey.h` → `OK`.

Soukromý klíč nikdy do repa, chatu ani logu.

## 3. Vydání nové verze

1. Zvýšit `FW_VERSION` v `jvmeteook2.ino` (semver `X.Y.Z`). Tag **musí** odpovídat (`v1.3.2` ↔ `"1.3.2"`), jinak build selže.
2. `python jvmeteook2/test/host/run_host_test.py` → exit 0 (logika + mutace + tok „cizí účet“).
3. `git tag -a vX.Y.Z -m "..." && git push origin main vX.Y.Z`.
4. Workflow `firmware.yml`:
   - `test` – host test; `build` – překlad ESP32-C3 **bez tajemství**, knihovny pinnuté;
   - `release` – `keycheck` (secret ↔ klíč ve firmware), podpis, `verify` proti klíči **z firmware**, negativní kontrola
     (jiná verze musí selhat), koncept release s přílohami, teprve potom zveřejnění.
5. Příloha release: `jvmeteo-esp32c3.bin` (podepsaný, pro OTA), `jvmeteo-esp32c3.version.txt`, `SHA256SUMS`,
   `jvmeteo-esp32c3-app-<ver>.bin` (nepodepsaná aplikace, pro USB bez ztráty konfigurace), `jvmeteo-esp32c3-usb-full-<ver>.bin`
   (celých 4 MB, **maže NVS**, jen pro novou desku).
6. Zkušební verze: tag `vX.Y.Z-rc1` → prerelease, zařízení ho nevidí.

## 4. Konfigurace jednotky je v NVS a přežije flash i OTA

Release binárka je pro všechny jednotky stejná, proto **nic jednotkového není v kódu**: Wi-Fi (`ssid`, `pass`),
ThingSpeak write klíč (`ts`), TalkBack (`tb_id`, `tb_key`), OTA volby (`en_ota`, `ota_hod`), senzory a piny jsou v NVS
(namespace `meteo`). Číslo kanálu ThingSpeak se k zápisu nepoužívá (knihovna posílá jen API klíč).
Změna konfigurace = **jen webový portál** `JVmeteo_SETUP` (EN do 1,2 s po zapnutí) **nebo ručně** zápisem NVS; firmware ani OTA
konfiguraci nepřepíše.

| Operace | NVS |
| --- | --- |
| OTA | zachována |
| `arduino-cli upload` / `esptool write-flash 0x10000 <app>` | zachována |
| `esptool erase-flash` / `arduino-cli upload --erase`… | **smazána** – nepoužívat |
| `jvmeteo-esp32c3-usb-full-<ver>.bin` na 0x0 | **smazána** (obraz má 0xFF přes celých 4 MB) |

Ruční zápis NVS bez portálu (bench):
```
pip install --user esp-idf-nvs-partition-gen
# nvs.csv:   key,type,encoding,value / meteo,namespace,, / ssid,data,string,<ssid> / pass,data,string,<heslo> / ts,data,string,<write key>
python -m esp_idf_nvs_partition_gen generate nvs.csv nvs.bin 0x5000
esptool --chip esp32c3 --port COMx write-flash 0x9000 nvs.bin      # smazat nvs.csv a nvs.bin, obsahují heslo
```

## 5. Co dělá zařízení

- OTA kontrola běží **jen po úspěšném odeslání na ThingSpeak** (Wi-Fi už běží, žádná samostatná relace). Výchozí interval 24 h
  (1–720 h v portálu); po neúspěchu ×2/×4/×8. Pokus se zapíše **před** zahájením (zaseknutý pokus má backoff).
- Nekontroluje se po brownoutu, po pádech a dokud se předchozí aktualizace nepotvrdila.
- **Ruční trigger:** ThingSpeak TalkBack – příkaz `OTA_CHECK` ve frontě spustí tutéž kontrolu při nejbližším odeslání, i při vypnuté
  automatice (min. rozestup 10 min). Příkaz nevybírá URL ani verzi. TalkBack ID + klíč v portálu.
- **Pozastavení při aktualizaci:** po rozhodnutí o kontrole se až do návratu pozastaví vše ostatní – neměří se, neodesílá, nebeží portál,
  `loop()` nic nespouští a Wi-Fi nesmí uspávat rádio (`WiFi.setSleep(false)`). Stahování má strop 150 s a hlídá ho awake watchdog.
  Konec (i chybový) vždy obnoví činnost (RAII), úspěšná instalace restartuje.
- **Čas (NTP):** JVmeteo nemá zálohované hodiny, po vypadnutí napájení je čas 1970. Po úspěšném odeslání se nejvýš 1× za 24 h
  (a vždy, když je čas neplatný) synchronizuje `pool.ntp.org` / `time.nist.gov` (strop 4 s, pásmo CET/CEST). Hodiny přežijí
  deep sleep. Čas se vypisuje do logu, OTA ho nepotřebuje (viz TLS výše).

## 6. První nasazení jednotky a ověření na seriálu

1. Nová deska: `…-usb-full-<ver>.bin` na 0x0 (nebo `arduino-cli upload`), potom konfigurace v portálu / zápisem NVS (oddíl 4).
2. Serial: u CH340 můstku otevření portu desku resetuje → log sbírat jednou otevřeným `tools/hw/serial-logger.js --port COMx`.
   Deska většinu času spí (cyklus 90 s), probuzení vypíše `JVmeteo <verze>, boot=…`.
3. Očekávané řádky při aktualizaci:
```
Data uspesne zapsana na ThingSpeak ...
Cas: 2026-.. ..:..:.. (NTP)
OTA: kontrola verze (fw 1.3.1, trigger=0, streak=N)
OTA: cinnost pozastavena (mereni, odesilani, portal)
OTA: server 1.3.2, lokalni 1.3.1
OTA: stazeno 1210848/1210848 B
OTA: podpis OK, obraz 1.3.2 zapsan
OTA: restart do noveho firmware
JVmeteo 1.3.2, boot=soft ... NEPOTVRZENO        (3 cykly)
JVmeteo 1.3.2, boot=deepsleep ...                (bez NEPOTVRZENO = potvrzeno)
```
4. Zkrácení interval/heartbeat pro bench: jen v **kopii sketche mimo repo** (OTA interval `jvOtaIntervalMs`, `HEARTBEAT_KAZDYCH_N_CYKLU`).
   Nikdy nekomitovat.

## 7. Selhání a co znamenají

| Log | Význam |
| --- | --- |
| `kontrola verze HTTP 404` | release ještě neexistuje / neexistuje ne-prerelease (TLS ke GitHubu funguje) |
| `velikost version.txt -1` | CDN poslal chunked; uvolnit kontrolu `getSize()` (zatím nenastalo) |
| `PODPIS NEPLATI` | obraz podepsán jiným klíčem nebo pro jinou verzi/čip; nic se neaktivovalo |
| `Chybi secret OTA_SIGNING_KEY` (CI) | secret není ani repozitářový, ani v environmentu `release` |
| `secret OTA_SIGNING_KEY nepatri k verejnemu klici ve firmware` (CI) | fork bez vlastního klíče: `keygen`, commit hlavičky, nový secret |
| `kanal=… nastavena=0` / `sit=0` při `duvody≠0` | chybí ssid / heslo / ts klíč v NVS |

## 8. Neověřeno (stav 2026-11-16)

- rollback na desce; negativní testy na desce (zlý podpis, převrácený bit, jiná verze) – ověřeno jen na hostu a v podepisovači;
- ruční trigger TalkBack proti skutečnému ThingSpeaku (fake server ho zatím neumí);
- **NTP:** host test hlídá jen rozhodování (BLOCK 03, T23). Na desce **1.3.2 selhalo**: `NTP: bez odpovedi do 4000 ms` při každém odeslání
  (zbytečné 4 s radia), přestože se čas nastavil (`Cas: … (RTC)` po restartu byl správný) – příčina: `sntp_get_sync_status()` nikdy nedosáhlo
  `COMPLETED`. 1.3.3 ověřuje úspěch notifikací jádra (`sntp_set_time_sync_notification_cb`); **na desce zatím neověřeno** (hledat `NTP: synchronizovano za … ms`);
- pozastavení při OTA: na desce ověřeno jen logem `cinnost pozastavena` před stažením (1.3.2); `cinnost obnovena` se vypíše jen při neúspěšné/aktuální kontrole;
- spotřeba OTA kontroly a NTP na baterii; GitHub Enterprise Server se nepodporuje (adresa je pevně `github.com`).
