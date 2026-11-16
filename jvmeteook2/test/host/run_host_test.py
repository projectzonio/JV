"""Host test BLOCK 03 z jvmeteook2.ino (MSVC).

  python test/host/run_host_test.py

1. Vyrizne BLOCK 03 podle border marks (//-----------3-----------), bez kopie.
2. Prelozi test_power_logic.cpp a spusti ho -> ocekava PASS.
3. Mutace: vrati opravu chyby v5 (baseline po uspesnem odeslani), vypne ulozeni
   RTC po bootu a rozbije kazdou z OTA pojistek; test musi u kazde spadnout.
   Kdyz mutovany radek ve bloku neni prave jednou, je to INVALID RUN, ne PASS.
4. Krizova kontrola: prefix podpisu, ktery pocita zarizeni (BLOCK 03), se musi
   bajt po bajtu shodovat s tim, co podepisuje tools/ota/sign_ota.py. Bez toho by
   zarizeni a podepisovac mohly tise rozejit a kazdy obraz by byl "neplatny".

Prekladac: MSVC (vcvars64), jinak g++/c++ (CI na Linuxu).

Exit: 0 = PASS a vsechny mutace odhaleny, 1 = FAIL, 2 = INVALID RUN.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.normpath(os.path.join(HERE, "..", "..", "jvmeteook2.ino"))
SIGNER = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "ota", "sign_ota.py"))
BUILD = os.path.join(HERE, "build")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
USE_MSVC = os.path.exists(VCVARS)
FIX_LINE = "for (int i = 0; i < JV_POLI; i++) s.odeslano[i] = cur[i];"
MASK_LINE = "s.odeslanoMaska = jvMaskaPlatnych(cur);"
COMMIT_LINE = "jvSlotyUloz(sloty, s); // commit-at-boot"
CRLF = "\r\n"


def invalid(msg):
    print("INVALID RUN: " + msg)
    sys.exit(2)


def extract_block(text, n):
    mark = "//-----------%d-----------" % n
    lines = text.splitlines()
    idx = [i for i, l in enumerate(lines) if l.strip() == mark]
    if len(idx) != 2:
        invalid("block %d ma %d border marks (ocekavany 2)" % (n, len(idx)))
    body = lines[idx[0] + 1: idx[1]]
    if not any(("BLOCK %02d" % n) in l for l in body[:3]):
        invalid("block %d nema hlavicku BLOCK %02d" % (n, n))
    if any(re.match(r"\s*#include\s*<(Arduino|WiFi|Wire|esp_)", l) for l in body):
        invalid("block %d zavisi na Arduino/IDF hlavicce" % n)
    return "\n".join(body) + "\n"


def build_and_run(block_src, tag):
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "block03.inc"), "w", encoding="utf-8") as f:
        f.write(block_src)
    exe = os.path.join(BUILD, "test_%s.exe" % tag)
    src = os.path.join(HERE, "test_power_logic.cpp")
    if os.path.exists(exe):
        os.remove(exe)
    if USE_MSVC:
        bat = os.path.join(BUILD, "build_%s.bat" % tag)
        with open(bat, "w", encoding="ascii", newline="") as f:
            f.write('@call "' + VCVARS + '" >nul' + CRLF)
            f.write('@cl /nologo /EHsc /W3 /utf-8 /I "' + BUILD + '" "' + src + '" /Fe:"' + exe
                    + '" /Fo"' + os.path.join(BUILD, "test_%s.obj" % tag) + '"' + CRLF)
        r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    else:
        cxx = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
        if not cxx:
            invalid("neni MSVC ani g++/c++/clang++")
        r = subprocess.run([cxx, "-std=c++17", "-O0", "-Wall", "-I", BUILD, src, "-o", exe, "-lm"],
                           capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(exe):
        print(r.stdout[-3000:], r.stderr[-2000:])
        invalid("preklad (%s) selhal" % tag)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


def kontrola_cizi_ucet(sketch_text):
    """JVmeteo je samostatny: musi fungovat pod JAKYMKOLI GitHub uctem, ne jen pod tim, ktery ho vytvoril.

    1) sketch nesmi mit natvrdo majitele repozitare (URL se sklada z JV_OTA_REPO);
    2) vlastni dvojice klicu z `keygen` projde cely tok sign -> verify proti VLASTNI hlavicce;
    3) negativni kontrola: tentyz podepsany obraz proti hlavicce z repa MUSI selhat
       (jinak by overeni nic nemerilo a fork by mohl vydat obraz, ktery zadne zarizeni neprijme).
    """
    import subprocess
    import tempfile
    jadro = os.path.dirname(SKETCH)
    cfg_path = os.path.join(jadro, "jv_ota_config.h")
    with open(cfg_path, encoding="utf-8") as f:
        cfg = f.read()
    m = re.search(r'^#define JV_OTA_REPO "([^"]+)"\s*$', cfg, re.M)
    if not m or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", m.group(1)):
        print("FAIL: jv_ota_config.h nema platny JV_OTA_REPO \"majitel/repozitar\"")
        return 1
    repo = m.group(1)
    if "JV_OTA_REPO" not in sketch_text or "github.com/\" JV_OTA_REPO" not in sketch_text:
        print("FAIL: sketch neskladá OTA URL z JV_OTA_REPO")
        return 1
    natvrdo = re.findall(r'github\.com/[A-Za-z0-9_.-]+', sketch_text)
    if natvrdo:
        print("FAIL: sketch obsahuje natvrdo adresu GitHub uctu mimo jv_ota_config.h: %s" % natvrdo)
        return 1
    print("OK: OTA URL se sklada z JV_OTA_REPO (%s), majitel v sketchi natvrdo neni" % repo)

    def signer(*args):
        r = subprocess.run([sys.executable, SIGNER, *args], capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr).strip()

    repo_header = os.path.join(jadro, "jv_ota_pubkey.h")
    with tempfile.TemporaryDirectory() as td:
        key = os.path.join(td, "ucet.pem")
        hdr = os.path.join(td, "jv_ota_pubkey.h")
        img = os.path.join(td, "app.bin")
        out = os.path.join(td, "signed.bin")
        with open(img, "wb") as f:
            f.write(b"\xE9" + bytes((i * 7 + 3) & 0xFF for i in range(4096)))
        pruchod = [
            ("keygen", ("keygen", "--out-key", key, "--pub-header", hdr), 0),
            ("keygen nesmi prepsat existujici klic", ("keygen", "--out-key", key, "--pub-header", hdr), 2),
            ("keycheck vlastni klic x vlastni hlavicka", ("keycheck", "--key", key, "--pub-header", hdr), 0),
            ("keycheck vlastni klic x hlavicka z repa (musi selhat)", ("keycheck", "--key", key, "--pub-header", repo_header), 1),
            ("sign", ("sign", "--in", img, "--key", key, "--type", "jvmeteo-esp32c3", "--version", "9.9.9", "--out", out), 0),
            ("verify proti vlastni hlavicce", ("verify", "--in", out, "--pub-header", hdr, "--type", "jvmeteo-esp32c3", "--version", "9.9.9"), 0),
            ("verify proti hlavicce z repa (musi selhat)", ("verify", "--in", out, "--pub-header", repo_header, "--type", "jvmeteo-esp32c3", "--version", "9.9.9"), 1),
            ("verify pod jinou verzi (musi selhat)", ("verify", "--in", out, "--pub-header", hdr, "--type", "jvmeteo-esp32c3", "--version", "9.9.8"), 1),
        ]
        for popis, args, ocekavany in pruchod:
            rc, vystup = signer(*args)
            if rc != ocekavany:
                print("FAIL: cizi ucet / %s: rc=%d, cekano %d\n%s" % (popis, rc, ocekavany, vystup))
                return 1 if rc != 2 else 2
            print("OK: cizi ucet / %s (rc=%d)" % (popis, rc))
        with open(hdr, encoding="ascii") as f:
            if "BEGIN PUBLIC KEY" not in f.read():
                print("FAIL: keygen nezapsal verejny klic do hlavicky")
                return 1
    return 0


def main():
    with open(SKETCH, encoding="utf-8") as f:
        text = f.read()
    block = extract_block(text, 3)

    rc, out = build_and_run(block, "fixed")
    print("== FW (BLOCK 03 ze sketche)" + "\n" + out)
    if rc == 2:
        return 2
    if rc != 0:
        return 1

    # Krizova kontrola prefixu podpisu: zarizeni vs. sign_ota.py.
    m = re.search(r"^PREFIX (\S+) (\S+) ([0-9a-f]+)$", out, re.M)
    if not m:
        invalid("test nevypsal radek PREFIX - krizova kontrola signeru nema co porovnat")
    spec = importlib.util.spec_from_file_location("sign_ota", SIGNER)
    signer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(signer)
    ocekavany = signer.prefix(m.group(1), m.group(2)).hex()
    if m.group(3) != ocekavany:
        print("FAIL: prefix zarizeni != prefix signeru\n  zarizeni: %s\n  signer:   %s" % (m.group(3), ocekavany))
        return 1
    print("OK: prefix podpisu zarizeni se shoduje se sign_ota.py (%d B)" % (len(ocekavany) // 2))

    rc_ucet = kontrola_cizi_ucet(text)
    if rc_ucet != 0:
        return rc_ucet

    mutace = [
        # Chovani v5: baseline se nastavi jen pri prvnim odeslani, dal uz ne.
        ("v5_baseline", [(FIX_LINE, "/* MUTACE: oprava v5 vracena */"),
                         (MASK_LINE, "if (!s.odeslanoMaska) { s.odeslanoMaska = jvMaskaPlatnych(cur);"
                                     " memcpy(s.odeslano, cur, sizeof(s.odeslano)); }")]),
        # Ulozeni RTC az pri uspani: pad uprostred cyklu se nezapocita.
        ("commit_at_boot", [(COMMIT_LINE, "/* MUTACE: ulozeni po bootu vypnuto */")]),
        # OTA: stejna verze se tvari jako novejsi -> zarizeni by se aktualizovalo porad dokola.
        ("ota_stejna_verze", [("return false;  // stejna verze neni novejsi (jinak by se aktualizovalo porad dokola)",
                               "return true;  /* MUTACE */")]),
        # OTA: obraz s nizsi verzi se povazuje za novejsi -> downgrade.
        ("ota_downgrade", [("if (a[i] != b[i]) return a[i] > b[i];", "if (a[i] != b[i]) return a[i] < b[i];")]),
        # OTA: po instalaci se obraz neoznaci jako nepotvrzeny -> rollback nikdy nesepne.
        ("ota_bez_nepotvrzeno", [('s.otaNepotvrzeno = 1;  // novy obraz bezi "na zkousku"', "/* MUTACE */")]),
        # OTA: obraz se nikdy nepotvrdi -> kazdy by se casem "vratil".
        ("ota_bez_potvrzeni", [("if (s.otaCyklyPoInstalaci >= t.otaPotvrzeniCyklu) {", "if (false) {")]),
        # OTA: pokus se nezapocita pred spustenim -> zaseknuty pokus nema backoff.
        ("ota_bez_pesimisticke_stopy", [("s.otaChybStreak = jvSatInc(s.otaChybStreak);", "/* MUTACE */")]),
        # OTA: kontrola i pri brownoutu/padech -> TLS spicka na slabe baterii.
        # Cas: po intervalu se nikdy znovu nesynchronizuje -> hodiny by driftovaly donekonecna.
        ("cas_bez_intervalu", [("return (now - posledniSync) >= (int64_t)intervalS;", "return false;  /* MUTACE */")]),
        # Cas: neplatne hodiny (1970) se nepovazuji za duvod k syncu -> cas by zustal 1970.
        ("cas_neplatne_hodiny", [("if (!jvCasPlatny(now)) return true;", "/* MUTACE */")]),
        ("ota_bez_pojistek", [("if (boot == JV_BOOT_BROWNOUT || s.brownoutStreak > 0 || s.padStreak > 0) return false;",
                               "/* MUTACE */")]),
    ]
    for jmeno, nahrady in mutace:
        mutant = block
        for stary, novy in nahrady:
            if block.count(stary) != 1:
                invalid("mutace %s: radek '%s' neni v BLOCK 03 prave jednou" % (jmeno, stary))
            mutant = mutant.replace(stary, novy)
        rc_m, out_m = build_and_run(mutant, "mut_" + jmeno)
        print("== MUTANT %s" % jmeno + "\n" + out_m)
        if rc_m != 1:
            print("FAIL: mutant %s nespadl (rc=%d) - test tuto opravu nehlida" % (jmeno, rc_m))
            return 1
        print("OK: mutant %s spadl" % jmeno)
    return 0


if __name__ == "__main__":
    sys.exit(main())
