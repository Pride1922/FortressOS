# Battery telemetry: withdraw fabricated hardware readings — 2026-11-02

The user booted the published live USB and photographed `battery` reporting
100% (Full), AC Connected and 25 C while the laptop was physically unplugged
and known not to be full. This is a physical FAIL, not successful ACPI telemetry.
The exact firmware branch has not been traced from a table dump.

Code review found three invalid sources of supposedly live measurements:

- Device/method detection with unavailable status invented 100%/Full when the
  scanned adapter value was online, and 80% otherwise.
- The hardware fallback assumed EC addresses 0x10..0x18 were standard battery
  registers without a verified platform mapping. That map belongs only to the
  synthetic host fixture; reading those bytes does not establish their meaning.
- Raw AML byte scanners searched for packages/ReturnOp constants without
  namespace lookup, control-flow execution, EC fields or method dependencies.
  They can select an unused fallback constant as an apparent live reading.

There is no valid production measurement backend. The complete hardware polling
path using those sources is removed. The limited parsers remain offline/test
helpers, not a live AML engine. SYS_BATTERY_GET now returns EOPNOTSUPP without
writing output, querying guessed EC registers or claiming that no battery is
installed. The CLI reports `Battery telemetry unavailable: ACPI method execution
is not supported`. The existing compositor handles the error by omitting the
battery status/icon; its Power settings and shutdown/reboot remain available.
The ABI and actual ACPI shutdown/reset paths are unchanged.

The status math also stops inferring adapter presence from battery state, and
95% idle capacity no longer means Full. A failed fixture refresh invalidates its
cached sample and returns EIO instead of returning stale data as success. The
null-output error now uses the actual EFAULT constant rather than literal -14,
which in FortressOS means EOPNOTSUPP.

## Verification and missing implementation

`make test-battery-host` includes the existing synthetic parser/math/cache suite,
new stale-refresh and AC-unknown assertions, and a second executable compiling
the driver **without HOST_TEST**. The production test verifies repeated unsupported
reads, untouched output, null-output validation and no timer/EC polling. It uses
ASan/UBSan where available. It exercises the previously excluded production
branch; passing fixture tests alone did not validate the removed hardware path.

Accurate telemetry remains unimplemented. It requires a real namespace-aware
AML execution backend, validated EC OperationRegion access/serialization and
actual _BST/_BIX or _BIF/_PSR/_TMP execution for the selected firmware devices,
with explicit unavailable/error handling and physical charger/capacity checks.
The current correction fixes false reporting, not laptop battery support. A
QEMU battery command test cannot establish the physical laptop's charge or AC
state. No further reflash is necessary merely to continue Wi-Fi diagnostics.
