# Offline kernel update, first version

The user selected a bootable update USB. This is kernel-only maintenance for a
newly installed, explicitly enrolled two-slot boot layout. Existing installations
without that layout are rejected. Application packages remain on SYS_PKGCTL.

The baseline installer reserves two 8 MiB kernel files and two 512-byte readiness
records in its 64 MiB ESP. Both slots initially contain the same padded ELF and
matching personalized readiness. Four identical Limine configurations expose
installed/recovery entries for each slot; `default_entry` selects the active slot.
The common personalized bootstrap and installed root are immutable to the updater.

The dedicated USB uses the existing installer boot mode, payload validation,
source exclusion and exclusive PID-bound raw lease. Its Ring 3 updater invokes
only enumerate/acquire/read/write/flush/release, never deploy. No new raw-write
permission, syscall, driver fallback, partition change or filesystem mount is
introduced. The physical boot USB is the trust boundary for this first prototype;
it is not an authenticated repository base-update implementation.

Preflight checks disk identity, consistent GPT copies, ESP/root boundaries, FAT
allocation graph, both existing readiness records, identical exact menu copies
and the expected baseline kernel hash embedded in the update USB. Reject ambiguous
or partially selected layouts. The only write ranges are the inactive fixed-size
kernel chain, its readiness sector and the first sector of each configuration.
No FAT, directory, GPT, BIOS stage, bootstrap or root sector is written.

Write and flush the inactive kernel, read it back, then write/flush/read back its
readiness record. Change the default-entry digit last, flushing and reading back
each menu sector. Do not retry uncertain writes. Interrupted preparation leaves
the selected old kernel intact. Interrupted selector publication can select either
complete boot set depending on which menu copy Limine finds. Previous installed
and recovery entries stay available; automatic failed-boot detection is outside
this first version. A mixed-selector state requires an explicit repair path and
must not trigger another update automatically.

The updater binds an exact predecessor hash to an exact candidate hash. Repeated
updates require freshly built media for the intended predecessor; this bounds ABI
compatibility rather than pretending arbitrary kernels are interchangeable.

Acceptance requires actual-code host sanitizer tests and injected interruption
boundaries, BIOS and UEFI QEMU USB installation/update/reboot, previous-kernel
manual selection, byte-exact root preservation across offline update, clean Linux
filesystem audits, and rot13 1.0 to 2.0 upgrade/reboot afterward. QEMU evidence is
separate from laptop acceptance. No flash command or PASS claim is justified by
this design document alone.

Implementation details: the existing boot module admission now recognizes the
second explicit basename `ready-b.v1`, still requiring exactly one 512-byte
`role=readiness` record and the existing complete identity/kernel/bootstrap hash
validation. No arbitrary readiness module names are accepted. Installation keeps
both records zero through copying and root provisioning, then durably publishes
B followed by A only after complete verification. Legacy single-slot publication
keeps its original order.

The two acceptance builds deliberately share the same user ABI and features;
their embedded release identifiers differ. This proves actual kernel replacement
and execution, rather than claiming a new feature release. Candidate debug sections
are removed with byte-exact comparison of every loadable segment. The updater ELF
must fit the existing ordinary executable snapshot allocation limit and ELF page
budget; no heap or loader limit is raised.

Host interruption tests model a partial-prefix write and reported flush failure.
They do not establish a physical drive's atomic-sector or power-loss guarantees.
Recovery boot-set tests explicitly select existing entries on disposable image
copies; they do not claim automatic failed-boot fallback or physical menu testing.
