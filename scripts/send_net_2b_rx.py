#!/usr/bin/env python3
"""Send the documented RX-liveness frame from a Linux wired peer."""
import argparse
import socket
import time


def make_frame(source, destination):
    return destination + source + bytes.fromhex("88b5") + b"FORTRESS-NET-2B-RX" + b"\xa5" * 28


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("interface", help="explicit wired interface on the peer")
    parser.add_argument("--mac", default="c8:f7:50:0e:35:80", help="Dell destination MAC")
    parser.add_argument("--seconds", type=float, default=90, help="send window, default 90s; start before Dell boot")
    args = parser.parse_args()
    try:
        destination = bytes.fromhex(args.mac.replace(":", ""))
    except ValueError:
        parser.error("invalid destination MAC")
    if len(destination) != 6 or not 0 < args.seconds <= 300:
        parser.error("require a six-byte MAC and seconds in (0, 300]")
    try:
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x88b5)) as peer:
            peer.bind((args.interface, 0))
            source = peer.getsockname()[4]
            if len(source) != 6:
                parser.error("interface does not have a six-byte Ethernet address")
            frame = make_frame(source, destination)
            print(f"Sending {len(frame)} bytes to {args.mac} on {args.interface}, 10 frames/s. Boot Dell now.", flush=True)
            deadline = time.monotonic() + args.seconds
            count = 0
            while time.monotonic() < deadline:
                if peer.send(frame) != len(frame):
                    raise OSError("short raw-frame send")
                count += 1
                time.sleep(0.1)
            print(f"Submitted {count} frames; peer submission does not prove Dell reception.")
    except OSError as error:
        parser.exit(1, f"Peer send failed: {error} (raw sockets need root/CAP_NET_RAW).\n")


if __name__ == "__main__":
    main()
