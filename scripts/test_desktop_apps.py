"""Built-in app/persistence acceptance; private BIOS/UEFI guests, no data disks.

Live /etc survives desktop restart, not VM reboot. No PKG admission or physical
Wi-Fi support is inferred from these tests.
"""
import time
import tempfile
from pathlib import Path
from test_compositor import ROOT, Guest, QMP, pixel, wait_image

ALPHA = (ROOT / "user/compositor/assets/ui-alpha.bin").read_bytes()
ADVANCE = (ROOT / "user/compositor/assets/ui-advances.bin").read_bytes()
PANEL = bytes.fromhex("1e1e1e")
TEXT = bytes.fromhex("e0e0e0")
MUTED = bytes.fromhex("989898")


def width(text):
    return sum(ADVANCE[ord(c)-32] for c in text)


def text_matches(rgb, w, text, x, y, foreground=TEXT, background=PANEL):
    for char in text:
        index = ord(char)-32
        for gy in range(18):
            for gx in range(ADVANCE[index]):
                alpha = ALPHA[index*16*18 + gy*16 + gx]
                expected = bytes((f*alpha+b*(255-alpha)+127)//255
                                 for f,b in zip(foreground,background))
                if pixel(rgb,w,x+gx,y+gy) != expected:
                    return False
        x += ADVANCE[index]
    return True


def run(firmware, smp):
    print(f"[TEST] Desktop apps {firmware} SMP={smp}", flush=True)
    with tempfile.TemporaryDirectory(prefix="fortress-desktop-apps-") as temporary:
        tmp = Path(temporary)
        guest = Guest(firmware=firmware,smp=smp,tmp=tmp)
        try:
            guest.wait(lambda s:s.endswith("FortressOS login: "),timeout=90)
            guest.qmp = QMP(tmp / "qmp.sock")
            guest.send("operator")
            guest.wait(lambda s:s.endswith("$ "),timeout=20)
            assert "FortressOS Desktop 0.2.0 (app API 1)" in guest.command("/bin/compositor --version")

            def launch():
                start=len(guest.data)
                guest.send("sudo /bin/compositor")
                guest.wait(lambda s:"[OK] Window server ready; polling" in s,start,timeout=30)
                w,h,_=wait_image(guest,lambda iw,ih,rgb:pixel(rgb,iw,iw//2,ih//2)==bytes.fromhex("101010"))
                return w,h,[w//2,h//2]

            w,h,pos=launch()

            def move(x,y):
                while pos!=[x,y]:
                    dx=max(-100,min(100,x-pos[0]));dy=max(-100,min(100,y-pos[1]))
                    guest.motion(dx,dy);pos[0]+=dx;pos[1]+=dy;time.sleep(.035)
                wait_image(guest,lambda iw,ih,rgb:pixel(rgb,iw,x,y)==bytes.fromhex("101010"))

            def click(x,y):
                move(x,y)
                for down in [True,False]:
                    guest.qmp.execute("input-send-event",{"events":[{"type":"btn","data":{"down":down,"button":"left"}}]})
                    time.sleep(.08)

            def label(text,x=89,y=87,color=TEXT):
                return wait_image(guest,lambda iw,ih,rgb:text_matches(rgb,iw,text,x,y,color))

            def exit_desktop(scale=1):
                start=len(guest.data)
                click(30,12);click(50,168*scale)
                guest.wait(lambda s:"[COMPOSITOR] Event loop stopped" in s,start,timeout=20)
                guest.wait(lambda s:s.endswith("$ "),start,timeout=20)

            click(30,12);click(50,146)
            label("Settings")
            # Native scale, plain wallpaper, 12-hour clock, then atomic save.
            click(215,203);click(360,276);click(280,348);click(135,413)
            move(900,600)
            label("Preferences saved.",89,447,MUTED)
            guest.qmp.execute("screendump",{"filename":str(ROOT/"build"/f"desktop-settings-{firmware}-{smp}.ppm")})

            # Date & Time uses actual timezone validation and shared engine.
            click(w-60,12);label("Date & Time")
            click(270,288);click(515,238);move(900,600)
            label("System timezone saved;",89,431,MUTED)
            # Shared navigation opens Network. It is a real app, not a launch-only tile.
            click(300,135);label("Network & Wi-Fi")
            label("Default interface:",89,163,TEXT)
            expected = "Wi-Fi unavailable on this hardware or driver"
            label(expected,89,251,TEXT)
            guest.qmp.execute("screendump",{"filename":str(ROOT/"build"/f"desktop-network-{firmware}-{smp}.ppm")})
            exit_desktop()
            preferences=guest.command("cat /etc/desktop.conf")
            assert all(s in preferences for s in ["version=1","scale=native","wallpaper=plain","clock=12h"]),preferences
            assert "Europe/Brussels" in guest.command("cat /etc/timezone")

            # Restart the actual process: preferences remain live-session persistent.
            w,h,new_pos=launch();pos[:]=new_pos
            _,_,screen=guest.screenshot()
            suffix_x=w-100+width("00:00 ")
            assert text_matches(screen,w,"AM",suffix_x,3) or text_matches(screen,w,"PM",suffix_x,3)
            click(30,12);click(50,146);label("Settings")
            # Compare a selected Native button's interior with its known selected fill.
            _,_,screen=guest.screenshot()
            assert pixel(screen,w,65+136+3,73+116+3)==bytes.fromhex("244b6e")
            # Original wallpaper can be restored through the same saved preference.
            click(145,276);click(135,413);move(900,600)
            label("Preferences saved.",89,447,MUTED)
            guest.qmp.execute("screendump",{"filename":str(ROOT/"build"/f"desktop-original-{firmware}-{smp}.ppm")})
            exit_desktop()
            assert "wallpaper=hello-world" in guest.command("cat /etc/desktop.conf")
            if firmware=="bios" and smp==1:
                # Exercise the real runtime override and viewport, then recover
                # native size through the GUI rather than editing a fixture file.
                w,h,new_pos=launch();pos[:]=new_pos
                click(30,12);click(50,146);label("Settings")
                click(365,203);click(135,413);move(900,650)
                wait_image(guest,lambda iw,ih,rgb:pixel(rgb,iw,200,30)==PANEL)
                exit_desktop(2)
                assert "scale=large" in guest.command("cat /etc/desktop.conf")
                w,h,new_pos=launch();pos[:]=new_pos
                click(30,12);click(50,292)
                # Large app starts at (64,72), client origin (65,97).
                click(377,349)
                move(650,400)
                for _ in range(3):
                    for down in [True,False]:
                        guest.qmp.execute("input-send-event",{"events":[{"type":"btn","data":{"down":down,"button":"wheel-down"}}]})
                        time.sleep(.12)
                click(200,585);move(900,650)
                wait_image(guest,lambda iw,ih,rgb:pixel(rgb,iw,200,30)!=PANEL)
                exit_desktop()
                assert "scale=native" in guest.command("cat /etc/desktop.conf")
            assert "[FORTRESS RUST PANIC]" not in guest.text()
            assert "LOCK ORDER" not in guest.text()
            print(f"[PASS] Desktop apps {firmware} SMP={smp}: settings, restart persistence, timezone, owner-safe status",flush=True)
        finally:
            (ROOT/"build"/f"desktop-apps-{firmware}-{smp}.log").write_text(guest.text())
            if guest.qmp and guest.proc.poll() is None:
                guest.qmp.execute("screendump",{"filename":str(ROOT/"build"/f"desktop-apps-last-{firmware}-{smp}.ppm")})
            guest.close()


def main():
    for firmware in ["bios","uefi"]:
        for smp in [1,4]:
            run(firmware,smp)


if __name__ == "__main__":
    main()
