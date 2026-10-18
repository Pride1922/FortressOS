IANA tzdata 2026c, public-domain data (LICENSE is inside the archive).

Source: https://data.iana.org/time-zones/releases/tzdata2026c.tar.gz

SHA-256: e4a178a4477f3d0ea77cc31828ff72aa38feff8d61aa13e7e99e142e9d902be4

`scripts/build_zoneinfo.py` verifies the archive and compiles it with `zic`.
Its `--pack` mode bundles the zones into one `usr/share/zoneinfo.tzdb` file,
preserving the installer's existing entry-count bound.
Updates must deliberately replace the archive, version and digest, then rerun
the timezone differential tests. Release builds do not download timezone data.
