# ESP8266 SNMP Agent

SNMPv1 + SNMPv2c agent for ESP8266 (Arduino/PlatformIO): multi-varbind
GET/GETNEXT/GETBULK/SET, full RFC 2863 ifTable for the WiFi adapter, device
MIB with hardware/heap/flash/WiFi instrumentation, standard traps with
reset-reason varbinds, and an authenticationFailure trap.

Protocol engine: **SNMP_Embedded v3.4.4** (vendored at `lib/SNMP_Embedded`,
MIT), selected for its deterministic memory model (compile-time bounded ASN
pool, zero per-packet allocation), RFC 3416 tooBig handling, runtime stats,
and its 39-case upstream test suite. All device-level value on top of it is
this project's: MIB file, ifTable, device objects, traps, NMS fan-out.

## Layout

| Path | What |
|---|---|
| `mibs/ESP8266-SNMP-MIB.mib` | Device MIB (SMIv2, enterprise 99999 — replace with your IANA PEN) |
| `src/main.cpp` | Firmware: registers all OID handlers against plain C backing variables refreshed each loop; trap logic |
| `lib/SNMP_Embedded/` | Vendored SNMP_Embedded engine (MIT, © 2026 syntax, v3.4.4) + technical manual |
| `tests/` | Catch2 host suite: upstream 39 cases + `esp_device_test.cpp` (type fidelity, multi-VB GET, community enforcement, SET semantics, v1/v2c trap wire formats) |
| `platformio.ini` | Build config; size flags MUST stay global (library one-definition rule) |

Replace `99999` with your IANA PEN (free: iana.org/assignments/enterprise-numbers)
in `mibs/ESP8266-SNMP-MIB.mib` and in the OID literals in `src/main.cpp`.
Trap destinations are the `NMS_ADDRS[]` constants in `src/main.cpp`
(compile-time; UDP/162). Communities: `"public"`/`"private"` in the
`SNMPAgent` constructor — change there.

## Serving tree

Standard groups (no private MIB needed):
- RFC 1213 system group via the engine's one-call helper: `sysDescr`,
  `sysObjectID` (→ enterprises.99999), live `sysUpTime`, `sysName`,
  `sysLocation` (RW via `loc <s>` serial), plus `sysServices`
- RFC 2863 interfaces: `ifNumber` + full `ifEntry.1` for the WiFi STA —
  `ifPhysAddress` (real OCTET STRING MAC), `ifAdminStatus`/`ifOperStatus`
  (`up/down/dormant` per association), `ifLastChange` (ticks at transition),
  typed Counter32 columns, `ifSpeed`, `ifType` `ieee80211(71)`,
  `ifSpecific` (handled as empty octets — see "Deviations")

Private tree `1.3.6.1.4.1.99999`: device identity (model, firmware, chipId,
hardware string `ESP8266EX Xtensa LX106`, CPU MHz, SDK version), heap health
(free/max-block/fragmentation, RW `espDeviceHeapLimit`), control (RW LED,
RW `espDeviceReset` — write 1 to reboot), `espDeviceResetReason`,
`espDeviceAuthFails`, WiFi state/SSID/channel/RSSI/BSSID/IP, flash
geometry/speed. ~25 walkable objects total.

Counters (`ifInUcastPkts`, `ifInDiscards`, authFails, ...) are fed from the
engine's runtime stats and the request loop — they measure the SNMP
management plane. The ESP8266 Arduino lwIP port routes WiFi RX/TX through
the closed-source SDK blob, which bypasses every observable stats hook
(established by building instrumented lwIP three ways in this project's
history); non-SNMP traffic totals cannot be counted honestly on this
platform, and the zero-valued columns are documented as such in the MIB.

## Traps

Sent to both compiled-in NMS addresses, v2c by default (serial `trapv1` to
switch dialect; `trapv2` back):

| Trap | Trigger | Notes |
|---|---|---|
| coldStart / warmStart | boot, once WiFi can carry UDP | cold: power-on/watchdog/exception/EN; warm: `Software/System restart`, `Deep-Sleep Wake`. Carries `espDeviceResetReason` string varbind (Cisco-style: standard trap OID + vendor reason varbind) |
| espTrapLowHeap | free heap < `espDeviceHeapLimit` | latched once per crossing, re-arms at limit+1 KiB; varbinds free heap + limit |
| authenticationFailure | bad-community datagram | standard `.5.5`; varbind is cumulative `espDeviceAuthFails`; rate-limited to 1/s (flood must not spray the NMS). The offending request itself is never answered on the wire |

## Build & flash

```
pio run                 # build (RAM ~43.6% incl. engine pool)
pio run -t upload        # flash NodeMCU on /dev/cu.usbserial-0001
pio device monitor       # 115200
```

WiFi credentials via build flags in `platformio.ini` (already set for this
device). Size overrides (`SNMP_MAX_CALLBACKS_PER_AGENT`, packet length,
varbind cap) are GLOBAL build flags by the library's one-definition rule —
a sketch-local `#define` desyncs class layout and boot-loops the ESP8266.

Serial commands: `loc <string>` (sysLocation), `trapv1`/`trapv2`.
Serial console etiquette on NodeMCU: opening the port can toggle DTR and
reset the board — use `pio device monitor` or hold DTR/RTS low.

## Tests

Host suite (no hardware):

```
cd tests && make test
```

45 cases / 2442 assertions: upstream engine suite + device-semantics cases
(wire-type fidelity per callback class, multi-varbind GET up to the
configured cap, community enforcement incl. reject counter, SET semantics,
v1/v2c trap wire formats hex-verified). `make ci-test` cleans first.
NOTE: SET is per-varbind in this engine (a failed varbind answers
`READ_ONLY`/`NO_ACCESS` at its index; earlier varbinds' writes stand) — the
tests pin this engine behavior; there is no all-or-nothing rollback.

Live interop (net-snmp against the device):

```
snmpwalk -v2c -c public <ip> ifTable
snmpwalk -v2c -c public -m ALL -M ~/.snmp/mibs:<path-to-this-mib> <ip> ESP8266-SNMP-MIB::espSnmpMib
snmpset  -v2c -c private <ip> ESP8266-SNMP-MIB::espDeviceLedState.0 i 1
snmpbulkwalk -v2c -c public <ip> .1.3    # works out of the box (local truncation patch)
```

## Deviations from upstream (local, marked in code)

- `lib/SNMP_Embedded/src/SNMP_Embedded.h`: public `addPrebuiltHandler()`
  wrapper over private addHandler — registers prebuilt custom callbacks
  (raw-octet MAC / IpAddress / empty-octet types) that no factory covers.
- `src/main.cpp` custom `ValueCallback` subclasses serve IEEE 802.11f MAC
  bytes as OCTET STRING and the IP as real IpAddress (application tag 0x40).
- `ifSpecific` is served as an empty OCTET STRING: RFC 2863's defined value
  is zeroDotZero, but the engine's OID encoder requires `.1.3.`-rooted OIDs,
  so the OID-typed value is unencodable; net-snmp flags the type mismatch
  (non-fatal, walk continues; the column is deprecated by the RFC).
- GETBULK repeater expansion now truncates at `SNMP_MAX_VARBINDS` per
  RFC 3416 4.2.3 instead of answering tooBig — snmpbulkwalk works out of
  the box, returning 8 varbinds per batch. Local patch, marked
  `LOCAL ESP8266_SNMP PATCH` in `lib/SNMP_Embedded/src/SNMPZeroCopy.cpp`
  (stageGetBulk) and `lib/SNMP_Embedded/src/SNMPPDUHandler.cpp`
  (handleGetBulkRequestPDU). Plain GET with more varbinds than the cap
  (8) is still dropped silently — upstream behavior, unchanged; NMS
  panels (4-6 objects) and snmpwalk are unaffected.

## Operational limits

- Communities are cleartext (v1/v2c nature). Trusted networks only.
- Trap targets are compile-time constants; change and reflash to move them.
- GETBULK walks the tree in batches of up to 8 varbinds (`SNMP_MAX_VARBINDS`)
  — the local truncation patch (see Deviations) keeps snmpbulkwalk going
  where upstream would abort the walk with tooBig responses.
- `espDeviceReset` SET takes effect within one loop (~ms).

## License

MIT (see LICENSE), Copyright (c) 2026 ThomasStolt — applies to all files
except `lib/SNMP_Embedded`, which is vendored from SNMP_Embedded v3.4.4
(github.com/syntax1269/SNMP_Embedded) and remains under its own MIT
license, Copyright (c) 2026 syntax (lib/SNMP_Embedded/LICENSE). Local
modifications inside `lib/SNMP_Embedded/src/` are marked
"LOCAL ESP8266_SNMP PATCH" and are MIT-licensed derivative works of that
library; clean upstreamable copies live in `patches/`.
