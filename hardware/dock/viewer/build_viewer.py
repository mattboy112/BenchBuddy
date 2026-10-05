import base64, json, os
here = os.path.dirname(os.path.abspath(__file__))
data = {}
for f in sorted(os.listdir(os.path.join(here, "stl"))):
    if f.endswith(".stl"):
        with open(os.path.join(here, "stl", f), "rb") as fh:
            data[f[:-4]] = base64.b64encode(fh.read()).decode()
tpl = open(os.path.join(here, "viewer_template.html"), encoding="utf-8").read()
fragment = tpl.replace("/*MESHES*/", json.dumps(data))
page = ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
        "</head>\n<body>\n" + fragment + "\n</body>\n</html>\n")
open(os.path.join(here, "BenchBuddy_Dock_viewer.html"), "w", encoding="utf-8").write(page)
if len(os.sys.argv) > 1:
    open(os.sys.argv[1], "w", encoding="utf-8").write(fragment)
print("viewer", round(len(page) / 1e6, 2), "MB")
