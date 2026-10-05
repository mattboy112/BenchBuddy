import gzip
import os
import re

try:
    Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WEB_DIR = os.path.join(PROJECT_DIR, "web")
OUT_FILE = os.path.join(PROJECT_DIR, "src", "web_assets.h")

FAVICON = (
    "data:image/svg+xml,"
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E"
    "%3Crect width='32' height='32' rx='7' fill='%2314191f'/%3E"
    "%3Cpath d='M5 19h5l3-9 4 14 3-8 2 3h5' fill='none' stroke='%2346d4e6' stroke-width='2.6' "
    "stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E"
)


def wrap_page(fragment):
    head_parts = []
    for tag in ("title", "style"):
        pattern = re.compile(r"<%s\b[^>]*>.*?</%s>" % (tag, tag), re.S | re.I)
        match = pattern.search(fragment)
        if match:
            head_parts.append(match.group(0))
            fragment = fragment[: match.start()] + fragment[match.end():]
    return (
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
        "<meta name=\"color-scheme\" content=\"dark light\">"
        "<link rel=\"icon\" href=\"%s\">%s</head><body>%s</body></html>"
        % (FAVICON, "".join(head_parts), fragment.strip())
    )


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    return "static const uint8_t %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def build():
    assets = []

    with open(os.path.join(WEB_DIR, "index.html"), "r", encoding="utf-8") as f:
        page = wrap_page(f.read()).encode("utf-8")
    assets.append(("/", "text/html; charset=utf-8", gzip.compress(page, 9, mtime=0), True, False, len(page)))

    font_dir = os.path.join(WEB_DIR, "fonts")
    for name in sorted(os.listdir(font_dir)):
        if not name.endswith(".woff2"):
            continue
        with open(os.path.join(font_dir, name), "rb") as f:
            data = f.read()
        assets.append(("/fonts/" + name, "font/woff2", data, False, True, len(data)))

    out = [
        "#pragma once\n",
        "#include <Arduino.h>\n\n",
        "struct WebAsset {\n  const char *path;\n  const char *mime;\n  const uint8_t *data;\n"
        "  size_t len;\n  bool gzip;\n  bool immutable;\n};\n\n",
    ]
    for i, (_, _, data, _, _, _) in enumerate(assets):
        out.append(c_array("kAsset%d" % i, data))
        out.append("\n")
    out.append("static const WebAsset kWebAssets[] = {\n")
    for i, (path, mime, data, gz, immutable, _) in enumerate(assets):
        out.append(
            '  {"%s", "%s", kAsset%d, %d, %s, %s},\n'
            % (path, mime, i, len(data), "true" if gz else "false", "true" if immutable else "false")
        )
    out.append("};\n")
    out.append("static const size_t kWebAssetCount = sizeof(kWebAssets) / sizeof(kWebAssets[0]);\n")
    text = "".join(out)

    old = None
    if os.path.exists(OUT_FILE):
        with open(OUT_FILE, "r", encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(OUT_FILE, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    page_raw = assets[0][5]
    page_gz = len(assets[0][2])
    fonts = sum(len(a[2]) for a in assets[1:])
    print("BenchBuddy web: page %d B -> %d B gzip, %d font(s) %d B" % (page_raw, page_gz, len(assets) - 1, fonts))


build()
