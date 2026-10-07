# HID integration contracts

## Current boundary

The shared HID tracker owns enumerated connection paths, open parser handles and
device-change callbacks. Controller slots are internal one-based handles, not
Gainput `DeviceId`s. A slot may be reused after removal. The normal pad bridge,
runtime feature wiring and adaptive-trigger output remain unfinished; use the Linux
joystick backend with HID disabled until the selected integration is complete.
The [implementation plan](TODO.md#ordered-implementation) owns that work and Cheryl's
[testing queue](../../docs/testing-requests.md) owns pending executable acceptance.

Tracking matches vendor/product and the complete path. Each enumeration retires
unobserved connections, including a null enumeration result; hidapi cannot
distinguish an empty list from enumeration failure through that return value.
Read failures close the old handle before a later poll can reopen it. Rechecks run
every four seconds as well as after Linux notifications, retrying unopened devices.
Only successfully opened parser handles announce connection or disconnection;
`HIDControllerConnected` establishes an open handle, not usable report delivery.
Linux notification setup cleans up partial resources and falls back to rechecks.
Teardown resets slots/counters and drops the callback before a new session.

The isolated `tests-native-hid` runner compiles the real tracker, whitelist and Sony
parsers against synthetic hidapi/udev/time fixtures. It exercises connection
lifetime and parser startup failures without a display/controller or installed
hidapi headers. It does not decode successful input reports, accept a system
hidapi ABI, establish physical identity/generations or validate output effects.
Those remain later implementation/acceptance work.

## Selected identity and reporting design

The following contracts guide the remaining implementation; they are not exposed
capabilities yet. The initial platform is Linux over USB and Bluetooth. Windows,
macOS and other platform acceptance are
[deferred](../../docs/planning/platform-acceptance.md).

- Each normal `InputDevicePad` keeps the ID assigned by `InputManager::CreateDevice`.
  Bind an opened HID connection to that pad, and send both retained-state updates
  and listener deltas through its actual ID. Internal HID slot numbers, player
  numbers and the old parser constant `4` never substitute for that ID.
- A live connection has an exact enumeration path and a nonzero generation issued
  by the owning backend session. Reopening the same path creates a new generation;
  neither path nor slot reuse validates an old binding or output command. Session
  identity also prevents commands surviving manager replacement.
- On Linux, correlate hidraw and joydev nodes through their canonical HID ancestor
  in sysfs/udev. Vendor/product IDs and enumeration order are insufficient when
  identical controllers coexist. Resolve that physical association before suppressing
  a native source. If it is unavailable or ambiguous, retain native input and report
  HID selection as unavailable. HID-only connections bind only to an unoccupied
  normal pad; they do not steal a pad already associated with another controller.
- Native input is the fallback when HID is disabled, initialization/open fails,
  the decoder is unsupported or association cannot be established. Promote HID only
  after a usable basic report and unambiguous pad association. Exactly one source
  writes that pad during a completed poll; successful enumeration alone does not
  suppress native input. Preserve the corrected joystick implementation.
- A source change or disconnect clears the former source's held state and retires
  its output binding before admitting the replacement. Reconnection starts with
  fresh parser/state/output data. Poll ordering must publish retained state before
  notifying listeners, including release edges; polling two backends and discarding
  duplicate callbacks afterward is insufficient.

One initialized native owner remains the supported lifetime. Initialization failure
leaves native input available and releases partial HID resources. Shutdown closes
open controllers, releases notification resources and clears retained callbacks and
tracking state. A notification failure may use periodic enumeration; expose that
fallback independently of successful HID initialization.

## Selected feedback design

Feedback addresses the same pad/connection used for HID input. Gainput owns Sony
packet encoding; Cheryl exposes portable values and connection-scoped capabilities,
not raw output bytes or borrowed mutable managers. Initial DualSense trigger modes
are clear, resistance and vibration. Each side has normalized activation position
and strength in `[0, 1]`; vibration additionally has an integer frequency in
`[1, 255]` Hz. Reject nonfinite/out-of-range values before queuing or writing.
Backend quantization must be documented and covered by captured-packet checks when
the encoder is implemented. Rumble, lights and trigger effects have one retained
desired-output state so changing one feature preserves the others.

Capability queries identify the backend session and connection generation. A
command targets that token and replaces the selected trigger's desired effect;
clear is explicit and persists until replaced. Unsupported controllers return an
explicit unsupported result without changing output. Distinguish queue admission
from completed writes; completion reports applied, unsupported, invalid parameters,
stale/disconnected connection or write failure. A successful write means transport
acceptance and does not establish a physical effect.

Gainput calls and writes stay on the platform owner. Cheryl simulation callers use
owned value commands through the existing platform dispatcher, with completion
results and lifetime cancellation. Recheck the token/capability when executing, not
only when submitting. Detachment/shutdown clears effects while writable; disconnect
discards desired output. An old queued command cannot affect a reused slot or a
reconnected controller. Broader feedback, touch/motion and battery contracts remain
separate requirements.
