"""
Downloads Chart.js and Socket.IO client into static/js/ for fully local serving.
Run once; subsequent runs skip already-downloaded files.
"""
import os
import urllib.request

BASE   = os.path.dirname(os.path.abspath(__file__))
JS_DIR = os.path.join(BASE, "static", "js")
os.makedirs(JS_DIR, exist_ok=True)

FILES = {
    "chart.min.js":     "https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js",
    "socket.io.min.js": "https://cdn.socket.io/4.7.4/socket.io.min.js",
}

for name, url in FILES.items():
    path = os.path.join(JS_DIR, name)
    if os.path.exists(path):
        print(f"  ok  {name}  (cached)")
    else:
        print(f"  dl  {name} ...")
        urllib.request.urlretrieve(url, path)
        print(f"      saved to {path}")

print("Static assets ready.\n")
