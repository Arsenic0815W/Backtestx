#!/usr/bin/env python3
"""Open report.html files in a headless Edge/Chrome and check that the page drew itself.

    python -I tools/browser_check.py [--shots DIR] FILE.html [FILE.html ...]

Checks per file: <body data-rendered="1"> was set by the page's script, no data-error attribute, and the rendered DOM contains the
expected parts (run report: 2 charts, the monthly table, the trades table; sweep report: the heatmaps and the comparison table).
Also checks that at a 390 px viewport the page itself is not wider than the viewport (tables and charts must scroll inside their own
containers). --shots DIR also writes PNG screenshots at 1400 px and 420 px wide for a human to look at. Skips cleanly (exit 0, says SKIPPED)
when no Edge/Chrome is installed. A browser-run check of the DOM does not prove the pixels look right: look at the screenshots.
"""
import html as htmllib
import os
import re
import subprocess
import sys
import tempfile

CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]


def find_browser():
    for c in CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def run(browser, profile, url, extra):
    cmd = [browser, "--headless", "--disable-gpu", "--no-first-run", f"--user-data-dir={profile}", "--virtual-time-budget=5000"] + extra + [url]
    return subprocess.run(cmd, capture_output=True, timeout=120)


def scroll_width(browser, profile, path, width):
    """Page width (documentElement.scrollWidth) of a report laid out in a `width`-px viewport.

    Headless Edge will not make a window narrower than about 500 px, so the report is placed in an iframe of the wanted width via
    `srcdoc` (same origin as the wrapper, so the wrapper can read the iframe's size) and the wrapper writes the numbers into <body>.
    """
    with open(path, encoding="utf-8") as f:
        doc = f.read()
    wrapper = ('<!doctype html><body style="margin:0"><iframe id="f" width="%d" height="1200" style="border:0" srcdoc="%s"></iframe>'
               '<script>setTimeout(function(){var d=document.getElementById("f").contentDocument.documentElement;'
               'document.body.setAttribute("data-sw",d.scrollWidth);document.body.setAttribute("data-cw",d.clientWidth);},2500);</script></body>'
               % (width, htmllib.escape(doc, quote=True)))
    with tempfile.TemporaryDirectory() as t:
        w = os.path.join(t, "wrap.html")
        with open(w, "w", encoding="utf-8") as f:
            f.write(wrapper)
        r = run(browser, profile, "file:///" + w.replace("\\", "/"), ["--dump-dom"])
    tag = re.search(r"<body[^>]*>", r.stdout.decode("utf-8", "replace"))
    m = re.search(r'data-sw="(\d+)"[^>]*data-cw="(\d+)"', tag.group(0)) if tag else None
    return (int(m.group(1)), int(m.group(2))) if m else None


def main(argv):
    shots = None
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == "--shots":
            shots = argv[i + 1]
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        print(__doc__)
        return 2
    browser = find_browser()
    if not browser:
        print("SKIPPED: no Edge or Chrome found; the rendered DOM was not checked")
        return 0
    problems = []
    with tempfile.TemporaryDirectory() as profile:
        for f in files:
            path = os.path.abspath(f)
            url = "file:///" + path.replace("\\", "/")
            before = len(problems)
            r = run(browser, profile, url, ["--dump-dom"])
            dom = r.stdout.decode("utf-8", "replace")
            body = re.search(r"<body[^>]*>", dom)
            tag = body.group(0) if body else ""
            if 'data-rendered="1"' not in tag:
                problems.append(f"{f}: the page did not finish drawing (body: {tag!r})")
            if "data-error" in tag:
                problems.append(f"{f}: the page reported an error: {tag}")
            sweep = 'id="heatmaps"' in dom
            wanted = ['id="heatmaps"', 'id="sweep-table"', 'id="bias-note"'] if sweep else \
                ['id="equity-chart"', 'id="drawdown-chart"', 'id="monthly"', 'id="trades"', 'id="fees"', 'id="config"']
            for w in wanted:
                if w not in dom:
                    problems.append(f"{f}: rendered DOM lacks {w}")
            if not sweep and dom.count("<svg") < 2:
                problems.append(f"{f}: fewer than 2 charts were drawn")
            print(f"{'ok  ' if len(problems) == before else 'FAIL'} {f}: rendered, {dom.count('<svg')} svg, {dom.count('<table')} tables")
            for vw in (390,):
                sw = scroll_width(browser, profile, path, vw)
                if sw is None:
                    problems.append(f"{f}: could not measure the page width at {vw}px")
                elif sw[0] > vw:
                    problems.append(f"{f}: at {vw}px the page is {sw[0]}px wide (horizontal scroll)")
                else:
                    print(f"     {f}: at {vw}px the page is {sw[0]}px wide (viewport {sw[1]}px)")
            if shots:
                os.makedirs(shots, exist_ok=True)
                stem = re.sub(r"[^A-Za-z0-9]+", "_", os.path.relpath(path, os.getcwd()))
                for w, h in ((1400, 2600), (420, 3600)):
                    out = os.path.abspath(os.path.join(shots, f"{stem}_{w}.png"))
                    run(browser, profile, url, [f"--window-size={w},{h}", f"--screenshot={out}"])
                    if not os.path.exists(out):
                        problems.append(f"{f}: screenshot at {w}px was not written")
    if problems:
        print("FAIL")
        for p in problems:
            print("  " + p)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
