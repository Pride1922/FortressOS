#!/usr/bin/env python3
"""Independent SP800-90A 10.1.2 oracle using Python's OpenSSL-backed HMAC."""
import hashlib
import hmac
import subprocess

k, v = bytes(32), bytes([1])*32
def mac(data):
    return hmac.new(k, data, hashlib.sha256).digest()
def update(seed):
    global k, v
    k = mac(v + b'\0' + seed)
    v = mac(v)
    if seed:
        k = mac(v + b'\1' + seed)
        v = mac(v)
def generate():
    global v
    out = b''
    for _ in range(2):
        v = mac(v)
        out += v
    update(b'')
    return out.hex()
update(bytes(range(48)))
expected = [generate(), generate()]
update(bytes(range(32)))
expected.append(generate())
actual = subprocess.check_output(['build/random_host', '--vector'], text=True).splitlines()
assert actual == expected, (actual, expected)
print('PASS independent hashlib HMAC-DRBG instantiate/generate/reseed vectors')
