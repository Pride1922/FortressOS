# Doom on FortressOS

2026-11-05. Proposed implementation plan, pending review.

## Objective

Run a Doom engine as an ordinary desktop application on FortressOS, using
Freedoom game data. The first playable release must open a level, accept
keyboard controls, coexist with other desktop windows, and exit cleanly.

The initial target is the Dell Latitude 5590. QEMU provides reproducible
integration checks, and the Proxmox VM provides an additional compatibility
check when available. Physical acceptance is reported separately.

## Proposed implementation

Start by evaluating a pinned revision of doomgeneric. Its platform interface
provides initialization, frame presentation, sleep, elapsed milliseconds, key
events, and window title hooks. Its normal build also depends on C library
services. Inventory those dependencies before selecting the final port.

Use the existing external desktop client protocol and shared pixel buffer.
Keep the engine in its own unprivileged Ring 3 process. The compositor retains
display ownership and routes input through the existing window focus policy.

Use a fixed 320×200 game render target initially, with integer enlargement
and letterboxing inside the client content area. Verify aspect treatment
against the selected engine and offer a consistent presentation. The renderer
must respect negotiated dimensions, stride, mapped size, and clipping.

Audio and music form a later project after the required audio-device and
userspace APIs have been assessed. Mouse look and multiplayer are later
milestones following keyboard gameplay.

## Existing FortressOS foundations

- `src/include/desktop_proto.h` defines the 64-byte external-client messages,
  inherited request/event/pixel descriptors, content configuration, damage,
  title, close, and key events.
- Key events include physical keycodes, modifiers, and press/release/repeat
  state. ASCII alone cannot represent held game controls.
- `user/libui/src/client.rs` provides an existing client runtime and heap
  setup. Reuse its protocol behavior when implementing the C engine adapter.
- `SYS_CLOCK_GETTIME`, `SYS_NANOSLEEP`, and `SYS_POLL` provide timing,
  sleeping, and event readiness.
- `user/compositor/src/app_registry.rs` contains the current application
  catalog and application/window identity. Integrate through this catalog and
  its existing launch path.
- FortressOS tools use syscall wrappers and a freestanding runtime. A complete
  hosted C library is an implementation prerequisite to investigate.

## Stage 0 — Feasibility and pinned inputs

1. Select an exact doomgeneric revision and a matching Freedoom release.
   Record source identifiers, download locations, hashes, and license notices.
2. Run that pair on a host platform to check menus, the first level, demo
   playback, and relevant engine limits. Record precisely which content passes.
3. Inventory required C functions, allocation, math, file access, exit paths,
   compiler assumptions, and static constructors.
4. Inspect compile-time stack usage and the deepest runtime call chains.
   Apply Gate G1 and the current userspace stack limit. Audit floating-point
   or SIMD dependencies against FortressOS's current CPU-state contracts.
5. Measure engine ELF size, memory use, WAD size, and proposed artifact size.
   Current limits include a 4 MiB executable limit and roughly 8 MiB PKG
   artifact payload budget. Verify these in the current producer and admission
   code before packaging.

Deliver a dependency/compatibility report and selected source/data identities.
If the candidate requires a kernel contract change, package-format expansion,
or a stack-budget exception, present the concrete requirement for discussion.

## Stage 1 — Minimal platform and runtime adapter

Implement only the C services required by the selected engine, reusing current
syscall wrappers and allocator facilities where suitable.

- Bounded allocation with overflow checks and explicit allocation failure.
- File reads, seek, size queries, and checked short-read/error handling.
- Monotonic millisecond time and sleeping through the existing time APIs.
- Local configuration and save paths under the launching user's home.
- Errors and orderly exit with useful diagnostics.

Keep engine modifications small and retain a reproducible upstream patch.
Measure stack frames, total stack depth, heap high-water use, and executable
size. Runtime adapters must not report success for unsupported operations.

Acceptance requires loading the selected WAD, reaching engine initialization,
and producing a framebuffer in a controlled host/disposable-guest test.
Exercise missing, truncated, and malformed game data and allocation failures.

## Stage 2 — Window, frame presentation, and lifecycle

Implement the platform hooks for the existing desktop transport.

- Create an external application window and map its inherited SHM buffer.
- Convert the engine's actual pixel format to desktop `0xAARRGGBB`.
- Scale and clip the game framebuffer using the latest configuration event.
- Submit bounded damage messages and handle full event/message transfers.
- Process window closure and broken compositor pipes promptly.
- Release descriptors, mappings, and allocations on every exit path.

Acceptance requires a visible title/menu or demo, correct resize clipping,
minimize/restore, close/relaunch, and responsive desktop interaction around it.
Record frame time, presentation traffic, and idle CPU consumption.

## Stage 3 — Keyboard gameplay and focus

Translate physical keycodes into Doom input and maintain a bounded held-key
state. Verify arrows, fire, use, strafe, run, Enter, and Escape, including
simultaneous keys and release events. Verify US and AZERTY input.

Audit what happens when a key is held while switching windows, minimizing,
opening the launcher, or invoking the Secure Attention Key. If the current
protocol cannot clear held keys reliably, add the smallest generic focus
mechanism or synthesize key releases in the compositor's existing input path.
Verify the same behavior for existing clients.

Use the engine's expected tick cadence with a monotonic clock and bounded
catch-up after pauses. Sleep when idle. Timing must stay stable while the
desktop, SSH, and networking remain usable.

Acceptance requires starting a level, walking, turning, firing, using doors,
opening the menu, switching away and back without stuck keys, and quitting.

## Stage 4 — Content delivery, saves, and application integration

Package the executable through the normal signed application pipeline.
Add an application entry and icon through the existing registry. The game
runs with the launching user's identity.

Treat WAD delivery as a separate size decision. For the first developer
acceptance, use an explicitly supplied absolute `-iwad` path to a verified
Freedoom file in the user's game directory. Document acquisition and hash
verification. Before public packaging, select a reproducible data-install
method that fits existing limits or present a separate proposal.

Choose a default per-user game-data location for launcher use. Missing data
must produce an actionable message. Keep mutable saves/configuration in a
per-user directory, separate from package-owned executables and assets.

Test saving, quitting, relaunching, loading, reboot persistence, and
configuration permissions. Define a durable replacement policy for existing
saves and test the failure paths before claiming save integrity.

Acceptance requires install, launch, close, relaunch, upgrade, and uninstall
without overwriting the user's saves. Include upstream license notices and
the matching engine source/build instructions in the release materials.

## Stage 5 — Automated acceptance

Run focused host verification for adapters, input translation, pixel conversion,
clipping, clock wraparound, and file-error handling. Use a deterministic demo
or scripted input sequence with pinned engine/data identities for reproducible
engine output checks.

Run disposable BIOS and UEFI QEMU campaigns at SMP=1 and SMP=4. Verify gameplay
startup, simultaneous keys, focus changes, resize, close, repeat launch, and
save/load. Include desktop coexistence and existing-client regressions for
any shared protocol/input changes.

Measure rather than assume performance. Report engine tick behavior, median
and high-percentile frame times, CPU use, memory high-water use, and resource
counts after repeated launches. Set release targets from the measured baseline.

## Stage 6 — Dell acceptance

Guide the operator through one test at a time.

1. Launch from the desktop and start the agreed first level.
2. Move, turn, fire, use a door, and test multiple held keys.
3. Hold movement, switch to another app, release the key, then return.
4. Minimize/restore, move/resize, open the launcher, and test the SAK.
5. Save, quit, relaunch, and load. Reboot and load again.
6. Play for ten minutes while checking responsiveness, rendering, and stability.
7. Repeat launch/close and review diagnostics for faults or resource growth.

Finish with an acceptance record containing the exact commit, package,
engine/data hashes, display scale, measured performance, and operator results.
Run the same functional subset on Proxmox when available.

## Milestone and delivery sequence

The first milestone is Stage 0 plus the minimal Stage 1 proof. It resolves
engine compatibility and runtime feasibility before desktop/gameplay work.

Later commits separate rendering/lifecycle, keyboard/focus, content/saves,
and packaging. Each packaging handoff identifies the exact commit and
automated evidence. Repository publication and physical deployment follow
the user's requested release workflow.

## Source references

- [doomgeneric platform hooks and porting overview](https://github.com/ozkl/doomgeneric)
- [doomgeneric platform header](https://github.com/ozkl/doomgeneric/blob/master/doomgeneric/doomgeneric.h)
- [doomgeneric build dependencies](https://github.com/ozkl/doomgeneric/blob/master/doomgeneric/Makefile)
- [doomgeneric license](https://github.com/ozkl/doomgeneric/blob/master/LICENSE)
- [Freedoom manual and source-port guidance](https://github.com/freedoom/freedoom/blob/master/manual/freedoom-manual-en.adoc)
- [Freedoom redistribution notices](https://github.com/freedoom/freedoom/blob/master/COPYING.adoc)
