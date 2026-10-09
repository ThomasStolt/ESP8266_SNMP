#!/usr/bin/env python3
"""Poll every object under the private tree .1.3.6.1.4.1.99999 and the Host
Resources tree .1.3.6.1.2.1.25 on the ESP8266 SNMP agent and print value +
one-line DESCRIPTION from the MIB file (ESP8266-SNMP-MIB for the private
tree, HOST-RESOURCES-MIB for the HR tree).

Usage:  python3 poll_esp_snmp.py [host]   (default 192.168.2.151)
"""
import re
import subprocess
import sys

SNMPWALK = "/opt/homebrew/Cellar/net-snmp/5.9.5.2/bin/snmpwalk"
MIB_DIR = "/Users/tstolt/dev/ESP8266_SNMP/mibs"
MIB_NAME = "ESP8266-SNMP-MIB"
ROOTS = [".1.3.6.1.4.1.99999", ".1.3.6.1.2.1.25"]

# name -> first sentence of DESCRIPTION
DESC = {}
blob = open(f"{MIB_DIR}/{MIB_NAME}.mib").read()
for m in re.finditer(r"(\w+) OBJECT-TYPE\s+.*?DESCRIPTION\s+(.*?)\s+::=", blob, re.S):
    text = " ".join(m.group(2).split()).lstrip('"')
    DESC[m.group(1)] = re.split(r"(?<=[.!?])\s+[A-Z]", text)[0].rstrip('"')

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.151"
for root in ROOTS:
    out = subprocess.run(
        [SNMPWALK, "-v2c", "-c", "public", "-M", f"+{MIB_DIR}", "-m", "ALL",
         "-Os", host, root],
        capture_output=True, text=True, check=True,
    ).stdout

    print(f"=== {root} ===")
    print(f"{'name':32} {'type':>12}  {'value':22}  description")
    for line in out.splitlines():
        if " = " not in line:
            continue
        name, _, rest = line.partition(" = ")
        typ, _, val = rest.partition(": ")
        if not val:
            continue
        label = re.sub(r"\.\d+$", "", name.split("::")[-1])
        print(f"{label:32} {typ:>12}  {val:22}  {DESC.get(label, '(no description)')}")
