"""Host test BLOCK 03 z jvmeteook2.ino (MSVC).

  python test/host/run_host_test.py

1. Vyrizne BLOCK 03 podle border marks (//-----------3-----------), bez kopie.
2. Prelozi test_power_logic.cpp a spusti ho -> ocekava PASS.
3. Mutace: vrati opravu chyby v5 (baseline po uspesnem odeslani) a ocekava FAIL.
   Kdyz mutovany radek ve bloku neni, je to INVALID RUN, ne PASS.

Exit: 0 = PASS a mutace odhalena, 1 = FAIL, 2 = INVALID RUN.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.normpath(os.path.join(HERE, "..", "..", "jvmeteook2.ino"))
BUILD = os.path.join(HERE, "build")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
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
    bat = os.path.join(BUILD, "build_%s.bat" % tag)
    with open(bat, "w", encoding="ascii", newline="") as f:
        f.write('@call "' + VCVARS + '" >nul' + CRLF)
        f.write('@cl /nologo /EHsc /W3 /utf-8 /I "' + BUILD + '" "' + src + '" /Fe:"' + exe
                + '" /Fo"' + os.path.join(BUILD, "test_%s.obj" % tag) + '"' + CRLF)
    if os.path.exists(exe):
        os.remove(exe)
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(exe):
        print(r.stdout[-3000:], r.stderr[-2000:])
        invalid("preklad (%s) selhal" % tag)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


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

    mutace = [
        # Chovani v5: baseline se nastavi jen pri prvnim odeslani, dal uz ne.
        ("v5_baseline", [(FIX_LINE, "/* MUTACE: oprava v5 vracena */"),
                         (MASK_LINE, "if (!s.odeslanoMaska) { s.odeslanoMaska = jvMaskaPlatnych(cur);"
                                     " memcpy(s.odeslano, cur, sizeof(s.odeslano)); }")]),
        # Ulozeni RTC az pri uspani: pad uprostred cyklu se nezapocita.
        ("commit_at_boot", [(COMMIT_LINE, "/* MUTACE: ulozeni po bootu vypnuto */")]),
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
