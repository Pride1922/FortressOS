Step 1 — Remove the dead fields
Files: src/drivers/xhci_ports.h, src/drivers/xhci_ports.c

In xhci_ports.h, delete these two lines from the xhci_port_report_t struct:

c
uint8_t selected_usb2_port; /* 1-based index of primary attached USB 2.0 port, 0 if none */
uint8_t selected_speed;     /* Speed of selected port */
In xhci_ports.c, delete the block at lines ~138–143:

c
if (info->enabled &&
    (info->speed == XHCI_SPEED_HIGH || info->speed == XHCI_SPEED_FULL)) {
    if (!report->selected_usb2_port) {
        report->selected_usb2_port = (uint8_t)p;
        report->selected_speed = info->speed;
    }
}
Verify: rg "selected_usb2_port|selected_speed" src/ returns zero hits.

If you get stuck: if the compiler complains that something still references those fields, paste the error.

Step 2 — Add the SuperSpeedPlus constant
File: src/drivers/xhci_ports.h

Add after the existing XHCI_SPEED_SUPER line:

c
#define XHCI_SPEED_SUPER_PLUS    5u  /* 10 Gb/s SuperSpeedPlus (USB 3.1) */
Verify: rg "XHCI_SPEED" src/drivers/xhci_ports.h shows five definitions (FULL, LOW, HIGH, SUPER, SUPER_PLUS).

If you get stuck: this should be trivial. If the existing constants use an enum instead of #define, mirror that style.

Step 3 — Add the USB 3.0 port reset in the port scan
File: src/drivers/xhci_ports.c

Find and replace this block:

c
if (info->protocol_major == 3) {
    /* SuperSpeed attachment: logged and skipped */
    continue;
}
With this:

c
if (info->protocol_major == 3) {
    /* USB 3.0 attachment: ensure power, issue port reset, then wait
     * for the link to reach U0 (operational). SuperSpeed ports report
     * operational state via PLS rather than PED alone. */
    if (!(raw & XHCI_PORTSC_PP)) {
        io->write32(io->mmio_ctx, portsc_off,
                    (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PP);
        for (int i = 0; i < 20; ++i) io->delay_ms(io->mmio_ctx);
        raw = io->read32(io->mmio_ctx, portsc_off);
    }

    io->write32(io->mmio_ctx, portsc_off,
                (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PR);

    bool reset_done = false;
    for (unsigned ms = 0; ms <= 100; ++ms) {
        raw = io->read32(io->mmio_ctx, portsc_off);
        if (raw != UINT32_MAX && !(raw & XHCI_PORTSC_PR)) {
            reset_done = true;
            break;
        }
        io->delay_ms(io->mmio_ctx);
    }

    if (reset_done) {
        /* Wait for PLS to reach U0. PLS = bits [8:5], U0 = 0. */
        for (unsigned ms = 0; ms <= 100; ++ms) {
            raw = io->read32(io->mmio_ctx, portsc_off);
            if (raw != UINT32_MAX && ((raw >> 5) & 0x7) == 0) break;
            io->delay_ms(io->mmio_ctx);
        }

        raw = io->read32(io->mmio_ctx, portsc_off);
        info->raw_portsc = raw;
        info->enabled = (raw & XHCI_PORTSC_PED) != 0;
        info->speed = (uint8_t)((raw & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);

        io->write32(io->mmio_ctx, portsc_off,
                    (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PRC | XHCI_PORTSC_CSC);
    }
    continue;
}
Verify: rg -n "SuperSpeed attachment: logged and skipped" src/ returns nothing.

If you get stuck: the most likely failures are XHCI_PORTSC_PRC, XHCI_PORTSC_CSC, or XHCI_PORTSC_SPEED_MASK not being defined. Check with rg "XHCI_PORTSC_" src/drivers/xhci_ports.h and paste what exists if the build fails.

Step 4 — Update the log block in xhci.c
File: src/drivers/xhci.c

Find and replace this block (around line 374):

c
if (pi->protocol_major == 3) {
    serial_puts(": SuperSpeed device attached; unsupported in Phase 9G, skipped\n");
}
With:

c
if (pi->protocol_major == 3) {
    serial_puts(": USB 3.0 device attached; ");
    if (pi->enabled) {
        serial_puts("reset complete, enabled, speed=");
        if (pi->speed == XHCI_SPEED_SUPER) serial_puts("SuperSpeed (5 Gbps)\n");
        else if (pi->speed == XHCI_SPEED_SUPER_PLUS) serial_puts("SuperSpeedPlus (10 Gbps)\n");
        else serial_puts("unknown SuperSpeed variant\n");
    } else {
        serial_puts("reset failed to enable port\n");
    }
}
Verify: rg -n "unsupported in Phase 9G" src/ returns nothing.

If you get stuck: trivial change. If XHCI_SPEED_SUPER_PLUS isn't seen, step 2 didn't land or is in the wrong header.

Step 5 — Relax the enumeration filters in xhci.c
File: src/drivers/xhci.c

Change line 399 from:

c
if (!pi->connected || !pi->enabled || pi->protocol_major != 2) continue;
To:

c
if (!pi->connected || !pi->enabled) continue;
Change line 400 from:

c
if (pi->speed != XHCI_SPEED_HIGH && pi->speed != XHCI_SPEED_FULL) continue;
To:

c
if (pi->speed != XHCI_SPEED_HIGH && pi->speed != XHCI_SPEED_FULL &&
    pi->speed != XHCI_SPEED_SUPER) continue;
Verify: rg -n "protocol_major != 2" src/ returns nothing.

If you get stuck: both are one-line changes. If a compilation error appears about pi->protocol_major becoming unused, remove the field reference but leave the field in the struct — it may still be needed elsewhere.

Step 6 — Build, flash, boot
Build:

powershell
wsl -d Ubuntu-24.04 -- bash -c "make 2>&1 | tail -15"
Watch the output. The build must end with the image verification block (the [PASS] Protective MBR verified... lines). If it ends with any error, do not flash. Paste the error and ask for help.

Then:

powershell
wsl -d Ubuntu-24.04 -- bash -c "sha256sum bin/fortress.img"
Note the hash. It should differ from the last one you captured.

Flash:

powershell
wsl -d Ubuntu-24.04 -- bash -c "sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync"
Replace sdX with the actual USB device.

Boot the Dell with the SanDisk in a USB 3.0 port.

Look for these lines in the log:

text
[USB 9G.1d] Port 0x10: USB 3.0 device attached; reset complete, enabled, speed=SuperSpeed (5 Gbps)
[USB 9G.1d] PASS: Root port scan complete
[USB 9G.1e] Probing Port 0x10 for BOT Mass Storage...
The third line is the acceptance for commit 1. It confirms the port was reset, enabled, selected, and reached the enumeration step.

What comes next will probably be a failure in xhci_enumerate_device — because xhci_dev.c still constructs USB 2.0-shaped slot and endpoint contexts. That failure is the input to commit 2. Paste the error lines after Probing Port 0x10 and that's the starting point for the next step.

Summary of Files Touched
src/drivers/xhci_ports.h — remove two fields, add one constant

src/drivers/xhci_ports.c — remove dead selection block, add USB 3.0 reset

src/drivers/xhci.c — update log, relax two filters

Four edits total, one of which (step 3) is the substantive change.

Where to Stop and Ask
Stop and ask for help if any of these happen:

Step 1: the compiler complains about a reference to selected_usb2_port or selected_speed outside the two files you edited.

Step 2: you can't find where to put the new #define.

Step 3: any of the XHCI_PORTSC_* constants aren't defined, or the build fails inside xhci_ports.c.

Step 4: the log block isn't where you expected it, or the surrounding code differs from what you pasted earlier.

Step 5: the two filter lines aren't where you expected.

Step 6: make fails, or the image hash doesn't change, or the boot log shows something other than reset complete, enabled, speed=SuperSpeed (5 Gbps) for Port 0x10.