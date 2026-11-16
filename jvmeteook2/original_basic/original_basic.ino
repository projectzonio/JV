#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include "ThingSpeak.h" // Oficialni knihovna pro bezpecny prenos dat

// Knihovny pro senzory
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>

// Casove konstanty v milisekundach
#define INTERVAL_SPANKU_MS 90000       // 90 sekund spanku mezi merenimi
#define WIFI_TIMEOUT_MS 7000           // maximalni doba pripojovani k Wi-Fi
#define TIMEOUT_KONFIGURACE_MS 300000  // 5 minut v rezimu nastaveni pred auto-restartem
#define OKNO_RESETU_PRO_KONFIGURACI_MS 1200  // jeden kratky reset po zapnuti

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
</section><button class='save' type='submit' name='ulozit' value='1'>Uložit nastavení a restartovat měření</button></form>
<footer class='footer'><div class='coreon'>COREON</div><div class='firmware'>Firmware 1.1 · pro hardware 1.1 až 2.4</div><div class='notice'>Po uložení se stanice restartuje a spustí měřicí cyklus.</div></footer>
</main></body></html>
)rawliteral";

// Inicializace I2C sbernic na vyzadani
bool inicializuj_i2c(int sda, int scl) {
  Wire.end(); 
  Wire.begin(sda, scl);
  return true;
}

// --- Nacteni dat ze vsech aktivovanych senzoru ---
void cti_vsechny_senzory() {
  // Neplatne nebo odpojene cidlo nesmi zapsat do ThingSpeak falesnou nulu.
  teplota = NAN; vlhkost = NAN; tlak = NAN;
  mpu_ax = NAN; mpu_ay = NAN; mpu_az = NAN; mq9_raw = -1;

  if (en_bme280) {
    inicializuj_i2c(bme_sda, bme_scl);
    Adafruit_BME280 bme;
    if (bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire)) {
      // Jedno presne mereni, pote se BME280 sam vrati do usporného sleep rezimu.
      bme.setSampling(Adafruit_BME280::MODE_FORCED,
        Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF);
      if (bme.takeForcedMeasurement()) {
        teplota = bme.readTemperature();
        vlhkost = bme.readHumidity();
        tlak = bme.readPressure() / 100.0F;
      }
    }
  }
  if (en_bmp280) {
    inicializuj_i2c(bmp_sda, bmp_scl);
    Adafruit_BMP280 bmp(&Wire); 
    if (bmp.begin(0x76) || bmp.begin(0x77)) {
      bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
        Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X1,
        Adafruit_BMP280::FILTER_OFF);
      if (bmp.takeForcedMeasurement()) {
        teplota = bmp.readTemperature();
        tlak = bmp.readPressure() / 100.0F;
      }
    }
  }
  if (en_mpu6050) {
    inicializuj_i2c(mpu_sda, mpu_scl);
    Adafruit_MPU6050 mpu;
    if (mpu.begin(0x68, &Wire) || mpu.begin(0x69, &Wire)) {
      sensors_event_t a, g, temp;
      mpu.getEvent(&a, &g, &temp);
      mpu_ax = a.acceleration.x;
      mpu_ay = a.acceleration.y;
      mpu_az = a.acceleration.z;
      if (!en_bme280 && !en_bmp280) { teplota = temp.temperature; }
    }
  }
  if (en_dht22) {
    DHT dht(dht_pin, DHT22);
    dht.begin();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) { teplota = t; }
    if (!isnan(h)) { vlhkost = h; }
  }
  if (en_mq9) {
    pinMode(mq9_pin, INPUT);
    long suma = 0;
    for (int i = 0; i < 5; i++) {
      suma += analogRead(mq9_pin);
      delay(20);
    }
    mq9_raw = suma / 5;
  }
}

// --- Odeslani platnych hodnot oficialni knihovnou ThingSpeak ---
bool odesli_na_thingspeak() {
  if (WiFi.status() != WL_CONNECTED || THINGSPEAK_CHANNEL_ID == 0UL) { return false; }
  
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
  if (response == 200) {
    Serial.println("Data uspesne zapsana na ThingSpeak skrze oficialni API.");
    return true;
  } else {
    Serial.print("Chyba odesilani ThingSpeak, kod: "); Serial.println(response);
    return false;
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

    pref.begin("meteo", false);
    pref.putString("ssid", wifi_ssid); pref.putString("pass", wifi_pass); pref.putString("ts", thingspeak_api);
    pref.putBool("en_bme", en_bme280); pref.putBool("en_bmp", en_bmp280); pref.putBool("en_mpu", en_mpu6050); pref.putBool("en_dht", en_dht22); pref.putBool("en_mq9", en_mq9);
    pref.putInt("bme_sda", bme_sda); pref.putInt("bme_scl", bme_scl);
    pref.putInt("bmp_sda", bmp_sda); pref.putInt("bmp_scl", bmp_scl);
    pref.putInt("mpu_sda", mpu_sda); pref.putInt("mpu_scl", mpu_scl);
    pref.putInt("dht_pin", dht_pin); pref.putInt("mq9_pin", mq9_pin);
    pref.end();

    server.send(200, "text/html", "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:linear-gradient(145deg,#020b29,#07326b);color:#f4f8ff;font-family:Arial;text-align:center}.box{padding:32px;border:1px solid #4f85bf;border-radius:24px;background:rgba(8,35,82,.8)}b{color:#72e4f3}</style><div class='box'><h1>✓ Nastavení uloženo</h1><p>Stanice se nyní restartuje a zahájí měření.</p><b>JVmeteo COREON</b></div></html>");
    delay(2000);
    ESP.restart();
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

  server.send(200, "text/html", page);
}

void uspani() {
  Wire.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  esp_sleep_enable_timer_wakeup((uint64_t)INTERVAL_SPANKU_MS * 1000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);

  // Inicializace ThingSpeak komunikacni vrstvy skrze WiFiClient objekt
  ThingSpeak.begin(client);

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
  pref.end();

  // Prvni start na 1,2 s pripravi konfiguracni okno. Kratky reset v tomto
  // okne spusti na dalsim bootu konfiguracni pristupovy bod. Jde o stejny,
  // na teto desce osvedceny princip jako v puvodnim firmware.
  pref.begin("meteo", true);
  const bool konfiguracni_okno = pref.getBool("config_arm", false);
  pref.end();
  if (konfiguracni_okno) {
    rezim_konfigurace = true;
    pref.begin("meteo", false);
    pref.remove("config_arm");
    pref.end();
  } else {
    pref.begin("meteo", false);
    pref.putBool("config_arm", true);
    pref.end();
    delay(OKNO_RESETU_PRO_KONFIGURACI_MS);
    pref.begin("meteo", false);
    pref.remove("config_arm");
    pref.end();
  }

  if (rezim_konfigurace) {
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
  else {
  cti_vsechny_senzory();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  const unsigned long zacatek_pripojeni = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - zacatek_pripojeni < WIFI_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
  odesli_na_thingspeak();
  }
  uspani();
  }
}

void loop() {
  if (rezim_konfigurace) {
    server.handleClient();
    if (millis() > TIMEOUT_KONFIGURACE_MS) {
      ESP.restart();
    }
  }
  delay(10);
}
