#!/usr/bin/env python3
"""Out-of-process desktop apps (user/compositor/src/clients.rs, proto.rs).

well_behaved: /bin/hello-window draws, receives keys, retitles, quits, is
relaunched and closed with [X]; no process remains.
hostile: /bin/hostile-window (via --app=) tries absurd sizes and a bad
title, ignores close, floods 20000 requests and exits early; the desktop
must stay intact and responsive and no hostile process may survive.
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
            before=shot(g,'a0')
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
            alive='Event loop stopped' not in g.text(start)
            check(alive, 'compositor survived all four attacks')
            key(g,b'q')
            g.wait(lambda s:'Event loop stopped' in s,start,timeout=30)
            g.wait(lambda s:s.endswith('$ '),start,timeout=20)
            time.sleep(1); ps=g.command('ps')
            check('hostile-window' not in ps, 'every hostile process is gone (incl. the close-ignoring one)')
            print('\n'.join(l for l in g.text(start).split('\n') if 'COMPOSITOR' in l and 'PERF' not in l))
            return all(results)
        finally:
            g.close()


if __name__ == '__main__':
    print('[TEST] desktop clients: well-behaved app', flush=True)
    ok = run_well_behaved()
    print('[TEST] desktop clients: hostile app', flush=True)
    ok = run_hostile() and ok
    print('[PASS] desktop clients' if ok else '[FAIL] desktop clients')
    sys.exit(0 if ok else 1)
