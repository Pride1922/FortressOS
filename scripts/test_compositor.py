#!/usr/bin/env python3
"""Phase 4 window-server and Phase 3 pixel-oracle and QMP input acceptance, BIOS/UEFI, ISO only.
No data disks; private OVMF variables; always reap QEMU.
"""

import re
import selectors
import shutil
import subprocess
import struct
import tempfile
import time
from pathlib import Path

from test_nmi_transitions import QMP

ROOT = Path(__file__).resolve().parent.parent


class Guest:
    def __init__(self, firmware='bios', smp=1, tmp=None, iso=None):
        self.data = bytearray()
        self.tmp = tmp
        self.qmp = None
        argv = [
            'qemu-system-x86_64', '-M', 'q35', '-m', '1024M', '-smp', str(smp),
            '-accel', 'tcg', '-cpu', 'max,+rdrand,+rdseed', '-display', 'none',
            '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-boot', 'd',
            '-cdrom', str(iso or ROOT / 'bin/fortress.iso'),
            '-device', 'qemu-xhci,id=xhci',
            '-device', 'usb-mouse,bus=xhci.0',
            '-qmp', f'unix:{tmp / "qmp.sock"},server=on,wait=off'
        ]
        if firmware == 'uefi':
            assert tmp is not None
            vars_fd = tmp / f'vars-{smp}.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', vars_fd)
            argv += [
                '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                '-drive', f'if=pflash,format=raw,unit=1,file={vars_fd}'
            ]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)

    def pump(self):
        for key, _ in self.sel.select(0.05):
            self.data.extend(key.fileobj.read1(65536))

    def text(self, start=0):
        cleaned = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', self.data[start:].decode(errors='replace'))
        return cleaned.replace('\r', '')

    def wait(self, predicate, start=0, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            t = self.text(start)
            if predicate(t):
                return t
            assert self.proc.poll() is None, 'Guest terminated unexpectedly: ' + t[-1500:]
        raise AssertionError(f'Timeout waiting for condition after {timeout}s: ' + t[-2000:])

    def send(self, line):
        self.proc.stdin.write((line + '\n').encode())
        self.proc.stdin.flush()

    def command(self, cmd, timeout=20):
        start = len(self.data)
        self.send(cmd)
        result = self.wait(lambda s: '\n' in s and s.endswith('$ '), start, timeout)
        return result.split('\n', 1)[1]

    def screenshot(self):
        path = self.tmp / 'screen.ppm'
        self.qmp.execute('screendump', {'filename': str(path)})
        with path.open('rb') as f:
            assert f.readline().strip() == b'P6'
            width, height = map(int, f.readline().split())
            assert f.readline().strip() == b'255'
            rgb = f.read()
        assert len(rgb) == width*height*3
        return width, height, rgb

    def motion(self, dx, dy):
        self.qmp.execute('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': dy}},
        ]})

    def close(self):
        if self.qmp:
            self.qmp.stream.close()
            self.qmp.sock.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.sel.close()


def pixel(rgb, width, x, y):
    i = (y*width+x)*3
    return rgb[i:i+3]


def wait_image(guest, predicate):
    end = time.monotonic()+10
    while time.monotonic() < end:
        guest.pump()
        image = guest.screenshot()
        if predicate(*image):
            return image
        time.sleep(0.05)
    raise AssertionError('Framebuffer did not reach expected cursor state')


def run_test(firmware):
    print(f'[TEST] {firmware.upper()}: text, cursor, window focus/drag/controls/damage repair')
    with tempfile.TemporaryDirectory(prefix='fortress-compositor-') as tmp_dir:
        tmp = Path(tmp_dir)
        guest = Guest(firmware=firmware, smp=1, tmp=tmp)
        try:
            guest.wait(lambda s: s.endswith('FortressOS login: '), timeout=60)
            guest.qmp = QMP(tmp / 'qmp.sock')
            guest.send('operator')
            guest.wait(lambda s: s.endswith('$ '), timeout=15)
            # Mouse devices are root-owned 0660; preserve the device policy.
            guest.send('sudo /bin/compositor --test')
            guest.wait(lambda s: '[OK] Window server ready; polling' in s, timeout=20)
            w, h, baseline = wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,80,120) == bytes.fromhex('607089') and pixel(rgb,iw,93,157) == bytes.fromhex('202b3b') and pixel(rgb,iw,iw//2,ih//2) == bytes.fromhex('101010'))
            guest.qmp.execute('screendump', {'filename': str(ROOT / 'build' / f'compositor-windows-{firmware}.ppm')})
            cx, cy = w//2, h//2
            bg, fg, outline = bytes.fromhex('141923'), bytes.fromhex('e0e8ff'), bytes.fromhex('101010')
            # Independent font oracle from the original C table, all title pixels.
            source = (ROOT / 'src/drivers/font.h').read_text()
            rows = re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){16})\}', source)
            assert len(rows) == 95
            rust = (ROOT / 'user/compositor/src/font.rs').read_text()
            assert re.findall(r'0x[0-9A-Fa-f]{2}', rust) == re.findall(r'0x[0-9A-Fa-f]{2}', '\n'.join(rows))
            for i, char in enumerate('FortressOS Rust Compositor'):
                glyph = [int(n,16) for n in re.findall(r'0x[0-9A-Fa-f]{2}', rows[ord(char)-32])]
                for y, bits in enumerate(glyph):
                    for x in range(8):
                        assert pixel(baseline,w,32+i*8+x,32+y) == (fg if bits & (128>>x) else bg), (char,x,y)
            def check_text(rgb, text, px, py, background):
                for i, char in enumerate(text):
                    glyph = [int(n,16) for n in re.findall(r'0x[0-9A-Fa-f]{2}', rows[ord(char)-32])]
                    for gy, bits in enumerate(glyph):
                        for gx in range(8):
                            assert pixel(rgb,w,px+i*8+gx,py+gy) == (fg if bits & (128>>gx) else background), (text,i,char,gx,gy,pixel(rgb,w,px+i*8+gx,py+gy).hex(), (fg if bits & (128>>gx) else background).hex())

            border = bytes.fromhex('607089')
            active, inactive = bytes.fromhex('2a3a5e'), bytes.fromhex('1e2533')
            client, gold, purple = bytes.fromhex('202b3b'), bytes.fromhex('e5c07b'), bytes.fromhex('c678dd')
            assert pixel(baseline,w,80,120) == border
            assert pixel(baseline,w,260,220) == border
            assert pixel(baseline,w,350,270) == gold, 'front palette must occlude monitor'
            check_text(baseline,'System Monitor',88,125,inactive)
            check_text(baseline,'Palette Canvas',268,225,active)
            check_text(baseline,f'Screen: {w} x {h}',93,157,client)
            check_text(baseline,f'Pitch: {w*4} bytes',93,181,client)
            check_text(baseline,f'Mouse: {cx}, {cy}',93,229,client)
            assert pixel(baseline,w,cx,cy) == outline
            guest.motion(40,20)
            _, _, moved = wait_image(guest, lambda iw,ih,rgb: pixel(rgb,iw,cx+40,cy+20) == outline)
            for y in range(16):
                for x in range(16):
                    assert pixel(moved,w,cx+x,cy+y) == bg, 'cursor left a trail'
                    assert pixel(moved,w,cx+40+x,cy+20+y) == pixel(baseline,w,cx+x,cy+y), 'sprite mismatch'
            # Return to the exact starting position, including alpha edge pixels.
            guest.motion(-40,-20)
            _, _, returned = wait_image(guest, lambda iw,ih,rgb: pixel(rgb,iw,cx,cy) == outline)
            for y in range(16):
                for x in range(16):
                    assert pixel(returned,w,cx+x,cy+y) == pixel(baseline,w,cx+x,cy+y)
                    assert pixel(returned,w,cx+40+x,cy+20+y) == bg
            # Repeated bounded packets exercise saturation at both edges.
            for _ in range((max(w,h)//127)+3):
                guest.motion(127,127)
                time.sleep(0.06)
            wait_image(guest, lambda iw,ih,rgb: pixel(rgb,iw,iw-1,ih-1) == outline)
            for _ in range((max(w,h)//127)+3):
                guest.motion(-127,-127)
                time.sleep(0.06)
            wait_image(guest, lambda iw,ih,rgb: pixel(rgb,iw,0,0) == outline)
            pos = [0,0]
            def move_to(x,y):
                while pos != [x,y]:
                    dx = max(-100,min(100,x-pos[0]))
                    dy = max(-100,min(100,y-pos[1]))
                    guest.motion(dx,dy)
                    pos[0] += dx; pos[1] += dy
                    time.sleep(0.06)
                return wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,x,y) == outline)[2]

            def button(down):
                guest.qmp.execute('input-send-event', {'events': [
                    {'type':'btn','data':{'down':down,'button':'left'}}]})
                # Waits below check actual resulting pixels, not a serial banner.
                time.sleep(0.10)

            move_to(100,160); button(True); button(False)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,350,270) == client and pixel(rgb,iw,85,122) == active)
            move_to(100,130); button(True)
            move_to(220,210); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,200,200) == border and pixel(rgb,iw,80,120) == bg)
            # Move it away again: the palette beneath must be repaired byte-exactly.
            move_to(220,210); button(True)
            move_to(580,210); button(False)
            move_to(100,100)
            _,_,repaired = wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,560,200) == border and pixel(rgb,iw,350,270) == gold)
            for yy in range(220,400):
                for xx in range(260,540):
                    expected = pixel(baseline,w,xx,yy)
                    # Focus changed; the palette's inactive titlebar has a new fill.
                    if 261 <= xx < 539 and 221 <= yy <= 244:
                        continue
                    assert pixel(repaired,w,xx,yy) == expected, ('exposed palette repair',xx,yy)
            # Drag palette over the monitor, proving the reverse occlusion order.
            move_to(280,230); button(True)
            move_to(400,310); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,380,300) == border and pixel(rgb,iw,600,350) == purple)
            # Capture continues beyond the old window, and clips at screen bounds.
            move_to(400,310); button(True)
            move_to(w-10,h-10); button(False)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,iw-30,ih-20) == border)
            button(True); move_to(400,310); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,380,300) == border and pixel(rgb,iw,iw-30,ih-20) == bg)
            # Release capture: an ordinary mouse move must leave frame coordinates unchanged.
            move_to(200,100)
            assert pixel(guest.screenshot()[2],w,380,300) == border
            move_to(620,302)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,615,302) == bytes.fromhex('486080'))
            button(True); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,380,300) == bg and pixel(rgb,iw,180,ih-26) == inactive)
            move_to(180,h-26); button(True); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,380,300) == border and pixel(rgb,iw,385,302) == active)
            move_to(640,302)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,637,302) == bytes.fromhex('a63c50'))
            button(True); button(False)
            move_to(100,100)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,380,300) == bg and pixel(rgb,iw,565,202) == active)
            move_to(865,202); button(True); button(False)
            move_to(100,100)
            _,_,empty = wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,560,200) == bg)
            for yy in range(120,480):
                for xx in range(80,880):
                    assert pixel(empty,w,xx,yy) == bg, ('close damage repair',xx,yy)
            guest.qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': True, 'button': 'right'}}]})
            guest.wait(lambda s: '[COMPOSITOR] Event loop stopped' in s, timeout=10)
            guest.wait(lambda s: s.endswith('$ '), timeout=10)
            # Exercise the normal desktop too: it has no demo windows or lock.
            guest.qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': False, 'button': 'right'}}]})
            start = len(guest.data)
            guest.send('sudo /bin/compositor')
            guest.wait(lambda s: '[OK] Window server ready; polling' in s, start, timeout=20)
            panel = bytes.fromhex('161d29')
            _, _, desktop = wait_image(guest, lambda iw, ih, rgb:
                pixel(rgb, iw, 200, 10) == panel and pixel(rgb, iw, 80, 120) != border)
            # Verify the original wallpaper is loaded, and later survives a
            # maximize/restore repair. Pick an ink pixel away from the cursor.
            wallpaper = (ROOT / 'build/initramfs/usr/share/wallpapers/hello-world.bin').read_bytes()
            magic, ow, oh, background, cw, ch = struct.unpack('<4sIIIII', wallpaper[:24])
            assert magic == b'FWP1'
            ink = next(i for i in range(cw * ch - 1, -1, -1)
                       if int.from_bytes(wallpaper[24+i*4:28+i*4], 'little') != background)
            ink_color = int.from_bytes(wallpaper[24+ink*4:28+ink*4], 'little')
            ink_rgb = bytes([(ink_color >> 16) & 255, (ink_color >> 8) & 255, ink_color & 255])
            ink_x, ink_y = (w-cw)//2 + ink % cw, (h-ch)//2 + ink // cw
            wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, ink_x, ink_y) == ink_rgb)
            pos[:] = [w // 2, h // 2]
            dock_x, dock_y = (w - 256) // 2, h - 66
            move_to(dock_x + 3 * 64 + 32, dock_y + 20)
            button(True); button(False)
            move_to(700, 400)
            _, _, system = wait_image(guest, lambda iw, ih, rgb:
                pixel(rgb, iw, 90, 190) == panel)
            # Independent alpha-atlas oracle: SYS_SYSINFO reports one CPU in
            # this SMP=1 guest. Check the full proportional glyph and margins.
            atlas = (ROOT / 'user/compositor/assets/ui-alpha.bin').read_bytes()
            advance = (ROOT / 'user/compositor/assets/ui-advances.bin').read_bytes()[ord('1') - 32]
            foreground = bytes.fromhex('e7edf6')
            for gy in range(18):
                for gx in range(advance):
                    a = atlas[(ord('1') - 32) * 16 * 18 + gy * 16 + gx]
                    expected = bytes((f * a + b * (255 - a) + 127) // 255
                                     for f, b in zip(foreground, panel))
                    assert pixel(system, w, 80 + 1 + 230 + gx, 120 + 25 + 84 + gy) == expected, ('system CPU telemetry', gx, gy)
            guest.qmp.execute('screendump', {'filename': str(ROOT / 'build' / f'compositor-polish-{firmware}.ppm')})
            # Maximize, keep the cursor inside the client through idle telemetry
            # refreshes, then restore. Timer repaints must preserve save-under.
            move_to(80 + 520 - 54, 132); button(True); button(False)
            move_to(700, 400)
            wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, 8, 28) == bytes.fromhex('30363d'))
            time.sleep(2.1)
            _, _, maximized = guest.screenshot()
            assert pixel(maximized, w, 700, 400) == outline, 'idle refresh erased cursor'
            move_to(w - 62, 40); button(True); button(False)
            move_to(700, 400)
            # The outer corner is transparent in restored rounded windows.
            wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, 88, 120) == bytes.fromhex('30363d'))
            wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, ink_x, ink_y) == ink_rgb)
            # A right click must not terminate the normal desktop.
            start = len(guest.data)
            guest.qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': True, 'button': 'right'}}]})
            time.sleep(0.2)
            guest.pump()
            assert 'Event loop stopped' not in guest.text(start)
            guest.qmp.execute('input-send-event', {'events': [
                {'type': 'btn', 'data': {'down': False, 'button': 'right'}}]})
            # Exit explicitly through the system menu.
            move_to(30, 12); button(True); button(False)
            move_to(50, 168); button(True); button(False)
            guest.wait(lambda s: '[COMPOSITOR] Event loop stopped' in s, start, timeout=10)
            guest.wait(lambda s: s.endswith('$ '), timeout=10)
            assert '[FORTRESS RUST PANIC]' not in guest.text()
            print(f'[PASS] {firmware.upper()} ({w}x{h})')
        finally:
            try:
                (ROOT / 'build' / f'compositor-{firmware}.log').write_text(guest.text())
                if guest.qmp and guest.proc.poll() is None:
                    guest.qmp.execute('screendump', {'filename': str(ROOT / 'build' / f'compositor-{firmware}.ppm')})
            finally:
                guest.close()


def run_hidpi_test(firmware):
    """Real bounded compositor heap, unlike host tests' system allocator."""
    print(f'[TEST] {firmware.upper()}: 1920x1080 System Information maximize/restore')
    with tempfile.TemporaryDirectory(prefix='fortress-compositor-hidpi-') as directory:
        tmp = Path(directory)
        conf = tmp / 'limine.conf'
        conf.write_text('timeout: 0\n/FortressOS\n    protocol: limine\n'
                        '    resolution: 1920x1080x32\n'
                        '    kernel_path: boot():/boot/fortress.elf\n'
                        '    module_path: boot():/boot/initramfs.tar\n'
                        '    kernel_cmdline: net=dhcp\n')
        iso = tmp / 'hidpi.iso'
        subprocess.run(['xorriso', '-indev', str(ROOT / 'bin/fortress.iso'),
                        '-outdev', str(iso), '-boot_image', 'any', 'replay',
                        '-map', str(conf), '/boot/limine/limine.conf',
                        '-map', str(conf), '/boot/limine.conf'], check=True, capture_output=True)
        subprocess.run([str(ROOT / 'limine/limine'), 'bios-install', str(iso)],
                       check=True, capture_output=True)
        guest = Guest(firmware=firmware, smp=1, tmp=tmp, iso=iso)
        try:
            guest.wait(lambda s: s.endswith('FortressOS login: '), timeout=60)
            guest.qmp = QMP(tmp / 'qmp.sock')
            guest.send('operator')
            guest.wait(lambda s: s.endswith('$ '), timeout=15)
            guest.send('sudo /bin/compositor')
            guest.wait(lambda s: '[OK] Window server ready; polling' in s, timeout=20)
            w,h,_ = wait_image(guest, lambda iw,ih,rgb: (iw,ih) == (1920,1080)
                              and pixel(rgb,iw,300,10) == bytes.fromhex('161d29'))
            pos = [w//2,h//2]
            def move(x,y):
                while pos != [x,y]:
                    dx=max(-100,min(100,x-pos[0])); dy=max(-100,min(100,y-pos[1]))
                    guest.motion(dx,dy); pos[0]+=dx; pos[1]+=dy
                    time.sleep(.04)
                wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,x,y) == bytes.fromhex('101010'))
            def click():
                for down in [True,False]:
                    guest.qmp.execute('input-send-event', {'events':[
                        {'type':'btn','data':{'down':down,'button':'left'}}]})
                    time.sleep(.1)
            move((w-256)//2 + 3*64+32,h-66+20); click()
            move(1300,700)
            wait_image(guest,lambda iw,ih,rgb: pixel(rgb,iw,88,120) == bytes.fromhex('30363d'))
            for _ in range(3):
                move(80 + 520 - 54, 132); click(); move(1300, 700)
                wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, 8, 28) == bytes.fromhex('30363d'))
                move(w - 62, 40); click(); move(1300, 700)
                wait_image(guest, lambda iw, ih, rgb: pixel(rgb, iw, 88, 120) == bytes.fromhex('30363d'))
            guest.qmp.execute('screendump',{'filename':str(ROOT/'build'/f'compositor-hidpi-{firmware}.ppm')})
            move(30,12); click(); move(50,168); click()
            guest.wait(lambda s: '[COMPOSITOR] Event loop stopped' in s, timeout=10)
            assert '[FORTRESS RUST PANIC]' not in guest.text()
            print(f'[PASS] {firmware.upper()} 1920x1080 repeated maximize/restore')
        finally:
            (ROOT/'build'/f'compositor-hidpi-{firmware}.log').write_text(guest.text())
            guest.close()


def main():
    print('========================================================')
    print(' FortressOS Rust Compositor Multi-Firmware Verification ')
    print('========================================================')
    for fw in ['bios', 'uefi']:
        run_test(fw)
        run_hidpi_test(fw)
    print('\n[SUCCESS] Rust Compositor Phase 4 verification 100% PASS across BIOS and UEFI!')


if __name__ == '__main__':
    main()
