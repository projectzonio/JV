#!/usr/bin/env python3
"""Podpis a overeni OTA obrazu JVmeteo (stdlib + openssl CLI).

  sign_ota.py sign   --in app.bin --key private.pem --type jvmeteo-esp32c3 --version 1.3.1 --out firmware.bin
  sign_ota.py verify --in firmware.bin --pub public.pem --type jvmeteo-esp32c3 --version 1.3.1
  sign_ota.py verify --in firmware.bin --pub-header jv_ota_pubkey.h --type ... --version ...
  sign_ota.py keygen --out-key private.pem --pub-header jv_ota_pubkey.h   (vlastni dvojice klicu)
  sign_ota.py keycheck --key private.pem --pub-header jv_ota_pubkey.h     (sedi secret k firmware?)

Format (shodny se zrt_ota.cpp, jen jina domena, aby se nedal podstrcit obraz
podepsany jinym klicem/produktem):

  firmware.bin = image || sig[256] || uint32_le(256)
  sig          = RSA-2048 PKCS#1 v1.5 nad SHA-256(
                   "JVMETEO-OTA-SIG-v1\\0" || type || "\\0" || version || "\\0" || image)

type + version jsou v podepsane zprave: zarizeni nenainstaluje obraz pro jiny
cip ani starsi podepsany obraz pod novejsim version.txt.

Exit: 0 = OK, 1 = podpis neplati / vstup nesedi, 2 = chyba nastroje (INVALID RUN).
"""
import argparse
import os
import subprocess
import sys
import tempfile

DOMAIN = b"JVMETEO-OTA-SIG-v1"
SIG_LEN = 256
TRAILER = SIG_LEN + 4
MAX_VERSION = 48
MAX_TYPE = 64


def openssl_bin():
    for c in (os.environ.get("OPENSSL"), "openssl", r"C:\Program Files\OpenSSL-Win64\bin\openssl.exe"):
        if not c:
            continue
        try:
            subprocess.run([c, "version"], capture_output=True, check=True)
            return c
        except (OSError, subprocess.CalledProcessError):
            continue
    print("INVALID RUN: openssl nenalezen (nastav OPENSSL=cesta)")
    sys.exit(2)


def prefix(typ, ver):
    t, v = typ.encode("ascii"), ver.encode("ascii")
    if not t or len(t) > MAX_TYPE or not v or len(v) > MAX_VERSION:
        print("INVALID RUN: type/version prazdne nebo prilis dlouhe")
        sys.exit(2)
    return DOMAIN + b"\0" + t + b"\0" + v + b"\0"


def sign(a):
    ossl = openssl_bin()
    image = open(a.infile, "rb").read()
    if len(image) < 1024 or image[0] != 0xE9:
        print("INVALID RUN: vstup neni ESP32 app image (chybi magic 0xE9 nebo je kratky)")
        return 2
    with tempfile.TemporaryDirectory() as td:
        msg = os.path.join(td, "msg.bin")
        sigf = os.path.join(td, "sig.bin")
        with open(msg, "wb") as f:
            f.write(prefix(a.type, a.version) + image)
        r = subprocess.run([ossl, "dgst", "-sha256", "-sign", a.key, "-out", sigf, msg], capture_output=True)
        if r.returncode != 0:
            print("INVALID RUN: openssl sign selhal:", r.stderr.decode(errors="replace").strip())
            return 2
        sig = open(sigf, "rb").read()
    if len(sig) != SIG_LEN:
        print("INVALID RUN: podpis nema %d bajtu (klic neni RSA-2048?): %d" % (SIG_LEN, len(sig)))
        return 2
    out = image + sig + SIG_LEN.to_bytes(4, "little")
    with open(a.out, "wb") as f:
        f.write(out)
    print("signed %s: image %d B + trailer %d B = %d B" % (a.out, len(image), TRAILER, len(out)))
    return 0


def pubkey_from_header(path, out_pem):
    """Vytahne PEM z jv_ota_pubkey.h (rada "..\\n" retezcu) - to, co ma firmware, ne to, co chceme mit."""
    import re
    text = open(path, encoding="utf-8").read()
    radky = re.findall(r'^\s*"(.*?)\\n"\s*\\?\s*$', text, re.M)
    pem = "\n".join(radky) + "\n"
    if not pem.startswith("-----BEGIN PUBLIC KEY-----") or "-----END PUBLIC KEY-----" not in pem:
        print("INVALID RUN: v %s nenalezen verejny klic" % path)
        sys.exit(2)
    with open(out_pem, "w", encoding="ascii", newline="\n") as f:
        f.write(pem)


def verify(a):
    ossl = openssl_bin()
    if a.pub_header:
        # Overuje se proti klici, ktery je ve firmware: kdyby tajny klic v CI nesedel,
        # vznikl by release, ktery by zadne zarizeni neprijalo.
        td0 = tempfile.mkdtemp()
        a.pub = os.path.join(td0, "pub.pem")
        pubkey_from_header(a.pub_header, a.pub)
    if not a.pub:
        print("INVALID RUN: chybi --pub nebo --pub-header")
        return 2
    data = open(a.infile, "rb").read()
    if len(data) <= TRAILER:
        print("FAIL: soubor je kratsi nez trailer")
        return 1
    if int.from_bytes(data[-4:], "little") != SIG_LEN:
        print("FAIL: trailer neobsahuje delku podpisu 256")
        return 1
    image, sig = data[:-TRAILER], data[-TRAILER:-4]
    with tempfile.TemporaryDirectory() as td:
        msg = os.path.join(td, "msg.bin")
        sigf = os.path.join(td, "sig.bin")
        with open(msg, "wb") as f:
            f.write(prefix(a.type, a.version) + image)
        with open(sigf, "wb") as f:
            f.write(sig)
        r = subprocess.run([ossl, "dgst", "-sha256", "-verify", a.pub, "-signature", sigf, msg], capture_output=True)
    if r.returncode != 0:
        print("FAIL: podpis NEPLATI pro type=%s version=%s" % (a.type, a.version))
        return 1
    print("OK: podpis plati (type=%s version=%s, image %d B)" % (a.type, a.version, len(image)))
    return 0


HEADER_TEMPLATE = """#pragma once
// Verejny klic pro overeni podpisu OTA obrazu (RSA-2048, PKCS#1 v1.5, SHA-256).
// Privatni klic NENI v repu: drzi ho majitel vydavatelskeho workflow a je v GitHub
// Actions secretu OTA_SIGNING_KEY. Vymena klice = novy USB flash vsech zarizeni,
// protoze stara zarizeni zadny jiny klic neprijmou.
//
// Format a tok: tools/ota/sign_ota.py, .github/workflows/firmware.yml.
#define JV_OTA_PUBKEY_PEM \\
{radky}
"""


def pem_body(pem_text):
    return "".join(pem_text.split())


def keygen(a):
    """Vlastni dvojice klicu pro vlastni GitHub ucet: privatni PEM mimo repo, verejny do hlavicky firmware."""
    ossl = openssl_bin()
    if os.path.exists(a.out_key) and not a.force:
        print("INVALID RUN: %s uz existuje (prepsani by znehodnotilo zarizeni, ktera ho znaji; --force)" % a.out_key)
        return 2
    r = subprocess.run([ossl, "genrsa", "-out", a.out_key, "2048"], capture_output=True)
    if r.returncode != 0 or not os.path.exists(a.out_key):
        print("INVALID RUN: openssl genrsa selhal:", r.stderr.decode(errors="replace").strip())
        return 2
    try:
        os.chmod(a.out_key, 0o600)
    except OSError:
        pass
    r = subprocess.run([ossl, "rsa", "-in", a.out_key, "-pubout"], capture_output=True)
    pub = r.stdout.decode("ascii", errors="replace").strip().splitlines()
    if r.returncode != 0 or len(pub) < 3 or not pub[0].startswith("-----BEGIN PUBLIC KEY-----"):
        print("INVALID RUN: nepodarilo se odvodit verejny klic:", r.stderr.decode(errors="replace").strip())
        return 2
    radky = "\n".join('  "%s\\n" %s' % (l, "\\" if i < len(pub) - 1 else "") for i, l in enumerate(pub)).rstrip()
    with open(a.pub_header, "w", encoding="ascii", newline="\n") as f:
        f.write(HEADER_TEMPLATE.format(radky=radky))
    print("Privatni klic: %s  (NIKDY do repa, do chatu ani do logu; zalohujte - bez nej nejde vydat dalsi verzi)" % a.out_key)
    print("Verejny klic zapsan do %s -> commitnete ho." % a.pub_header)
    print("Dalsi krok: ulozte obsah privatniho klice jako GitHub secret OTA_SIGNING_KEY"
          " (Settings -> Secrets and variables -> Actions, nebo environment 'release')."
          " Zarizeni se zabudovanym klicem z JINEHO uctu tuhle zmenu neprijmou - nahrajte je pres USB.")
    return 0


def keycheck(a):
    """Sedi tajny klic z CI s verejnym klicem ve firmware? Srozumitelna chyba misto zahadneho FAIL pri overeni."""
    ossl = openssl_bin()
    with tempfile.TemporaryDirectory() as td:
        pub = os.path.join(td, "pub.pem")
        pubkey_from_header(a.pub_header, pub)
        firmware_pem = open(pub, encoding="ascii").read()
    r = subprocess.run([ossl, "rsa", "-in", a.key, "-pubout"], capture_output=True)
    if r.returncode != 0:
        print("FAIL: secret OTA_SIGNING_KEY neni platny soukromy RSA klic (PEM) - vlozte cely obsah souboru vcetne radku BEGIN/END")
        return 1
    if pem_body(r.stdout.decode("ascii", errors="replace")) != pem_body(firmware_pem):
        print("FAIL: secret OTA_SIGNING_KEY nepatri k verejnemu klici ve firmware (%s). "
              "Bud vlozte spravny privatni klic, nebo si vyrobte vlastni dvojici: "
              "python tools/ota/sign_ota.py keygen --out-key <soubor.pem> --pub-header %s, "
              "commitnete hlavicku a ulozte novy secret. Zarizeni se starym klicem se pak aktualizuji jen pres USB."
              % (a.pub_header, a.pub_header))
        return 1
    print("OK: secret odpovida verejnemu klici ve firmware")
    return 0


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("keygen")
    g.add_argument("--out-key", required=True, help="kam zapsat soukromy klic (mimo repozitar)")
    g.add_argument("--pub-header", required=True, help="jv_ota_pubkey.h, ktery se prepise")
    g.add_argument("--force", action="store_true")
    c = sub.add_parser("keycheck")
    c.add_argument("--key", required=True)
    c.add_argument("--pub-header", required=True)
    s = sub.add_parser("sign")
    s.add_argument("--in", dest="infile", required=True)
    s.add_argument("--key", required=True)
    s.add_argument("--type", required=True)
    s.add_argument("--version", required=True)
    s.add_argument("--out", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--in", dest="infile", required=True)
    v.add_argument("--pub")
    v.add_argument("--pub-header", help="jv_ota_pubkey.h: overit proti klici, ktery ma firmware")
    v.add_argument("--type", required=True)
    v.add_argument("--version", required=True)
    a = p.parse_args()
    return {"sign": sign, "verify": verify, "keygen": keygen, "keycheck": keycheck}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
