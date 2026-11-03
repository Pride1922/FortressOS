# Dell acceptance — complete installer, 2026-11-03

Record PASS/FAIL for each test and photograph failures. QEMU and host results do
not replace these physical checks. Install this release on the internal disk;
remove the USB before testing persistence. The installer erases the selected disk.

1. **Installation and applications.** Complete installation, remove USB, boot and
   log in. Open the desktop, Files, Notepad, Terminal and Calculator. Save a note
   in your home directory. In Calculator enter `12+34` and `=`: expect `46`.
   Clear it and calculate `6*7`: expect `42`. Close and reopen each app.
2. **Terminal resizing.** In the desktop Terminal run
   `printf 'ABCDEFG 0123456789\n'`. Drag all four edges repeatedly, then maximize
   and restore. Letters must keep their shape throughout the drag; newly exposed
   space must be clean. Type another command: text and cursor must remain usable.
   Also resize Calculator and Notepad; content must repaint without stale pixels.
3. **Wi-Fi and DHCP.** Connect once using `wifi connect` with your normal network
   arguments. Run `wifi status` and `ifconfig`: expect CONNECTED, ONLINE and a
   valid LAN address. No separate `dhcpc` command should be needed. Run
   `ping 1.1.1.1`. Record how long obtaining an address takes.
4. **Saved connection.** Reboot twice, including one full shutdown/power-on.
   Before running any connect command, run `wifi status` and `ifconfig`.
   The saved network must reconnect and obtain an address automatically.
   Check the saved note still exists. Run `ls -l /etc/network/wlan.conf`:
   expect root ownership and mode 0600. Do not photograph or print its contents.
   Password encryption is not implemented; this checks access permissions.
5. **Inbound LAN before outbound traffic.** After another reboot, leave FortressOS
   idle; do not ping Bazzite from it. On Bazzite run the following, substituting
   FortressOS's CURRENT address if it changed:

   ```sh
   sudo ip neigh del 192.168.0.194 dev wlo1
   ping -c 4 -W 1 192.168.0.194
   ip neigh show 192.168.0.194 dev wlo1
   ```

   A missing-neighbour error from the first command is harmless. Expect ping
   replies and a resolved MAC without adding a static neighbour. Repeat after
   five idle minutes. On failure photograph `sudo dmesg -n 4` on FortressOS.
6. **Power and ordinary use.** Verify desktop shutdown and reboot work. Run
   `battery` with the charger disconnected and connected. This release must
   report telemetry unavailable rather than invent 100%, AC power or temperature;
   real battery telemetry is still pending. Use the desktop for ten minutes,
   including typing, moving windows and reopening apps; record any crash/hang.

Acceptance is complete only when all applicable checks pass on the Dell. For
failures, report the test number, exact visible error, current IP and whether this
was a fresh installed boot. Do not reconnect manually before capturing a failed
saved-connection test.
