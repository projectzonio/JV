#pragma once
// Odkud si zarizeni bere aktualizace: "<majitel>/<repozitar>" na github.com.
//
// JVmeteo OTA je nezavisle na tom, kdo firmware stavi: kazdy GitHub ucet (fork,
// kopie, preneseny repozitar) vydava a podepisuje SVOJE verze a zarizeni z jeho
// buildu se aktualizuji jen od nej.
//   - GitHub Actions (.github/workflows/firmware.yml) tenhle soubor pred prekladem
//     PREPISE hodnotou ${{ github.repository }}, takze CI buildy nic nastavovat nemusi.
//   - Lokalni preklad (Arduino IDE / arduino-cli) bere hodnotu odsud: pri vlastnim
//     forku ji prepiste. Pro lokalni testy proti vlastnimu serveru viz jv_ota_local.h.
// Podpis (jv_ota_pubkey.h) se musi shodovat s klicem, kterym se v CI podepisuje:
// vlastni dvojici vyrobi `python tools/ota/sign_ota.py keygen`.
// Repozitar musi byt verejny: zarizeni stahuje bez prihlaseni.
#define JV_OTA_REPO "projectzonio/JV"
