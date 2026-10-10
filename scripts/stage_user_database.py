"""Root-owned live-media database; explicit hashes travel through a file only.

FORTRESS_OPERATOR_HASH_FILE is an optional build environment variable. No
password/plaintext/default hash is stored in source, argv or build logs.
"""
import os,re,sys
from pathlib import Path
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
hash_file=os.environ.get('FORTRESS_OPERATOR_HASH_FILE')
value=Path(hash_file).read_text().strip() if hash_file else ''
if value:
    match=re.fullmatch(r'\$5\$(?:rounds=(\d+)\$)?[./0-9A-Za-z]{1,16}\$[./0-9A-Za-z]{43}',value)
    if not match or match[1] and not 1000<=int(match[1])<=100000:
        raise SystemExit('Unsupported operator hash (SHA256-crypt, 1000..100000 rounds required)')
(root/'passwd').write_text('root:x:0:0:Locked live-media root:/root:/bin/shell\noperator:x:1000:1000:Live operator:/run/user/1000:/bin/shell\n')
(root/'group').write_text('root:x:0:\ntty:x:5:\ndisk:x:6:\nwheel:x:10:operator\nvideo:x:44:operator\ninput:x:104:operator\noperator:x:1000:\n')
(root/'shadow').write_text('root:!:::::::\n')
with (root/'shadow').open('a') as f:f.write('operator:'+value+':::::::\n')
(root/'shadow').chmod(0o600)
