#!/usr/bin/env python3
"""Loopback-only controlled DNS peer; pipe closure stops and joins it."""
import sys
from net_dns_peer import HostDNS
peer=HostDNS()
print('READY',flush=True)
try: sys.stdin.read()
finally: peer.close()
