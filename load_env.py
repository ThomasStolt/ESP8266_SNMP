# Loads .env (gitignored) and injects WIFI_SSID/WIFI_PASS into the build.
# platformio.ini:  extra_scripts = pre:load_env.py
import os

Import("env")

env_path = os.path.join(env["PROJECT_DIR"], ".env")

creds = {}
try:
    with open(env_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            creds[k.strip()] = v.strip()
except FileNotFoundError:
    env.Exit("ERROR: .env not found next to platformio.ini — see .env.example")

ssid = creds.get("WIFI_SSID")
pw = creds.get("WIFI_PASS")
if not ssid or not pw:
    env.Exit("ERROR: .env must define both WIFI_SSID and WIFI_PASS")

# Values must reach the firmware as C string literals, so wrap in escaped
# quotes: -DWIFI_SSID='"myssid"' equivalent.
env.Append(CPPDEFINES=[("WIFI_SSID", '\\"%s\\"' % ssid),
                        ("WIFI_PASS", '\\"%s\\"' % pw)])
print("load_env.py: WiFi credentials loaded from .env (SSID: %s)" % ssid)
