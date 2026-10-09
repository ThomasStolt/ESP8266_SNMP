# ESP8266_SNMP Project State — for continuation after /clear

## What exists (this repo, committed)

SNMPv1/v2c agent for ESP8266. **Protocol engine: vendored SNMP_Embedded v3.4.4**
(MIT, `lib/SNMP_Embedded/`). **All device value in `src/main.cpp`** (the only
firmware source — old hand-rolled BER/agent/traps were deleted in clean cutover).

- `mibs/ESP8266-SNMP-MIB.mib` — private-root MIB, enterprise 99999
  (placeholder PEN), 20 objects (espDevice 14 + espWifi 6) + boot/lowHeap
  trap definitions. FROZEN OIDs (contract). CPU/memory/disk objects
  MOVED OUT of this MIB into the standard RFC 2790 Host Resources MIB
  (`1.3.6.1.2.1.25`, registered directly in src/main.cpp — hrSystemUptime,
  hrSystemProcesses, hrMemorySize, hrStorageTable RAM+Flash rows,
  hrDeviceTable CPU row, hrProcessorLoad); hrSWRun*/hrFS* DO NOT FIT:
  every column costs one ASN-pool slot; the full set (cap 88) left <2 KiB
  free and WiFi never associated (measured; fixed cap back to 80 after
  on-device OOM crash loops — see sendTrapToAll's OOM guard). Vcc
  (espDeviceSupplyVoltage) removed entirely, no RFC 2790 equivalent,
  along with the ESP_SNMP_ENABLE_VCC flag.
- `tests/` — `cd tests && make test` → 52/52, 2587 assertions (45
  upstream+device cases + RFC2790 multi-varbind GET case). OID arcs:
  hrDeviceTable = 25.3.2, hrProcessor = 25.3.3 (verify with `snmptranslate
  -On HOST-RESOURCES-MIB::hrProcessorLoad` if in doubt). Live walk count
  under `1.3.6.1.2.1.25`: 22 objects.
- `README.md` — current: RFC 2790 serving list incl. hrSystemUptime, the
  hrSWRun/hrFS RAM ceiling, and the trap OOM guard.
- `platformio.ini` — WiFi creds via build_flags (placeholders in the public repo; set locally), communities
  public/private via src/main.cpp SNMPAgent ctor.

## Live hardware state

- NodeMCU on `/dev/cu.usbserial-0001`, flashed with HEAD, answering at
  the device's LAN IP (DHCP; check your router or serial console). RAM 43.6/80 KB, flash 29%.
- Trap sinks: compiled in via `NMS_ADDRS[]` in src/main.cpp (set yours there).
  UDP/162, community `public`, v2c dialect (serial `trapv1` toggles v1).
- Serial 115200. CRITICAL etiquette: opening the port can pulse DTR→EN and
  REBOOT the board; `pio device monitor` is safe, ad-hoc scripts must
  set DTR/RTS low like earlier session scripts did.
- net-snmp 5.9.5.2 at `/opt/homebrew/Cellar/net-snmp/5.9.5.2/bin`
  (`/usr/bin/snmp*` is an old 5.6.2 — use the homebrew one). MIB copy for
  `-M /tmp`: regenerate with `cp mibs/ESP8266-SNMP-MIB.mib /tmp/ESP8266-SNMP-MIB.txt`.
  System SDK MacOSX27.0 is malformed — host g++ needs
  `-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk`.
- ~47 KB free heap live; heap limit (espDeviceHeapLimit) 16384.

## Hard-won quirks (do not relearn these)

1. **OID string format**: direct `OIDType(...)`/`SortableOIDType(...)` and
   trap OIDs REQUIRE leading dot (".1.3.6..."), else OID parse returns
   invalid → trap serialise error -37 → sendTo silently returns false
   (queue helper swallows it). Agent factories (addXxxHandler) accept both.
2. Upstream `sendTo()` failure is silent — trust wire captures
   (temporary setUDPport e.g. 1162 + python UDP listener + hook in
   lib/SNMP_Embedded/src/SNMPTrap.h exposing serialise length), not console
   prints from loop code.
3. lwIP WiFi RX/TX counters are NOT observable (SDK blob bypasses all lwIP
   stats hooks — proven by 3 instrumented-lwIP builds; libs+headers under
   ~/.platformio/.../tools/sdk were replaced with MIB2_STATS-enabled build,
   that archaeology is done, counters stay agent-level). ifTable counters
   = SNMP management plane only. Also: custom-built lwIP libs live in the
   package dir; stock backups at /tmp/stock-*-backup.a (volatile).
4. Engine SET is per-varbind (no all-or-nothing); engine drops >8-varbind
   GETs silently (SNMP_MAX_VARBINDS=8 @ 1400B packets) — upstream,
   unchanged. NMS panels (≤6) unaffected.
5. **GETBULK truncation — local patch**: `lib/SNMP_Embedded/src/
   SNMPZeroCopy.cpp` (stageGetBulk) + `lib/SNMP_Embedded/src/
   SNMPPDUHandler.cpp` (handleGetBulkRequestPDU), marked `LOCAL
   ESP8266_SNMP PATCH (RFC 3416 4.2.3)`: repeater expansion stops and
   returns a truncated response when the response array hits
   SNMP_MAX_VARBINDS instead of answering tooBig. Why: snmpbulkwalk
   then walks the full tree in batches of 8 instead of aborting.
   Device-verified live: snmpbulkwalk .1.3.6.1 walks all 54 objects in
   batches of 8, end-of-view clean (2026-10-06).
6. `addPrebuiltHandler()` in lib/SNMP_Embedded/src/SNMP_Embedded.h is OUR
   extension (upstream has none) — used for RawOctetCallback (MAC),
   IpAddressCallback, ZeroOctetCallback (ifSpecific as empty octets;
   their OID encoder can't do zeroDotZero). Keep the extension marked.
7. v1 traps: engine puts trapOID into the ENTERPRISE field; v1 dialect traps
   in src/main.cpp register ".1.3.6.1.4.1.99999" + generic/specific numbers.
   Verified: coldStart=0/warmStart=1/authFail=4/enterpriseSpecific 6;spec 5.
8. Size flags (SNMP_MAX_CALLBACKS_PER_AGENT etc.) MUST stay in platformio.ini
   build_flags (one-definition rule; sketch-local #define = reboot loop).

## Verified-done (do not redo)

Device interop vs real net-snmp: multi-VB panel GET ✓, 22-row ifTable with
correct types ✓, 20-object named enterprise walk ✓ (25 pre-migration;
CPU/memory/disk now under RFC 2790 hr* tree), SET led + denied writes ✓,
coldStart v2c trap captured byte-for-byte (113 B, snmpTrapOID .5.1 +
"External System" varbind) ✓, authFail trap + watermark 1/s latch ✓,
espDeviceReset reboot via SET ✓. Host suite 45/45 ✓. Clean rebuild ✓.

Possible future work (nothing pending): stricter PEN registration (replace
99999 private root — the tree is now identity/heap/WiFi only; CPU/memory/
disk live under the standard RFC 2790 `.1.3.6.1.2.1.25` tree), snmptrapd
runbook for the second NMS, OTA/partition awareness for the Flash
hrStorage row's used value (sketch size; hrStorageSize counts whole chip).
