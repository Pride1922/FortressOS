# Literal RFC wire vector worksheet

These expected bytes were specified before the peer/audit implementation.
Layout follows RFC 791/9293; checksum arithmetic follows RFC 1071. Arithmetic
was independently checked with a one-off word-sum calculation, not either
implementation. Do not regenerate expected hex with the peer or audit.

Ethernet bytes 0..5 = 52:54:00:12:34:56 destination, 6..11 =
02:03:04:05:06:07 source, 12..13 = 0800. IPv4 begins at 14, TCP at 34.
IP version/IHL=45, DSCP=00, ID=1234, flags/offset=4000, TTL=40, protocol=06,
source=0a000202, destination=0a00020f. IP checksum is bytes 24..25.
TCP source=1e61 (7777), destination=9c40 (40000), sequence=01020304 except
wrap=fffffffe, ACK=10203040 except SYN=00000000, window=2000, urgent=0000.
TCP checksum is bytes 50..51. Header offset/flags=5002 SYN, 5012 SYN/ACK,
5010 ACK, 5018 data, 5011 FIN/ACK, 5014 RST/ACK. MSS SYN uses 6002 plus
02040218 (kind 2, length 4, value 536). Data is 616263 (abc); checksum-only
odd-byte pad adds a zero octet, not an extra IP/TCP payload byte.

Sum all 16-bit network-order words with checksum fields zero; fold end-around
carries until 16 bits remain, then invert. For the 0028-byte IPv4 packet:

```
4500 + 0028 + 1234 + 4000 + 4006 + 0000 + 0a00 + 0202 + 0a00 + 020f
= ef73; complement = 108c
```

For plain SYN, the pseudo-header words are 0a00 0202 0a00 020f 0006 0014.
TCP words are 1e61 9c40 0102 0304 0000 0000 5002 2000 0000 0000:

```
raw sum = 146d4; fold = 46d4 + 1 = 46d5; complement = b92a
```

| Vector | IP total / checksum | Pseudo TCP length | TCP raw sum -> folded -> complement |
| --- | --- | --- | --- |
| SYN | 0028 / 108c | 0014 | 146d4 -> 46d5 -> b92a |
| SYN MSS | 002c / 1088 | 0018 | 15af4 -> 5af5 -> a50a |
| SYN/ACK | 0028 / 108c | 0014 | 18744 -> 8745 -> 78ba |
| ACK | 0028 / 108c | 0014 | 18742 -> 8743 -> 78bc |
| odd data abc | 002b / 1089 | 0017 | 24baf -> 4bb1 -> b44e |
| FIN/ACK | 0028 / 108c | 0014 | 18743 -> 8744 -> 78bb |
| RST/ACK | 0028 / 108c | 0014 | 18746 -> 8747 -> 78b8 |
| wrap data abc | 002b / 1089 | 0017 | 447a6 -> 47aa -> b855 |

Minimum Ethernet frame length is 60 bytes without FCS. Add zero Ethernet pad
after the IP total: six bytes for plain control, two for MSS SYN and three for
odd data. SYN/FIN each advance sequence space once; wrap data begins at fffffffe
and its three bytes end at sequence 00000001. The JSON records literal bytes
and field expectations, independent of any calculated test output.
