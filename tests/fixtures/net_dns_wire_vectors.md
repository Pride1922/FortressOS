# DNS golden worksheet

These are hand-computed wire bytes, independent of the guest encoder and the
synthetic DNS peer. The JSON literal and the C host-test literal must agree.
DNS has no message checksum of its own; UDP/TCP pseudo-header checksums are
separately checked by the wire auditor and the accepted transport suites.

| Offset | Bytes / meaning |
| --- | --- |
| 0–1 | 12 34: transaction ID |
| 2–3 | Query 01 00: standard query, RD; response 81 80: QR, RD, RA, NOERROR |
| 4–11 | Query counts 1/0/0/0; A response 1/1/0/0 |
| 12 | 07: example label length |
| 13–19 | ASCII example |
| 20 | 04: test label length |
| 21–24 | ASCII test |
| 25 | 00: root terminator |
| 26–29 | 00 01 / 00 01: type A / class IN |
| 30–31 | c0 0c: answer owner points back to name at 12 |
| 32–35 | 00 01 / 00 01: type A / class IN |
| 36–39 | 00 00 00 3c: TTL 60 seconds |
| 40–41 | 00 04: address RDATA length |
| 42–45 | c0 00 02 07: 192.0.2.7 |

Query length 30, response length 46. DNS-over-TCP prepends 00 2e to the
46-byte response; the prefix is not included in that length. Host tests also
contain a literal CNAME response with its second owner pointing to offset 40.
See [the codec requirements](../../docs/plans/NET2_STEP7_DNS.md#b--pure-codec-and-hostile-input-tests).
