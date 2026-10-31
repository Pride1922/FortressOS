#!/usr/bin/env python3
"""Out-of-process desktop apps (user/compositor/src/clients.rs, proto.rs).

well_behaved: /bin/hello-window draws, receives keys, retitles, quits, is
relaunched and closed with [X]; no process remains.
hostile: /bin/hostile-window (via --app=) tries absurd sizes and a bad
title, ignores close, floods 20000 requests and exits early; the desktop
must stay intact and responsive and no hostile process may survive.
notepad: /bin/notepad edits and saves a file as the session user.
"""
import sys, time, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_compositor import Guest, wait_image, pixel
from test_nmi_transitions import QMP
from PIL import Image

BOX = bytes.fromhex('e8e8e8')


def key(g, b, delay=0.6):
    g.proc.stdin.write(b); g.proc.stdin.flush(); time.sleep(delay)


def run_well_behaved():
    def shot(g, name):
        g.qmp.execute('screendump', {'filename': f'/tmp/{name}.ppm'}); time.sleep(.4)
        return Image.open(f'/tmp/{name}.ppm').convert('RGB')
    def px(im, x, y): return bytes(im.getpixel((x, y)))
    results = []
    def check(ok, what): results.append(ok); print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    with tempfile.TemporaryDirectory() as t:
        tmp=Path(t); g=Guest(firmware='bios',smp=1,tmp=tmp)
        try:
            g.wait(lambda s: s.endswith('FortressOS login: '), timeout=120)
            g.qmp=QMP(tmp/'qmp.sock')
            g.send('operator'); g.wait(lambda s: s.endswith('$ '), timeout=30)
            start=len(g.data)
            g.send('sudo /bin/compositor')
            g.wait(lambda s: 'Window server ready' in s, start, timeout=40)
            wait_image(g,lambda iw,ih,rgb:pixel(rgb,iw,iw//2,ih//2)==bytes.fromhex('101010'))
            time.sleep(2)  # first frame complete before it becomes the baseline
            before=shot(g,'a0')
            assert before.getpixel((1000,600))==before.getpixel((300,200)), 'baseline not fully painted'
            key(g,b'h'); time.sleep(2)
            im=shot(g,'a1')
            check(px(im,500,355)==BOX and px(before,500,355)!=BOX, 'app window drawn: box at initial spot')
            g1=px(im,700,500); check(g1!=px(before,700,500), 'gradient content visible inside window')
            for _ in range(3): key(g,b'l')
            time.sleep(1); im=shot(g,'a2')
            check(px(im,560,355)==BOX and px(im,500,355)!=BOX, 'keys reach the app: box moved 60 px right')
            key(g,b'x'); time.sleep(1); im=shot(g,'a3')
            check(px(im,560,355)!=BOX, 'other key recoloured the box')
            key(g,b'q'); time.sleep(3); im=shot(g,'a4')
            check(px(im,560,355)==px(before,560,355) and px(im,700,500)==px(before,700,500), "app quit with 'q': window removed, desktop restored")
            key(g,b'h'); time.sleep(2); im=shot(g,'a5')
            check(px(im,500,355)==BOX, 'app relaunched')
            # close button: right end of the title bar
            w=[640,400]
            def move(x,y):
                while w!=[x,y]:
                    dx=max(-100,min(100,x-w[0]));dy=max(-100,min(100,y-w[1])); g.motion(dx,dy); w[0]+=dx; w[1]+=dy; time.sleep(.04)
                time.sleep(.3)
            move(430+420-18, 270+12)
            for down in [True,False]:
                g.qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'down':down,'button':'left'}}]}); time.sleep(.15)
            move(900,650); time.sleep(3); im=shot(g,'a6')
            check(px(im,500,355)==px(before,500,355), 'close button removes the app window')
            key(g,b'q')
            g.wait(lambda s:'Event loop stopped' in s,start,timeout=30)
            g.wait(lambda s:s.endswith('$ '),start,timeout=20)
            ps=g.command('ps')
            check('hello-window' not in ps, 'no hello-window process left after closing')
            print('\n'.join(l for l in g.text(start).split('\n') if 'COMPOSITOR' in l and 'PERF' not in l))
            return all(results)
        finally:
            g.close()


def run_hostile():
    def shot(g, name):
        g.qmp.execute('screendump', {'filename': f'/tmp/{name}.ppm'}); time.sleep(.4)
        return Image.open(f'/tmp/{name}.ppm').convert('RGB')
    results = []
    def check(ok, what): results.append(ok); print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    with tempfile.TemporaryDirectory() as t:
        tmp=Path(t); g=Guest(firmware='bios',smp=1,tmp=tmp)
        try:
            g.wait(lambda s: s.endswith('FortressOS login: '), timeout=120)
            g.qmp=QMP(tmp/'qmp.sock')
            g.send('operator'); g.wait(lambda s: s.endswith('$ '), timeout=30)
            start=len(g.data)
            g.send('sudo /bin/compositor --app=/bin/hostile-window')
            g.wait(lambda s: 'Window server ready' in s, start, timeout=40)
            wait_image(g,lambda iw,ih,rgb:pixel(rgb,iw,iw//2,ih//2)==bytes.fromhex('101010'))
            # The first frame must be complete (bottom rows and dock painted)
            # before it serves as the baseline.
            time.sleep(2)
            desk=shot(g,'h0')
            assert desk.getpixel((1000,600))==desk.getpixel((300,200)), 'baseline not fully painted'
            pos=[640,400]
            def click(x,y):
                while pos!=[x,y]:
                    dx=max(-100,min(100,x-pos[0]));dy=max(-100,min(100,y-pos[1])); g.motion(dx,dy); pos[0]+=dx; pos[1]+=dy; time.sleep(.04)
                time.sleep(.3)
                for down in [True,False]:
                    g.qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'down':down,'button':'left'}}]}); time.sleep(.15)
            def restored(name, pts):
                im=shot(g,name); return all(im.getpixel(p)==desk.getpixel(p) for p in pts)
            pts=[(300,200),(600,380),(1000,600),(700,540)]  # clear of the cursor
            # 0: absurd sizes + bad title -> dropped
            key(g,b'h'); time.sleep(4)
            check(restored('h1',pts), 'absurd sizes / bad title: app dropped, desktop intact')
            # 1: ignores close -> SIGKILL after grace
            key(g,b'h'); time.sleep(3)
            im=shot(g,'h2'); check(im.getpixel((600,380))!=desk.getpixel((600,380)), 'close-ignoring app got its window')
            click(490+300-18, 300+12); click(1100,650); time.sleep(4)
            check(restored('h3',pts), 'its window closed via [X]')
            # 2: flood
            key(g,b'h'); time.sleep(5)
            im=shot(g,'h4'); check(im.getpixel((600,262))!=desk.getpixel((600,262)), 'flooding app got its window (title bar)')
            click(440+400-18, 250+12); click(1100,650); time.sleep(4)
            check(restored('h5',pts), 'desktop stayed responsive under a 20000-request flood; [X] closed it')
            # 3: exits without a window
            key(g,b'h'); time.sleep(3)
            check(restored('h6',pts), 'app exiting before CREATE leaves no window')
            # 4-6: malformed open-in-editor requests drop the app, open nothing.
            for mode, what in [(4, 'a relative path'), (5, 'an oversized length'), (6, 'an incomplete path')]:
                key(g, b'h'); time.sleep(3)
                check(restored(f'h{mode + 3}', pts), f'open request with {what}: app dropped, desktop intact')
            g.pump()
            check('Editor opened for app' not in g.text(start), 'no editor started for malformed requests')
            # 7: 50 valid opens in a burst start exactly one editor.
            key(g, b'h'); time.sleep(6); g.pump()
            n = g.text(start).count('Editor opened for app')
            check(n == 1, f'50 valid open requests in a burst started {n} editor(s) (expect 1)')
            # Close the editor (cascaded second app) and the hostile window.
            click(432 + 480 - 18, 292 + 12); click(1100, 650); time.sleep(3)
            click(490 + 300 - 18, 300 + 12); click(1100, 650); time.sleep(4)
            check(restored('h11', pts), 'editor and hostile window closed')
            alive='Event loop stopped' not in g.text(start)
            check(alive, 'compositor survived all eight attacks')
            key(g,b'q')
            g.wait(lambda s:'Event loop stopped' in s,start,timeout=30)
            g.wait(lambda s:s.endswith('$ '),start,timeout=20)
            time.sleep(1); ps=g.command('ps')
            check('hostile-window' not in ps and 'notepad' not in ps, 'every hostile process is gone (incl. the close-ignoring one)')
            print('\n'.join(l for l in g.text(start).split('\n') if 'COMPOSITOR' in l and 'PERF' not in l))
            return all(results)
        finally:
            g.close()


def run_notepad():
    """/bin/notepad as its own process: launched with 'e', a click places
    the cursor, typed text is saved with Ctrl+S as the session user (not
    root), [X] closes it and no process remains."""
    def shot(g, name):
        g.qmp.execute('screendump', {'filename': f'/tmp/{name}.ppm'}); time.sleep(.4)
        return Image.open(f'/tmp/{name}.ppm').convert('RGB')
    results = []
    def check(ok, what): results.append(ok); print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    with tempfile.TemporaryDirectory() as t:
        tmp=Path(t); g=Guest(firmware='bios',smp=1,tmp=tmp)
        try:
            g.wait(lambda s: s.endswith('FortressOS login: '), timeout=120)
            g.qmp=QMP(tmp/'qmp.sock')
            g.send('operator'); g.wait(lambda s: s.endswith('$ '), timeout=30)
            g.command('rm -f /tmp/notes.txt')
            start=len(g.data)
            g.send('sudo /bin/compositor')
            g.wait(lambda s: 'Window server ready' in s, start, timeout=40)
            wait_image(g,lambda iw,ih,rgb:pixel(rgb,iw,iw//2,ih//2)==bytes.fromhex('101010'))
            time.sleep(2); desk=shot(g,'n0')
            key(g,b'e',3)
            im=shot(g,'n1')
            # notepad window 480x280 centred: x 400..880, y 260..540; header row at content y<20
            try:
                g.wait(lambda s:'Notepad started' in s, start, timeout=10); launched=True
            except AssertionError:
                launched=False
            check(launched, 'compositor launched /bin/notepad')
            check(im.getpixel((650,292))==(0x1e,0x1e,0x1e), 'notepad header drawn by the app')
            # click at the start of text line 0 (content x 48, y 24+8) and type
            pos=[640,400]
            def click(x,y):
                while pos!=[x,y]:
                    dx=max(-100,min(100,x-pos[0]));dy=max(-100,min(100,y-pos[1])); g.motion(dx,dy); pos[0]+=dx; pos[1]+=dy; time.sleep(.04)
                time.sleep(.3)
                for down in [True,False]:
                    g.qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'down':down,'button':'left'}}]}); time.sleep(.15)
            click(401+48+2, 285+24+8); time.sleep(1)
            for ch in b'XY': key(g,bytes([ch]),0.25)
            time.sleep(1); im=shot(g,'n2')
            key(g,b'\x13',2)   # Ctrl+S
            im=shot(g,'n3')
            # close the window with [X]
            click(400+480-18, 260+12); click(1100,650); time.sleep(4)
            im=shot(g,'n4')
            check(im.getpixel((600,400))==desk.getpixel((600,400)), 'notepad window closed via [X]')
            key(g,b'q')
            g.wait(lambda s:'Event loop stopped' in s,start,timeout=30)
            g.wait(lambda s:s.endswith('$ '),start,timeout=20)
            time.sleep(1)
            content=g.command('cat /tmp/notes.txt')
            print('--- /tmp/notes.txt:'); print(content)
            check(content.split('\n',1)[1].lstrip().startswith('XY#') or 'XY# FortressOS' in content, 'click placed the cursor and typed text was saved')
            ls=g.command('ls -l /tmp/notes.txt')
            print(ls)
            check('operator' in ls and ' root ' not in ls, 'file written as the user, not root')
            check('notepad' not in g.command('ps'), 'no notepad process left')
            return all(results)
        finally:
            g.close()


def run_files():
    """/bin/files as its own process: 'f' launches it, the [/etc] button
    navigates, Enter on a file makes the desktop open it in the editor, 'u'
    goes back up, [X] closes both and no process remains."""
    def shot(g, name):
        g.qmp.execute('screendump', {'filename': f'/tmp/{name}.ppm'}); time.sleep(.4)
        return Image.open(f'/tmp/{name}.ppm').convert('RGB')
    results = []
    def check(ok, what): results.append(ok); print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t); g = Guest(firmware='bios', smp=1, tmp=tmp)
        try:
            g.wait(lambda s: s.endswith('FortressOS login: '), timeout=120)
            g.qmp = QMP(tmp / 'qmp.sock')
            g.send('operator'); g.wait(lambda s: s.endswith('$ '), timeout=30)
            start = len(g.data)
            g.send('sudo /bin/compositor')
            g.wait(lambda s: 'Window server ready' in s, start, timeout=40)
            wait_image(g, lambda iw, ih, rgb: pixel(rgb, iw, iw // 2, ih // 2) == bytes.fromhex('101010'))
            time.sleep(2); desk = shot(g, 'f0')
            pos = [640, 400]
            def click(x, y):
                while pos != [x, y]:
                    dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                    g.motion(dx, dy); pos[0] += dx; pos[1] += dy; time.sleep(.04)
                time.sleep(.3)
                for down in [True, False]:
                    g.qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]}); time.sleep(.15)
            key(g, b'f', 3); g.pump()
            check('Files started' in g.text(start), 'compositor launched /bin/files')
            root = shot(g, 'f1')
            # Files: 460x300 centred -> x 410, y 250; content origin (411, 275).
            check(root.getpixel((600, 262)) != desk.getpixel((600, 262)), 'Files window shown')
            click(411 + 170, 275 + 10); click(1100, 650); time.sleep(2)
            etc = shot(g, 'f2')
            region = [(x, y) for x in range(430, 860, 40) for y in range(305, 520, 20)]
            check(any(etc.getpixel(p) != root.getpixel(p) for p in region), '[/etc] button changed the listing')
            opened = False
            for k in range(1, 8):
                for _ in range(k): key(g, b'j', 0.3)
                key(g, b'\r', 2); g.pump()
                if 'Editor opened for app' in g.text(start):
                    opened = True; break
                key(g, b'u', 1)  # it was a directory: back to /etc
            check(opened, 'Enter on a file in /etc opened it in the editor')
            time.sleep(2); im = shot(g, 'f3')
            check(im.getpixel((682, 324)) == (0x1e, 0x1e, 0x1e), 'editor window drawn (cascaded)')
            click(432 + 480 - 18, 292 + 12); click(1100, 650); time.sleep(3)
            key(g, b'u', 1.5)
            up = shot(g, 'f4')
            check(all(up.getpixel(p) == root.getpixel(p) for p in region), "'u' returned to the / listing")
            click(410 + 460 - 18, 250 + 12); click(1100, 650); time.sleep(4)
            fin = shot(g, 'f5')
            check(all(fin.getpixel(p) == desk.getpixel(p) for p in [(300, 200), (600, 380), (1000, 600)]), 'both windows closed')
            key(g, b'q')
            g.wait(lambda s: 'Event loop stopped' in s, start, timeout=30)
            g.wait(lambda s: s.endswith('$ '), start, timeout=20)
            time.sleep(1); ps = g.command('ps')
            check('files' not in ps and 'notepad' not in ps, 'no files or notepad process left')
            return all(results)
        finally:
            g.close()


if __name__ == '__main__':
    print('[TEST] desktop clients: well-behaved app', flush=True)
    ok = run_well_behaved()
    print('[TEST] desktop clients: hostile app', flush=True)
    ok = run_hostile() and ok
    print('[TEST] desktop clients: notepad', flush=True)
    ok = run_notepad() and ok
    print('[TEST] desktop clients: files', flush=True)
    ok = run_files() and ok
    print('[PASS] desktop clients' if ok else '[FAIL] desktop clients')
    sys.exit(0 if ok else 1)
