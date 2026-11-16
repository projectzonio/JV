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

  if (g_invalid) { printf("RESULT: INVALID RUN (%d)  pass=%d fail=%d\n", g_invalid, g_pass, g_fail); return 2; }
  printf("RESULT: %s  pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
