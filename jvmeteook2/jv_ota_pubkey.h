#pragma once
// Verejny klic pro overeni podpisu OTA obrazu (RSA-2048, PKCS#1 v1.5, SHA-256).
// Privatni klic NENI v repu: drzi ho majitel vydavatelskeho workflow a je v GitHub
// Actions secretu OTA_SIGNING_KEY. Vymena klice = novy USB flash vsech zarizeni,
// protoze stara zarizeni zadny jiny klic neprijmou.
//
// Format a tok: tools/ota/sign_ota.py, .github/workflows/firmware.yml.
#define JV_OTA_PUBKEY_PEM \
  "-----BEGIN PUBLIC KEY-----\n" \
  "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt1Se6RIr1aDvghdYO53g\n" \
  "xz7CjrFLATRPKk5DJ5CtbLXjgcC3nd/39lD6cBZGVbVVJ88E/nwRVAjEjIeezDvg\n" \
  "3Je1/OsN+QfiLeByAVDF0b3TDyc+1cte8ZhUN4shGcSHSjJ0VvaQP2zkSW6fQ63o\n" \
  "lMnNnWhfy2rFZTbLIx1gjOiyNG+FQH9hkfrsQUsbNoDwjSEsbo9qrNOdqhHYM6jR\n" \
  "T+hBWDAh1KToyDVIDapNG3Ph8ab6ghCFfPl0qVJM9scbctAFuqFJORPbWnYlTLQo\n" \
  "WkkBMjJy648LEryGUAojztwaXywAQumus2meAg10V3am5HsFteQ2L7VE7vL9ExEv\n" \
  "owIDAQAB\n" \
  "-----END PUBLIC KEY-----\n"
