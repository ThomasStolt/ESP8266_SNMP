# GETBULK: return a truncated response per RFC 3416 §4.2.3 instead of `tooBig`

## Summary

On v3.4.4, a GetBulk request whose repeater expansion exceeds the compile-time
varbind cap (`SNMP_MAX_VARBINDS`) is answered with a `tooBig` error PDU. RFC 3416
§4.2.3 instead requires the responder to return a truncated response when hit by
a local constraint, and the `tooBig` error-status is not among the outcomes
defined for GetBulk. The practical effect: `snmpbulkwalk` aborts a walk that the
library could have served.

## Reproduction

Firmware: ESP8266 with the default platform auto-tune caps (see `defs.h`: on
ESP8266 `MAX_SNMP_PACKET_LENGTH` defaults to 1024, and `SNMP_MAX_VARBINDS` is
derived as (1024 − 40) / 160 = 6), plus a handful of OIDs registered under
`.1.3.6.1.2.1.1`. Any NMS:

```
$ snmpbulkwalk -v2c -c public <ip> .1.3.6.1.2.1.1
```

`snmpbulkwalk` defaults to `max-repetitions = 10`, which exceeds the cap of 6.
The agent answers with an empty-varbind `tooBig` PDU (errStatus=1); observed on
the device, the walk then aborts with `Response message would have been too
large` and exits non-zero. `snmpbulkwalk` treats the explicit `tooBig`
error-status as terminal and does not re-request with a smaller
max-repetitions. The equivalent GETNEXT-based walk against the same agent
completes without errors.

Any `max-repetitions > (SNMP_MAX_VARBINDS − N)` on a repeater varbind
reproduces it; with the ESP8266 defaults, the stock `snmpbulkwalk` invocation
above does.

Two code paths share the behavior:

1. **Classic path** (`SNMPParser.cpp` → `SNMPPDUHandler.cpp`
   `handleGetBulkRequestPDU`): `appendResponseVarBind()` returns false when
   `outResponseCount >= SNMP_MAX_VARBINDS`; the caller sets `*outOverflow = true`
   and returns false; `SNMPParser.cpp` then emits `globalError = TOO_BIG` and
   responds with the empty-varbind error PDU. The non-repeaters loop has the same
   overflow behavior.
2. **Zero-copy path** (`SNMPZeroCopy.cpp` `stageGetBulk` → `ResponsePlan::add`):
   when `plan.count >= SNMP_MAX_VARBINDS`, `add()` returns false;
   `stageGetBulk` returns false; the dispatcher (SNMPZeroCopy.cpp ~line 566) sets
   `globalErrorStatus = TOO_BIG` and sends the alternate empty PDU.

## Why `tooBig` is wrong here

RFC 3416 §4.2.3 defines three reasons a GetBulk response may be generated with a
lesser number of variable bindings, and truncation from a local constraint is
the first one:

> If the size of the message encapsulating the Response-PDU containing the
> requested number of variable bindings would be greater than either a local
> constraint or the maximum message size of the originator, then the response is
> generated with a lesser number of variable bindings. This lesser number is
> the ordered set of variable bindings with some of the variable bindings at
> the end of the set removed [...]

The varbind cap is exactly such a local constraint, and because
`SNMP_MAX_VARBINDS` is derived (defs.h) so that a cap-max *worst-case* response
always fits `MAX_SNMP_PACKET_LENGTH`, a truncated (or even cap-max) response is
guaranteed serializable. §4.2.3 reserves `genErr` for per-varbind processing
failures and `noError` for the successful (possibly truncated) response; a
GetBulk responder never answers `tooBig` for a capacity constraint.

In the field, the `tooBig` answer is strictly worse than the truncation it
replaced (the v3.4.4 comment in `SNMPParser.cpp` says this path was previously a
silent truncation): `snmpbulkwalk` prints its message and aborts. It does not
re-request with smaller max-repetitions, and it's not a packet the agent couldn't
fit — the agent could have responded with the same contents minus the trailing
varbinds. The boot-time walk of a monitoring system therefore fails against a
device that answers ordinary GET/GETNEXT fine, and the reported error directs
the user to a firmware-side fix (raise `SNMP_MAX_VARBINDS`) that is not always
possible on ESP8266 RAM budgets.

## Proposed fix

Break at the cap in both paths and return what fits, with per-varbind error rows
left as-is:

- `SNMPZeroCopy.cpp stageGetBulk`: at the top of each repetition iteration,
  `if(plan.count >= SNMP_MAX_VARBINDS) break;` — plan full is now normal
  truncation, not a `false` return.
- `SNMPPDUHandler.cpp handleGetBulkRequestPDU`: same check in the repeaters loop
  using `outResponseCount`. The non-repeaters loop is left untouched: it
  processes at most `varbindCount ≤ SNMP_MAX_VARBINDS` requests (parse-side
  rejects anything larger), so it cannot overflow once the repeaters loop
  breaks at the cap.

The result satisfies the RFC: the response ends with a regular `noError` PDU whose
varbind list is truncated at the cap — the client sees `M_eff < M` and paginates
exactly as it would against a size-constrained full agent.

## Backwards compatibility

- The overflow mechanism is retained: `handleGetBulkRequestPDU` still takes
  `*outOverflow`, the non-repeaters and per-varbind GEN_ERR failure branches
  still set it, and the dispatcher's `TOO_BIG` mapping for those cases is
  unchanged. Only the "repeater expansion alone exceeded the cap" case no
  longer takes that path; a caller that would have seen tooBig there sees a
  truncated `noError` response instead, which is what the RFC prescribes.
- Parse-side rejection (SNMPPacket.cpp) of *requests* with more varbinds than
  `SNMP_MAX_VARBINDS` is untouched — only response generation changes.

Attached patches: `0001-getbulk-truncate-per-rfc3416.patch` (both paths),
`0002-addPrebuiltHandler.patch` (separate, unrelated API extension for
registering prebuilt ValueCallback instances; included for completeness).
