#!/usr/bin/env python3
"""Hand-written mutation testing for BacktestX (Python standard library only).

For every mutation in tools/mutations.json this script
  1. patches ONE source file (the 'old' text must occur exactly once),
  2. rebuilds, runs the unit tests (and, for mutations with "also_check", the Python differential check),
  3. reports CAUGHT (something failed) or SURVIVED (everything passed),
  4. restores the file byte-for-byte.

    python tools/mutate.py                 # run everything (slow: one rebuild per mutation, roughly 20-40 s each)
    python tools/mutate.py --list          # show the mutations, run nothing
    python tools/mutate.py --only S3 I1    # ids (prefix match)
    python tools/mutate.py --group strategies indicators
    python tools/mutate.py --verbose       # list every failing test case per mutation

Exit status: 0 if every mutation behaved as its "expect" says (caught -> caught, equivalent -> may survive); 1 otherwise.

This is NOT part of build.ps1 (too slow). A mutation score is not a coverage measurement: it only shows that THESE
specific bugs are detected. Survivors are either test gaps or "equivalent mutants" (behaviourally identical).

Safety: the project has no version control, so before touching a file the original is saved under
build/mutate_backup/. If a previous run was killed, the next run restores those files first. At the end the SHA-256 of
every touched file is compared with its hash from before the run.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP = os.path.join(ROOT, "build", "mutate_backup")
TOOLCHAIN = [r"D:\Tools\w64devkit\bin", r"D:\Tools\cmake\bin"]  # same portable toolchain as build.ps1
ENV = dict(os.environ, PATH=os.pathsep.join(TOOLCHAIN + [os.environ.get("PATH", "")]))
TIMEOUT = 900


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, env=ENV, capture_output=True, text=True, shell=True, timeout=TIMEOUT)


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def recover_leftovers():
    """Restore files left mutated by a run that was killed."""
    if not os.path.isdir(BACKUP):
        return
    restored = []
    for dirpath, _, files in os.walk(BACKUP):
        for name in files:
            saved = os.path.join(dirpath, name)
            rel = os.path.relpath(saved, BACKUP)
            shutil.copyfile(saved, os.path.join(ROOT, rel))
            restored.append(rel)
    shutil.rmtree(BACKUP)
    if restored:
        print("WARNING: a previous run did not finish; restored: " + ", ".join(restored))


def failing_tests(output):
    return sorted(set(re.findall(r"TEST CASE:\s+(.*)", output)))


def build_and_test(also_check, verbose_out):
    """Returns (caught: bool, detail: str)."""
    b = run("cmake --build build -j")
    if b.returncode != 0:
        return None, "BUILD FAILED:\n" + (b.stdout + b.stderr)[-800:]
    t = run(os.path.join("build", "bt_tests.exe"))
    out = t.stdout + t.stderr
    m = re.search(r"test cases:\s*(\d+) \|\s*(\d+) passed \|\s*(\d+) failed", out)
    if not m:
        return True, "test run did not finish normally (crash or hang counts as caught)"
    failed = int(m.group(3))
    if failed or t.returncode != 0:
        names = failing_tests(out)
        verbose_out.extend(names)
        return True, f"{failed} test case(s) failed: " + "; ".join(n[:55] for n in names[:3]) + (" ..." if len(names) > 3 else "")
    for chk in also_check:
        c = run(chk)
        if c.returncode != 0:
            return True, f"unit tests passed but `{chk}` failed: " + (c.stdout + c.stderr).strip().splitlines()[-1][:120]
    return False, f"all {m.group(2)} test cases passed"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", nargs="+", metavar="ID", help="run only mutations whose id starts with one of these")
    ap.add_argument("--group", nargs="+", metavar="GROUP")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    with open(os.path.join(ROOT, "tools", "mutations.json"), encoding="utf-8") as f:
        muts = json.load(f)["mutations"]
    if a.only:
        muts = [m for m in muts if any(m["id"].startswith(p) for p in a.only)]
    if a.group:
        muts = [m for m in muts if m["group"] in a.group]
    if a.list:
        for m in muts:
            print(f"{m['id']:5} {m['group']:11} {m['expect']:10} {m['file']:28} {m['description']}")
        print(f"{len(muts)} mutations")
        return 0

    recover_leftovers()
    # Baseline must be green, otherwise every failure would be misread as "caught".
    if not os.path.isdir(os.path.join(ROOT, "build")):
        r = run('cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release')
        if r.returncode != 0:
            print("configure failed:\n" + r.stderr[-800:])
            return 1
    base, detail = build_and_test([], [])
    if base is not False:
        print("BASELINE IS NOT GREEN, refusing to run mutations: " + detail)
        return 1
    print(f"baseline OK ({detail}); running {len(muts)} mutation(s)\n")

    before = {}
    results = []
    diff_only = []   # caught by the Python differential check but NOT by the C++ unit tests
    for m in muts:
        path = os.path.join(ROOT, m["file"])
        with open(path, "rb") as f:
            original = f.read()
        before.setdefault(m["file"], sha(path))
        text = original.decode("utf-8")
        n = text.count(m["old"])
        if n != 1:
            print(f"[{m['id']}] NOT APPLIED: pattern occurs {n} times in {m['file']}")
            results.append((m, "not_applied"))
            continue
        saved = os.path.join(BACKUP, m["file"])
        os.makedirs(os.path.dirname(saved), exist_ok=True)
        with open(saved, "wb") as f:
            f.write(original)
        names = []
        try:
            with open(path, "wb") as f:
                f.write(text.replace(m["old"], m["new"]).encode("utf-8"))
            caught, detail = build_and_test(m.get("also_check", []), names)
        finally:
            with open(path, "wb") as f:
                f.write(original)
            os.remove(saved)
        if caught is None:
            print(f"[{m['id']}] {m['description']}\n      {detail}")
            results.append((m, "build_failed"))
            continue
        status = "caught" if caught else "survived"
        only_diff = bool(caught) and detail.startswith("unit tests passed but")
        print(f"[{m['id']:4}] {status.upper():8} {m['description']}\n         {detail}")
        if a.verbose and names:
            for nme in names:
                print("           - " + nme)
        results.append((m, status))
        if only_diff:
            diff_only.append(m)

    # Leave the build tree consistent with the restored sources, then verify nothing was left modified.
    run("cmake --build build -j")
    for rel, h in before.items():
        if sha(os.path.join(ROOT, rel)) != h:
            print(f"ERROR: {rel} differs from its pre-run content!")
            return 1
    if os.path.isdir(BACKUP) and not any(files for _, _, files in os.walk(BACKUP)):
        shutil.rmtree(BACKUP, ignore_errors=True)  # no saved files left: remove the empty directory tree

    caught = [m for m, s in results if s == "caught"]
    surv = [m for m, s in results if s == "survived"]
    unexpected = [m for m in surv if m["expect"] != "equivalent"]
    odd = [m for m, s in results if s in ("not_applied", "build_failed")]
    print(f"\n{len(results)} mutations: {len(caught)} caught, {len(surv)} survived "
          f"({len(surv) - len(unexpected)} expected equivalent), {len(odd)} not applied / build failed")
    if diff_only:
        print("  caught ONLY by the Python differential check: " + ", ".join(m["id"] for m in diff_only))
    for m in unexpected:
        print(f"  UNEXPECTED SURVIVOR: {m['id']} {m['description']}")
    for m in odd:
        print(f"  PROBLEM: {m['id']} {m['description']}")
    for m in caught:
        if m["expect"] == "equivalent":
            print(f"  note: {m['id']} was marked equivalent but was caught (its label can be dropped)")
    return 1 if unexpected or odd else 0


if __name__ == "__main__":
    sys.exit(main())
