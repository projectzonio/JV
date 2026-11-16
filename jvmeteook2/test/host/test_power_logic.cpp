// Host test BLOCK 03 (power logic core) z jvmeteook2.ino.
// Blok se nekopiruje: run_host_test.py ho vyrizne podle border marks do
// build/block03.inc, takze test vzdy bezi nad tim, co je ve sketchi.
//
// Co test umi shodit (overeno mutaci v run_host_test.py):
//  - vraceni opravy v5 (baseline se po uspesnem odeslani neaktualizuje)
//    -> T03 "skok drzi 15 cyklu" posle 16x misto 2x.
// Validity: T03 si nejdriv na referencnim modelu v5 overi, ze scenar
// v5 chybu opravdu vyvola; jinak hlasi INVALID RUN, ne PASS.

#include <stdio.h>
#include <stdlib.h>
#include "block03.inc"

static int g_fail = 0, g_pass = 0, g_invalid = 0;
#define CHECK(name, cond) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)

static JvLadeni ladeni_default() {
  JvLadeni t;
  const float p[JV_POLI] = {0.5f, 3.0f, 1.0f, 0.5f, 0.5f, 0.5f, 50.0f};
  memcpy(t.prah, p, sizeof(p));
  t.heartbeatCyklu = 20;
  t.maxPreskocit = 15;
  t.rezimPriZmene = true;
  t.intervalMs = 90000;
  t.brownoutSpanekMs = 600000;
  t.padSpanekMs = 600000;
  t.limitPadu = 3;
  t.otaPovoleno = true;
  t.otaIntervalMs = 24UL * 3600000UL;
  t.otaMaxBackoff = 3;
  t.otaPotvrzeniCyklu = 3;
  t.otaMinTriggerMs = 600000UL;
  return t;
}

static void jen_bme(float out[JV_POLI], float t, float h, float p) {
  for (int i = 0; i < JV_POLI; i++) out[i] = NAN;
  out[0] = t; out[1] = h; out[2] = p;
}

// Jeden merici cyklus tak, jak ho dela BLOCK 09. Vraci 1 = odeslano.
static int cyklus(JvRtcState &s, JvBoot boot, const float cur[JV_POLI], bool sitFunguje,
                  const JvLadeni &t, bool *zkouselSit = nullptr) {
  jvPoBootu(s, boot);
  JvPlan plan = jvNaplanuj(s, boot, cur, true, t);
  bool ok = false;
  if (plan.sit) { ok = sitFunguje; jvPoSiti(s, ok, cur, ok ? -60 : 0, t); }
  jvKonecCyklu(s, plan.sit, ok, jvMaskaPlatnych(cur) != 0);
  if (zkouselSit) *zkouselSit = plan.sit;
  return ok ? 1 : 0;
}

// Referencni model rozhodnuti v5_deepsleep_BME280 (runMeasurementCycle):
// trigger proti baseline, baseline se po trigger publishi NEaktualizuje,
// periodicky publish kazdych 12 vzorku baseline aktualizuje.
struct V5Model { bool baselineValid = false; float bT = 0, bH = 0, bP = 0; uint32_t n = 0; };
static int v5_cyklus(V5Model &m, float T, float H, float P) {
  m.n++;
  bool trig = !m.baselineValid || fabsf(T - m.bT) >= 1.5f || fabsf(H - m.bH) >= 10.0f || fabsf(P - m.bP) >= 1.5f;
  bool cas = (m.n % 12) == 0;
  if (!(trig || cas)) return 0;
  bool force = !m.baselineValid;
  if (!trig || force) { m.bT = T; m.bH = H; m.bP = P; m.baselineValid = true; }
  return 1;
}

int main() {
  const JvLadeni t = ladeni_default();
  float cur[JV_POLI];

  // T01 RTC integrita
  {
    JvRtcState s; jvStavInit(s); jvStavZapecetit(s);
    CHECK("T01 zapeceteny stav je platny", jvStavPlatny(s));
    s.pocetVzorku ^= 1u;
    CHECK("T01 zmeneny bajt -> neplatny", !jvStavPlatny(s));
    JvRtcState g; memset(&g, 0xA5, sizeof(g));
    CHECK("T01 nahodna RTC pamet po zapnuti -> neplatna", !jvStavPlatny(g));
    JvRtcState z; memset(&z, 0, sizeof(z));
    CHECK("T01 nulova pamet -> neplatna", !jvStavPlatny(z));
    CHECK("T01 CRC32 test vektor '123456789'", jvCrc32((const uint8_t *)"123456789", 9) == 0xCBF43926UL);
  }

  // T02 prvni cyklus a nenastavena komunikace
  {
    JvRtcState s; jvStavInit(s);
    jen_bme(cur, 21.0f, 50.0f, 1000.0f);
    JvPlan p = jvNaplanuj(s, JV_BOOT_COLD, cur, true, t);
    CHECK("T02 prvni cyklus posila (PRVNI)", p.sit && (p.duvody & JV_DUVOD_PRVNI));
    p = jvNaplanuj(s, JV_BOOT_COLD, cur, false, t);
    CHECK("T02 sablona bez ThingSpeak kanalu -> Wi-Fi se nezapina", !p.sit);
    float prazdne[JV_POLI]; for (int i = 0; i < JV_POLI; i++) prazdne[i] = NAN;
    p = jvNaplanuj(s, JV_BOOT_COLD, prazdne, true, t);
    CHECK("T02 zadna platna hodnota -> nic neposilat", !p.sit && p.duvody == 0);
  }

  // T03 regresni test chyby v5: skok teploty drzeny 15 cyklu
  {
    const int pred = 5, po = 15;
    V5Model m; int v5 = 0;
    for (int i = 0; i < pred; i++) v5 += v5_cyklus(m, 20.0f, 50.0f, 1000.0f);
    for (int i = 0; i < po; i++) v5 += v5_cyklus(m, 22.0f, 50.0f, 1000.0f);
    // Validity gate: scenar musi chybu v5 vyvolat (odeslani skoro kazdy cyklus po skoku).
    if (v5 < po) {
      g_invalid++;
      printf("INVALID RUN T03: referencni model v5 poslal jen %d zprav, scenar chybu nevyvolal\n", v5);
    } else {
      JvRtcState s; jvStavInit(s); int nove = 0;
      for (int i = 0; i < pred; i++) { jen_bme(cur, 20.0f, 50.0f, 1000.0f); nove += cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t); }
      for (int i = 0; i < po; i++) { jen_bme(cur, 22.0f, 50.0f, 1000.0f); nove += cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t); }
      printf("info T03: v5 model odeslal %d/%d, novy FW %d/%d\n", v5, pred + po, nove, pred + po);
      CHECK("T03 skok drzeny 15 cyklu -> presne 2 odeslani (prvni + skok)", nove == 2);
    }
  }

  // T04 heartbeat a sum pod prahem
  {
    JvRtcState s; jvStavInit(s); int n = 0;
    for (int i = 0; i < 45; i++) {
      jen_bme(cur, 20.0f + ((i & 1) ? 0.2f : -0.2f), 50.0f + ((i & 1) ? 1.0f : -1.0f), 1000.0f);
      n += cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t);
    }
    CHECK("T04 45 cyklu se sumem pod prahem -> 3 odeslani (cyklus 1, 21, 41)", n == 3);
  }

  // T05 vypadek pole netriggeruje, nove pole ano
  {
    JvRtcState s; jvStavInit(s);
    jen_bme(cur, 20.0f, 50.0f, 1000.0f);
    cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t);
    cur[1] = NAN;
    CHECK("T05 vypadek vlhkosti -> zadny duvod", jvDuvodyOdeslani(s, cur, t) == 0);
    jen_bme(cur, 20.0f, 50.0f, 1000.0f); cur[6] = 300.0f;
    CHECK("T05 nove pole MQ-9 -> NOVE_POLE", (jvDuvodyOdeslani(s, cur, t) & JV_DUVOD_NOVE_POLE) != 0);
  }

  // T06 backoff site + negativni kontrola + zotaveni
  {
    JvRtcState s; jvStavInit(s); int pokusy = 0; bool z;
    for (int i = 0; i < 40; i++) {
      jen_bme(cur, 20.0f + (float)i, 50.0f, 1000.0f);  // kazdy cyklus duvod poslat
      cyklus(s, JV_BOOT_DEEPSLEEP, cur, false, t, &z); pokusy += z;
    }
    CHECK("T06 40 cyklu bez site -> 6 pokusu (1,2,4,8,16,32)", pokusy == 6);

    JvLadeni bez = t; bez.maxPreskocit = 0;  // negativni kontrola: backoff vypnut
    JvRtcState s2; jvStavInit(s2); int pokusy2 = 0;
    for (int i = 0; i < 40; i++) {
      jen_bme(cur, 20.0f + (float)i, 50.0f, 1000.0f);
      cyklus(s2, JV_BOOT_DEEPSLEEP, cur, false, bez, &z); pokusy2 += z;
    }
    CHECK("T06 negativni kontrola: bez backoffu 40 pokusu", pokusy2 == 40);

    // Sit se vrati: pending zmena se odesle pri nejblizsim povolenem pokusu i bez dalsi zmeny.
    int odeslano = 0, cyklu = 0;
    while (odeslano == 0 && cyklu < 20) {
      jen_bme(cur, 59.0f, 50.0f, 1000.0f);
      odeslano = cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t); cyklu++;
    }
    CHECK("T06 po obnoveni site se pending zmena odesle do 16 cyklu", odeslano == 1 && cyklu <= 16);
    CHECK("T06 uspech nuluje backoff", s.sitChybStreak == 0 && s.sitPreskocit == 0);
  }

  // T07 brownout: zadna Wi-Fi, eskalace spanku, reset po uspechu
  {
    JvRtcState s; jvStavInit(s);
    jen_bme(cur, 20.0f, 50.0f, 1000.0f);
    jvPoBootu(s, JV_BOOT_BROWNOUT);
    JvPlan p = jvNaplanuj(s, JV_BOOT_BROWNOUT, cur, true, t);
    CHECK("T07 brownout boot -> Wi-Fi vypnuta i pri duvodu", !p.sit && p.duvody != 0);
    CHECK("T07 1. brownout -> spanek 10 min", jvDelkaSpankuMs(s, JV_BOOT_BROWNOUT, 0, t) == 600000UL);
    jvPoBootu(s, JV_BOOT_BROWNOUT);
    CHECK("T07 2. brownout -> spanek 20 min", jvDelkaSpankuMs(s, JV_BOOT_BROWNOUT, 0, t) == 1200000UL);
    for (int i = 0; i < 10; i++) jvPoBootu(s, JV_BOOT_BROWNOUT);
    CHECK("T07 strop 6x", jvDelkaSpankuMs(s, JV_BOOT_BROWNOUT, 0, t) == 3600000UL);
    cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, t);
    CHECK("T07 uspesne odeslani nuluje brownout streak", s.brownoutStreak == 0);
  }

  // T08 pady: po 3 za sebou dlouhy spanek, radny deep sleep wake nuluje
  {
    JvRtcState s; jvStavInit(s);
    jvPoBootu(s, JV_BOOT_CRASH); jvPoBootu(s, JV_BOOT_CRASH);
    CHECK("T08 2 pady -> normalni perioda", jvDelkaSpankuMs(s, JV_BOOT_CRASH, 0, t) == 90000UL);
    jvPoBootu(s, JV_BOOT_CRASH);
    CHECK("T08 3 pady -> 10 min", jvDelkaSpankuMs(s, JV_BOOT_CRASH, 0, t) == 600000UL);
    jvPoBootu(s, JV_BOOT_DEEPSLEEP);
    CHECK("T08 deep sleep wake nuluje pady", s.padStreak == 0);
  }

  // T09 kompenzace doby bdeni
  {
    JvRtcState s; jvStavInit(s);
    CHECK("T09 bdeni 2,5 s -> spanek 87,5 s", jvDelkaSpankuMs(s, JV_BOOT_DEEPSLEEP, 2500, t) == 87500UL);
    CHECK("T09 bdeni delsi nez perioda -> 1 s", jvDelkaSpankuMs(s, JV_BOOT_DEEPSLEEP, 95000, t) == 1000UL);
  }

  // T10 puvodni rezim (kazdy cyklus)
  {
    JvLadeni puv = t; puv.rezimPriZmene = false;
    JvRtcState s; jvStavInit(s); int n = 0;
    for (int i = 0; i < 10; i++) { jen_bme(cur, 20.0f, 50.0f, 1000.0f); n += cyklus(s, JV_BOOT_DEEPSLEEP, cur, true, puv); }
    CHECK("T10 REZIM_ODESILANI_PRI_ZMENE=0 -> 10/10", n == 10);
  }

  // T11 awake timeout = selhani site
  {
    JvRtcState s; jvStavInit(s);
    jvPoAwakeTimeoutu(s, t); jvPoAwakeTimeoutu(s, t);
    CHECK("T11 awake timeout zvysuje streak a backoff", s.awakeTimeouty == 2 && s.sitChybStreak == 2 && s.sitPreskocit == 1);
  }

  // T12 TX uroven a konfiguracni okno
  {
    CHECK("T12 RSSI nezname -> plny vykon", jvTxUroven(0) == 0);
    CHECK("T12 RSSI -50 -> nizky", jvTxUroven(-50) == 2);
    CHECK("T12 RSSI -60 -> stredni", jvTxUroven(-60) == 1);
    CHECK("T12 RSSI -80 -> plny", jvTxUroven(-80) == 0);
    CHECK("T12 okno jen po zapnuti", jvMamDetekovatKonfiguracniOkno(JV_BOOT_COLD) &&
          !jvMamDetekovatKonfiguracniOkno(JV_BOOT_DEEPSLEEP) && !jvMamDetekovatKonfiguracniOkno(JV_BOOT_SOFT) &&
          !jvMamDetekovatKonfiguracniOkno(JV_BOOT_BROWNOUT) && !jvMamDetekovatKonfiguracniOkno(JV_BOOT_CRASH));
  }

  // T13 ochrana RTC: dva sloty, preruseny zapis, pretoceni sekvence
  {
    JvRtcState sloty[2]; memset(sloty, 0, sizeof(sloty));
    JvRtcState s; jvStavInit(s);
    s.pocetVzorku = 1; jvSlotyUloz(sloty, s);
    s.pocetVzorku = 2; jvSlotyUloz(sloty, s);
    JvRtcState r;
    CHECK("T13 nacte nejnovejsi slot", jvSlotyNacti(sloty, r) && r.pocetVzorku == 2);
    const int novy = (sloty[0].pocetVzorku == 2) ? 0 : 1;
    ((uint8_t *)&sloty[novy])[8] ^= 0xFF;  // preruseny zapis do noveho slotu
    CHECK("T13 poskozeny novy slot -> predchozi platny stav", jvSlotyNacti(sloty, r) && r.pocetVzorku == 1);
    s.pocetVzorku = 3; jvSlotyUloz(sloty, s);
    CHECK("T13 dalsi zapis prepise poskozeny slot, oba platne",
          jvSlotyNacti(sloty, r) && r.pocetVzorku == 3 && jvStavPlatny(sloty[0]) && jvStavPlatny(sloty[1]));
    JvRtcState zz[2]; memset(zz, 0, sizeof(zz));
    JvRtcState a; jvStavInit(a); a.sekvence = 0xFFFFFFFEUL;
    jvSlotyUloz(zz, a); a.pocetVzorku = 7; jvSlotyUloz(zz, a);
    CHECK("T13 pretoceni sekvence 0xFFFFFFFF -> 0", jvSlotyNacti(zz, r) && r.pocetVzorku == 7);
    memset(sloty, 0xA5, sizeof(sloty));
    CHECK("T13 nahodna pamet v obou slotech -> cisty start", !jvSlotyNacti(sloty, r));
  }

  // T14 smycka padu: pad uprostred cyklu (bez ulozeni pri uspani) se musi zapocitat
  {
    JvRtcState sloty[2]; memset(sloty, 0xA5, sizeof(sloty));
    JvRtcState s;
    CHECK("T14 cold boot nad nahodnou pameti -> cisty start", !jvBootStav(sloty, s, JV_BOOT_COLD) && s.padStreak == 0);
    jvPripoctiCas(s, 90000); jvSlotyUloz(sloty, s);  // radne uspani
    JvRtcState kopie[2]; memcpy(kopie, sloty, sizeof(sloty));
    for (int i = 0; i < 3; i++) { jvBootStav(sloty, s, JV_BOOT_CRASH); /* pad pred uspanim: nic dalsiho */ }
    CHECK("T14 3 pady uprostred cyklu -> padStreak 3", s.padStreak == 3);
    CHECK("T14 -> dlouhy spanek 10 min", jvDelkaSpankuMs(s, JV_BOOT_CRASH, 0, t) == 600000UL);
    // Negativni kontrola: ukladani jen pri uspani (puvodni 1.2-lp) streak nikdy nezvedne.
    JvRtcState n;
    for (int i = 0; i < 3; i++) { if (!jvSlotyNacti(kopie, n)) jvStavInit(n); jvPoBootu(n, JV_BOOT_CRASH); }
    CHECK("T14 negativni kontrola: bez ulozeni po bootu zustane padStreak 1", n.padStreak == 1);
  }

  // T15 zhaveni MQ pocitane pres deep sleep, nove zapnuti zacina od nuly
  {
    JvRtcState sl[2]; memset(sl, 0xA5, sizeof(sl));
    JvRtcState s;
    jvBootStav(sl, s, JV_BOOT_COLD);
    CHECK("T15 po zapnuti zbyva 3 min - bdeni", jvMqZbyvaMs(s, 300, 180000) == 179700UL);
    jvPripoctiCas(s, 90000); jvSlotyUloz(sl, s);
    jvBootStav(sl, s, JV_BOOT_DEEPSLEEP);
    CHECK("T15 po 90 s stale zhavi", jvMqZbyvaMs(s, 300, 180000) == 89700UL);
    jvPripoctiCas(s, 90000); jvSlotyUloz(sl, s);
    jvBootStav(sl, s, JV_BOOT_DEEPSLEEP);
    CHECK("T15 po 180 s zahraty", jvMqZbyvaMs(s, 0, 180000) == 0);
    jvBootStav(sl, s, JV_BOOT_COLD);
    CHECK("T15 nove zapnuti -> zhaveni znovu", jvMqZbyvaMs(s, 0, 180000) == 180000UL);
  }

  // ---------------------------------------------------------------------------
  // OTA (T16-T22)
  // ---------------------------------------------------------------------------

  // T16 verze: numericky porovnani, striktne vyssi, neplatne vstupy nikdy "novejsi"
  {
    CHECK("T16 1.3.0 > 1.2.9", jvOtaNovejsi("1.3.0", "1.2.9"));
    CHECK("T16 1.10.0 > 1.9.9 (cisla, ne text)", jvOtaNovejsi("1.10.0", "1.9.9"));
    CHECK("T16 2.0.0 > 1.99.99", jvOtaNovejsi("2.0.0", "1.99.99"));
    CHECK("T16 1.3.1 > 1.3.0", jvOtaNovejsi("1.3.1", "1.3.0"));
    CHECK("T16 v1.4 > 1.3.9 (prefix v, bez patch)", jvOtaNovejsi("v1.4", "1.3.9"));
    CHECK("T16 stejna verze neni novejsi", !jvOtaNovejsi("1.3.0", "1.3.0"));
    CHECK("T16 pripona se ignoruje: 1.3.0-rc1 vs 1.3.0", !jvOtaNovejsi("1.3.0-rc1", "1.3.0"));
    CHECK("T16 downgrade 1.2.0 vs 1.3.0", !jvOtaNovejsi("1.2.0", "1.3.0"));
    CHECK("T16 downgrade 0.9.9 vs 1.0.0", !jvOtaNovejsi("0.9.9", "1.0.0"));
    CHECK("T16 starsi nazev 1.2-lp je platna verze 1.2.0", jvOtaNovejsi("1.3.0", "1.2-lp"));
    const char *spatne[] = { "", "abc", "1", "1.", "1.3.", ".3.0", "1.3.0.5", "1.3.x", "1..3", "9999999.0.0", "v", " ", "1.3.0abc" };
    bool zadna_novejsi = true, zadna_platna = true;
    for (const char *s : spatne) {
      int v[3];
      if (jvOtaNovejsi(s, "1.0.0")) zadna_novejsi = false;
      if (jvOtaParseVerze(s, v)) zadna_platna = false;
    }
    CHECK("T16 neplatna verze na serveru nikdy neaktualizuje", zadna_novejsi);
    CHECK("T16 neplatne retezce se neparsuji", zadna_platna);
    CHECK("T16 neplatna lokalni verze -> nikdy novejsi", !jvOtaNovejsi("9.9.9", "garbage"));
    CHECK("T16 NULL bezpecne", !jvOtaNovejsi(nullptr, "1.0.0") && !jvOtaNovejsi("1.0.0", nullptr));
  }

  // T17 prefix podpisu: DOMENA\0 typ\0 verze\0, hranice delek
  {
    uint8_t out[JV_OTA_PREFIX_CAP];
    const size_t n = jvOtaPrefix(out, sizeof(out), "jvmeteo-esp32c3", "1.3.1");
    const size_t ocekavano = sizeof(JV_OTA_DOMENA) + strlen("jvmeteo-esp32c3") + 1 + strlen("1.3.1") + 1;
    CHECK("T17 delka prefixu = domena+NUL + typ+NUL + verze+NUL", n == ocekavano);
    CHECK("T17 zacina domenou a NUL", n > 0 && memcmp(out, "JVMETEO-OTA-SIG-v1\0jvmeteo-esp32c3\0" "1.3.1\0", n) == 0);
    uint8_t jiny[JV_OTA_PREFIX_CAP];
    const size_t m = jvOtaPrefix(jiny, sizeof(jiny), "jvmeteo-esp32s3", "1.3.1");
    CHECK("T17 jiny cip -> jiny prefix", m == n && memcmp(out, jiny, n) != 0);
    const size_t k = jvOtaPrefix(jiny, sizeof(jiny), "jvmeteo-esp32c3", "1.3.2");
    CHECK("T17 jina verze -> jiny prefix", k == n && memcmp(out, jiny, n) != 0);
    char dlouha[80]; memset(dlouha, '1', sizeof(dlouha) - 1); dlouha[sizeof(dlouha) - 1] = 0;
    CHECK("T17 verze nad 48 znaku -> 0", jvOtaPrefix(out, sizeof(out), "jvmeteo-esp32c3", dlouha) == 0);
    CHECK("T17 typ nad 64 znaku -> 0", jvOtaPrefix(out, sizeof(out), dlouha, "1.3.1") == 0);
    CHECK("T17 prazdna verze -> 0", jvOtaPrefix(out, sizeof(out), "jvmeteo-esp32c3", "") == 0);
    CHECK("T17 prazdny typ -> 0", jvOtaPrefix(out, sizeof(out), "", "1.3.1") == 0);
    CHECK("T17 maly buffer -> 0", jvOtaPrefix(out, 10, "jvmeteo-esp32c3", "1.3.1") == 0);
    // Vystup pro krizovou kontrolu se signerem (run_host_test.py ho porovna s tools/ota/sign_ota.py).
    const size_t p = jvOtaPrefix(out, sizeof(out), "jvmeteo-esp32c3", "1.3.1");
    printf("PREFIX jvmeteo-esp32c3 1.3.1 ");
    for (size_t i = 0; i < p; i++) printf("%02x", out[i]);
    printf("\n");
  }

  // T18 plan kontrol: prvni po zapnuti, interval, backoff, trigger, pojistky
  {
    JvRtcState s; jvStavInit(s);
    const JvLadeni t24 = t;
    CHECK("T18 hodiny -> ms (24 h)", jvOtaHodinyNaMs(24) == 86400000UL);
    CHECK("T18 hodiny pod minimem -> 1 h", jvOtaHodinyNaMs(0) == 3600000UL && jvOtaHodinyNaMs(-5) == 3600000UL);
    CHECK("T18 hodiny nad maximem -> 720 h", jvOtaHodinyNaMs(99999) == 720UL * 3600000UL);
    CHECK("T18 po zapnuti je prvni kontrola splatna hned", jvOtaMaKontrolovat(s, JV_BOOT_COLD, true, t24));
    CHECK("T18 bez uspesneho odeslani se nekontroluje", !jvOtaMaKontrolovat(s, JV_BOOT_COLD, false, t24));
    JvLadeni vyp = t24; vyp.otaPovoleno = false;
    CHECK("T18 auto-update vypnuty -> nekontroluje", !jvOtaMaKontrolovat(s, JV_BOOT_COLD, true, vyp));
    JvRtcState r = s; jvOtaZadostTrigger(r);
    CHECK("T18 trigger funguje i pri vypnute automatice", jvOtaMaKontrolovat(r, JV_BOOT_COLD, true, vyp));

    // po uspesne kontrole: ticho do uplynuti intervalu
    jvOtaPrijemZahajeni(s); jvOtaPoKontrole(s, JV_OTA_AKTUALNI);
    CHECK("T18 po kontrole neni splatna", !jvOtaMaKontrolovat(s, JV_BOOT_DEEPSLEEP, true, t24));
    jvPripoctiCas(s, 24UL * 3600000UL - 1UL);
    CHECK("T18 o 1 ms pred intervalem neni splatna", !jvOtaMaKontrolovat(s, JV_BOOT_DEEPSLEEP, true, t24));
    jvPripoctiCas(s, 1UL);
    CHECK("T18 po intervalu je splatna", jvOtaMaKontrolovat(s, JV_BOOT_DEEPSLEEP, true, t24));

    // backoff po neuspechu: interval x2, x4, x8, strop x8
    JvRtcState b; jvStavInit(b);
    const uint32_t zaklad = t24.otaIntervalMs;
    const uint32_t ocek[6] = { 2, 4, 8, 8, 8, 8 };
    bool backoff_ok = true;
    for (int i = 0; i < 6; i++) {
      jvOtaPrijemZahajeni(b); jvOtaPoKontrole(b, JV_OTA_CHYBA_SITE);
      if (jvOtaIntervalMs(b, t24) != zaklad * ocek[i]) backoff_ok = false;
    }
    CHECK("T18 po neuspechu se interval prodluzuje x2,x4,x8 a strop je x8", backoff_ok);
    jvPripoctiCas(b, zaklad * 8UL - 1UL);
    CHECK("T18 pod prodlouzenym intervalem se nekontroluje", !jvOtaMaKontrolovat(b, JV_BOOT_DEEPSLEEP, true, t24));
    jvPripoctiCas(b, 1UL);
    CHECK("T18 po prodlouzenem intervalu ano", jvOtaMaKontrolovat(b, JV_BOOT_DEEPSLEEP, true, t24));
    jvOtaPrijemZahajeni(b); jvOtaPoKontrole(b, JV_OTA_AKTUALNI);
    CHECK("T18 uspech nuluje backoff", b.otaChybStreak == 0 && jvOtaIntervalMs(b, t24) == zaklad);

    // zapocteny pokus preziva zaseknuti: kdyz se po Zahajeni nic nezavola, streak uz je zvyseny
    JvRtcState h; jvStavInit(h);
    jvOtaPrijemZahajeni(h);
    CHECK("T18 zaseknuty pokus (bez PoKontrole) uz je zapocitany", h.otaChybStreak == 1 && h.otaMsOdKontroly == 0);

    // trigger: cooldown, jinak by stale prikaz zahlcoval radio
    JvRtcState c; jvStavInit(c);
    jvOtaPrijemZahajeni(c); jvOtaPoKontrole(c, JV_OTA_AKTUALNI);
    jvOtaZadostTrigger(c);
    CHECK("T18 trigger hned po kontrole se ignoruje (cooldown)", !jvOtaMaKontrolovat(c, JV_BOOT_DEEPSLEEP, true, t24));
    jvPripoctiCas(c, 600000UL);
    CHECK("T18 trigger po cooldownu projde", jvOtaMaKontrolovat(c, JV_BOOT_DEEPSLEEP, true, t24));

    // pojistky: slaba baterie / nestabilita / nepotvrzeny obraz
    JvRtcState p; jvStavInit(p);
    CHECK("T18 brownout boot -> nekontroluje", !jvOtaMaKontrolovat(p, JV_BOOT_BROWNOUT, true, t24));
    p.brownoutStreak = 1;
    CHECK("T18 brownoutStreak -> nekontroluje", !jvOtaMaKontrolovat(p, JV_BOOT_DEEPSLEEP, true, t24));
    p.brownoutStreak = 0; p.padStreak = 1;
    CHECK("T18 padStreak -> nekontroluje", !jvOtaMaKontrolovat(p, JV_BOOT_DEEPSLEEP, true, t24));
    p.padStreak = 0; p.otaNepotvrzeno = 1;
    CHECK("T18 nepotvrzeny obraz -> zadne retezeni aktualizaci", !jvOtaMaKontrolovat(p, JV_BOOT_DEEPSLEEP, true, t24));
    jvOtaZadostTrigger(p);
    CHECK("T18 ani trigger neprebije nepotvrzeny obraz", !jvOtaMaKontrolovat(p, JV_BOOT_DEEPSLEEP, true, t24));
  }

  // T19 instalace, potvrzeni a rollback
  {
    JvRtcState s; jvStavInit(s);
    jvOtaPrijemZahajeni(s); jvOtaPoKontrole(s, JV_OTA_NAINSTALOVANO);
    CHECK("T19 po instalaci je obraz nepotvrzeny", s.otaNepotvrzeno == 1 && s.otaCyklyPoInstalaci == 0 && s.otaChybStreak == 0);
    jvOtaPoCyklu(s, t); jvOtaPoCyklu(s, t);
    CHECK("T19 po 2 cyklech je stale nepotvrzeny", s.otaNepotvrzeno == 1);
    jvOtaPoCyklu(s, t);
    CHECK("T19 po 3 cyklech je potvrzeny", s.otaNepotvrzeno == 0 && s.otaCyklyPoInstalaci == 0);

    // obraz, ktery pada: 3 pady po sobe pred potvrzenim -> rollback
    JvRtcState sl[2]; memset(sl, 0xA5, sizeof(sl));
    JvRtcState n;
    jvBootStav(sl, n, JV_BOOT_COLD);
    jvOtaPrijemZahajeni(n); jvOtaPoKontrole(n, JV_OTA_NAINSTALOVANO); jvSlotyUloz(sl, n);
    jvBootStav(sl, n, JV_BOOT_SOFT);  // restart po instalaci
    CHECK("T19 po restartu je obraz stale nepotvrzeny, rollback nehrozi", n.otaNepotvrzeno == 1 && !jvOtaRollbackNutny(n, t));
    jvBootStav(sl, n, JV_BOOT_CRASH); jvBootStav(sl, n, JV_BOOT_CRASH);
    CHECK("T19 2 pady jeste nejsou rollback", !jvOtaRollbackNutny(n, t));
    jvBootStav(sl, n, JV_BOOT_CRASH);
    CHECK("T19 3 pady pred potvrzenim -> rollback", jvOtaRollbackNutny(n, t));
    jvOtaPoRollbacku(n, t);
    CHECK("T19 po rollbacku neni nepotvrzeny a padStreak je nulovy", n.otaNepotvrzeno == 0 && n.padStreak == 0);
    CHECK("T19 po rollbacku nejdelsi backoff (vadna verze se hned nestahuje znovu)",
          jvOtaIntervalMs(n, t) == t.otaIntervalMs * 8UL && !jvOtaMaKontrolovat(n, JV_BOOT_DEEPSLEEP, true, t));

    // negativni kontroly: potvrzeny obraz se kvuli padum nevraci
    JvRtcState g; jvStavInit(g); g.padStreak = 3;
    CHECK("T19 potvrzeny obraz (otaNepotvrzeno=0) se nevraci ani po 3 padech", !jvOtaRollbackNutny(g, t));
  }

  // T20 TalkBack prikaz: jen OTA_CHECK, nic jineho
  {
    CHECK("T20 OTA_CHECK", jvOtaPrikazJeKontrola("OTA_CHECK"));
    CHECK("T20 male pismena a bile znaky", jvOtaPrikazJeKontrola("  ota_check\r\n"));
    CHECK("T20 v uvozovkach", jvOtaPrikazJeKontrola("\"OTA_CHECK\""));
    CHECK("T20 prazdna fronta", !jvOtaPrikazJeKontrola(""));
    CHECK("T20 prazdny JSON", !jvOtaPrikazJeKontrola("{}"));
    CHECK("T20 jiny prikaz", !jvOtaPrikazJeKontrola("REBOOT"));
    CHECK("T20 prefix prikazu", !jvOtaPrikazJeKontrola("OTA"));
    CHECK("T20 prikaz s priponou", !jvOtaPrikazJeKontrola("OTA_CHECK2"));
    CHECK("T20 prikaz s argumentem (URL/verze nelze vybrat)", !jvOtaPrikazJeKontrola("OTA_CHECK http://evil/x.bin"));
    CHECK("T20 NULL", !jvOtaPrikazJeKontrola(nullptr));
  }

  // T21 cas pro OTA tece pres deep sleep a saturuje, nikdy se neprevali na "cerstve"
  {
    JvRtcState s; jvStavInit(s);
    CHECK("T21 init: kontrola nikdy nebezela", s.otaMsOdKontroly == JV_OTA_MS_NEZNAMO);
    jvPripoctiCas(s, 90000);
    CHECK("T21 pricitani k NEZNAMO zustava NEZNAMO", s.otaMsOdKontroly == JV_OTA_MS_NEZNAMO);
    jvOtaPoKontrole(s, JV_OTA_AKTUALNI);
    jvPripoctiCas(s, 90000); jvPripoctiCas(s, 90000);
    CHECK("T21 po kontrole se pocita realny cas", s.otaMsOdKontroly == 180000UL);
    s.otaMsOdKontroly = 0xFFFFFFF0UL; jvPripoctiCas(s, 1000);
    CHECK("T21 saturace misto pretoceni", s.otaMsOdKontroly == 0xFFFFFFFFUL);
  }

  // T22 RTC stav s OTA poli prezije slot-ochranu a zmena verze ho zneplatni
  {
    JvRtcState sloty[2]; memset(sloty, 0, sizeof(sloty));
    JvRtcState s; jvStavInit(s);
    jvOtaPrijemZahajeni(s); jvOtaPoKontrole(s, JV_OTA_NAINSTALOVANO); jvSlotyUloz(sloty, s);
    JvRtcState r;
    CHECK("T22 OTA pole preziji ulozeni a nacteni", jvSlotyNacti(sloty, r) && r.otaNepotvrzeno == 1);
    JvRtcState stary = s; stary.verze = 2; jvStavZapecetit(stary);
    CHECK("T22 stav starsi verze (1.2-lp) se po aktualizaci nepouzije", !jvStavPlatny(stary));
  }

  // T23 cas (NTP): kdy se synchronizuje
  {
    const int64_t T0 = 1760000000LL;  // platny cas (2025-10)
    CHECK("T23 hodiny 1970 nejsou platne", !jvCasPlatny(0));
    CHECK("T23 hranice platnosti", !jvCasPlatny(1699999999LL) && jvCasPlatny(1700000000LL));
    CHECK("T23 neplatne hodiny -> sync (i kdyz sync vypada cerstve)", jvCasPotrebaSync(100, 50, 86400));
    CHECK("T23 nikdy nesynchronizovano -> sync", jvCasPotrebaSync(T0, 0, 86400));
    CHECK("T23 cerstvy sync (1 h) -> bez syncu", !jvCasPotrebaSync(T0 + 3600, T0, 86400));
    CHECK("T23 presne interval -> sync", jvCasPotrebaSync(T0 + 86400, T0, 86400));
    CHECK("T23 o sekundu min nez interval -> bez syncu", !jvCasPotrebaSync(T0 + 86399, T0, 86400));
    CHECK("T23 po intervalu -> sync", jvCasPotrebaSync(T0 + 90000, T0, 86400));
    CHECK("T23 hodiny sly zpet -> sync", jvCasPotrebaSync(T0 - 10, T0, 86400));
  }

  if (g_invalid) { printf("RESULT: INVALID RUN (%d)  pass=%d fail=%d\n", g_invalid, g_pass, g_fail); return 2; }
  printf("RESULT: %s  pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
