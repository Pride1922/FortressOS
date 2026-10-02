"""Runtime worker-ordering guard must trap; do not retain a core dump."""
import os
import resource
import signal
import subprocess

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

r = subprocess.run(['build/net_tcp_socket_host'],
                   env=dict(os.environ, NET_TCP_EXPIRY_GUARD='1'),
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   preexec_fn=no_core, timeout=10)
assert r.returncode == -signal.SIGILL, (r.returncode, r.stdout, r.stderr)
print('TCP deadline worker prepare/commit runtime guard: injected expiry traps PASS')
