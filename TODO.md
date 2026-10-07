# HID and adaptive-trigger handoff

This is the active Linux HID plan for work from the Cheryl checkout.
Work on the existing `extern/gainput` submodule; do not clone another copy. The goal
is reliable Sony controller HID integration followed by usable DualSense adaptive
triggers in Cheryl. The [integration contracts](HID.md) define the selected identity,
source and feedback design; unfinished phases below do not advertise capabilities.

## Start here

Read Cheryl's [AGENTS.md](../../AGENTS.md) before editing. Inspect Git status and
relevant pre-existing diffs in both repositories, and preserve unfinished user work.
Use these documents for current contracts and outstanding acceptance:

- [Native input lifetime and mapping](../../projects/modules/platform/native-glfw/README.md#input-lifetime-and-mapping).
- [Controller backend limits and capability scope](../../projects/modules/platform/native-glfw/README.md#controller-backend-limits).
- [Cheryl native input plan](../../docs/planning/develop-review-and-development-plan.md#native-input-lifetime-safety).
- [User testing queue](../../docs/testing-requests.md).

Gainput changes belong in this submodule. Adapter, portable API, demo, integration
tests and testing requests belong in Cheryl. Keep Gainput independent of Cheryl.
Cheryl's [.gitmodules](../../.gitmodules) selects the user's personal Gainput fork;
inspect the current branch/remotes rather than assuming a branch name or changing
upstream configuration.

Do not build, run tests or push unless explicitly authorized. Submit deferred tests
to Cheryl's testing queue. User command blocks establish the Cheryl root regardless
of the document's working directory, use Ninja for CMake configuration, and use
normal priority and available parallelism. Resource-sharing limits apply to agent
commands. Keep normal builds free of deliberately triggered compiler warnings.

## Starting evidence and scope

The Linux joystick correction is commit
`dd3f7468bfd7c7042aa885a38276516e9a364f00`. It translates kernel button/axis maps for
unknown controller dialects, updates the created pad's state and device ID, and
clears/reloads state on disconnect/reconnect. Bluetooth DualSense physical QA is
accepted on Linux with HID disabled: mapped buttons/axes, A press behavior,
off-center disconnect/reconnect, connected startup and attachment after startup in
both runtimes. Automated joystick regressions remain in the testing queue. This
does not accept USB or HID behavior. Preserve the joystick path as a separate
usable input source and regression baseline; its reusable
[controller checks](../../docs/development/native-desktop-checks.md#linux-joystick-controller-checks)
retain the physical procedure.

The original missing Cross/A button came from joystick translation. The earlier
HID-enabled CMake selection did not define the compiler guard for HID startup and
polling. Do not interpret that failure as evidence that hidapi broke the controller.

The HID code arrived in `f81fe97` as an import from The Forge. This fork's
[README](README.md#2023-fork-of-jochumdev) describes an incomplete standalone port.
PS4/PS5 USB/Bluetooth parsing and conventional rumble/light packet code exist, but
the normal Gainput pad bridge is unfinished. The parsers emit the fixed logical
device ID `4`; Cheryl normally creates its pad as ID `2`. HID output forwarding in
the pad API is commented out. DualSense trigger packet fields exist, but
`HIDDoRumbleTriggersPS5Controller` is an empty disabled stub.

The initial end-to-end target is DualSense input and adaptive triggers on Linux over
USB and Bluetooth. Windows/macOS/other-platform testing is shelved in Cheryl's
[long-term platform plan](../../docs/planning/platform-acceptance.md). Shared contracts
remain portable, but Linux development does not wait for those platforms. Touch,
motion, battery exposure and other DualSense features need their own accepted
contracts before being advertised. Both existing parsers route each touch contact's
X/Y through one axis ID; retain that discovered correction as follow-on work if it
is outside the selected feature scope.

## Ordered implementation

Activate one phase at a time and keep its concise progress checklist here. Future
phases remain at the following roadmap detail. Commit coherent development units
independently, with focused regression sources and testing requests in each unit.
Resolve invalidated assumptions and required correctness work before accumulating
dependent implementation. Distinguish source completion from executable acceptance;
keep deferred execution visible in the testing queue.

The connection-tracker prerequisite precedes phases 2–3. Source fixes and synthetic
coverage are submitted for acceptance; runtime activation waits until pad identity,
state and source selection agree.

### Active foundation: contracts and connection lifecycle

- [x] Inspect native/HID ownership and select pad identity, physical association,
  fallback, connection generations and portable feedback contracts in [HID.md](HID.md).
- [x] Correct exact-path tracking, empty enumeration, slot/counter reset and capacity
  handling; close failed connections and retry failed opens without phantom callbacks.
- [x] Make Linux notification setup release partial resources and keep periodic
  enumeration available after notification or open failures.
- [x] Add an isolated runner with the real tracker/whitelist/Sony parsers and synthetic
  transport, notification setup and time; retain HID-OFF joystick coverage.
- [x] Submit the runner with the existing Linux automated request and reconcile
  Cheryl's gitlink/current limits without enabling the unfinished report bridge.
- [ ] Accept the lifecycle regressions through user-run Linux execution. Physical
  report/state and trigger acceptance remain prerequisites of later phases.

### 2. Repair feature selection and startup/notification lifetime

Start with [CMakeLists.txt](CMakeLists.txt), [lib/CMakeLists.txt](lib/CMakeLists.txt)
and [InputManager](lib/source/gainput/GainputInputManager.cpp).

Make the Linux HID CMake selection, source/dependency selection and compiler guards
agree for ON and OFF configurations. The current code uses `defined(GAINPUT_ENABLE_HID)`;
defining it to zero does not disable that guard. Linux Bluetooth requires hidapi's
hidraw transport; verify the actual selected target, not a libusb include directory.

Handle `HIDInit` failure and partial cleanup explicitly. Gate manager polling on
backend readiness. Windows message dispatch/readiness and the original notification
window remain deferred platform work. Windows currently calls the HID message
handler independently of the startup guard and filters whitelisted Sony
pads out of DirectInput. The macOS native pad leaves Sony support to HID, while the
Apple CMake branch omits the shared runtime/parsers. Resolve those platforms'
fallback ownership/source selection before advertising them.

### 3. Integrate report state, availability and reconnection

Start with [HID lifecycle](lib/source/gainput/hid/GainputHID.cpp),
[normal pad delegation](lib/source/gainput/pad/GainputInputDevicePad.cpp) and the
[PS4](lib/source/gainput/hid/hidparsers/HIDParserPS4Controller.cpp)/
[PS5](lib/source/gainput/hid/hidparsers/HIDParserPS5Controller.cpp) parsers.

Route reports to the associated pad, update its retained state and emit deltas under
that pad's actual ID. Make availability and control validity agree with the selected
HID source. Verify the manager's update order so retained held controls and listener
transitions describe the same completed poll.
Honor explicit pad indices while establishing those associations: the current pad
constructor converts every non-`AutoIndex` index to zero. Independent pad bindings
must not alias the first native joystick or an already-owned HID connection.

Use the tracker's exact-path, empty-enumeration and read-failure retirement behavior
defined in [HID.md](HID.md). Disconnect releases held controls
and invalidates the old connection; reconnect binds fresh state without delivering
old input or output to a reused slot. Preserve native joystick behavior when selected.

Add opt-in observations for initialization outcome, enumeration/open, source
selection, report delivery and disconnect/reconnect. Cheryl's existing controller
trace does not establish those HID stages. Avoid requiring Cheryl's logger inside
Gainput or recording keyboard/text input as part of controller diagnostics.

### 4. Complete output routing and implement adaptive triggers

Connect pad output APIs to their associated physical controller. Correct the HID
rumble timeout's seconds/milliseconds mismatch and make failed writes observable.
Define ownership of persistent output state so rumble, light and trigger updates
do not inadvertently overwrite each other.

Implement left/right trigger effect encoding and enable bits, initially covering
clear, resistance and resistance/vibration effects. Validate parameters and support
explicit clearing. Reuse the existing USB/Bluetooth packet transport after checking
its framing, enhanced-report setup and Bluetooth checksum handling. Useful primary
references are SDL's
[PS5 driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c)
and [controller effect examples](https://github.com/libsdl-org/SDL/blob/main/test/testcontroller.c).
Trigger effects are new implementation, not completed by uncommenting the stub.

Clear active effects while the device is writable when stopping/deinitializing;
discard connection-specific desired output after disconnect and prevent stale
commands from applying to a different/reconnected controller. Provide capability
queries and an explicit unsupported result for controllers without this feature.

### 5. Expose the feature through Cheryl

Implement the agreed contract through the
[portable input interface](../../projects/engine/include/cheryl/core/controls/input-interface.h)
and [native adapter](../../projects/modules/platform/native-glfw/src/core/controls/input-system.cpp).
The existing interface has no controller feedback API. Respect platform-thread
ownership; simulation use must follow the defined command handoff rather than
mutating the borrowed Gainput manager from another thread.

Add a small capability-aware demo control for selecting/clearing trigger effects.
Verify ordinary button mapping, held state and press counting with effects enabled
in both sequential and concurrent runtimes. Keep controller identity and output
capability details in the authoritative module/API documents.

### 6. Submit and accept focused regressions and physical QA

Automated coverage needs arbitrary assigned pad IDs, independent controllers,
callback/retained-state agreement, no duplicate reports, initialization/open/read
failure, held-state clearing and fresh reconnection. Exercise effect encoding,
parameter validation, clear behavior, output coexistence, correct timing and
connection identity with synthetic reports/write capture. Keep existing joystick
checks as HID-OFF coverage; they cannot establish HID decoding or output acceptance.

Extend Cheryl's [testing queue](../../docs/testing-requests.md), including the
blocked HID lifecycle request once its observation harness has runnable setup.
Provide automated command blocks with required CMake configuration and concise QA
requirements. Physical USB/Bluetooth QA verifies actual trigger resistance/effects,
clearing, coexistence with rumble/lights, held-input disconnect/reconnect, orderly
shutdown and fresh startup. Record the tested platforms/configurations; unselected,
skipped or device-free checks do not accept missing hardware behavior.

## Budget, commits and completion

The prior planning estimate is 10-20% of a full weekly allowance for the Linux
scope, or 20-35% for all three desktop platforms, under the session's model/settings.
It includes expected QA fixes and is not a measured cap. Recalibrate against current
usage and respect the user's daily/weekly limits at coherent phase boundaries.

Commit only separable task work using Cheryl's required per-command Git identity
and message conventions. Commit Gainput changes in the submodule and update
Cheryl's gitlink plus its affected contracts/plans/testing requests as a corresponding
unit. Leave publishing to an explicitly authorized push; do not change remotes to
make a local submodule revision appear published.

As phases finish, retain their checklist while the owning task remains open. Move
durable contracts and reusable validation procedures into the appropriate guides;
remove completed handoff detail instead of accumulating an execution log. Completion
requires the selected platform/configuration acceptance as well as finished source.
