#!/bin/bash
# Pinned, portable constant-time AES for the SWEET legacy CBC/ECB API.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f libkern/crypto/corecrypto_aes.c ]] || exit 1
bearssl_sha=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f
bearssl_tmp=$(mktemp -d)
trap 'rm -rf "$bearssl_tmp"' EXIT

# bearssl.org answers 502 for long stretches. Its git server is the canonical
# source; when it is down take the same commit as a tarball from the GitHub
# mirror, which needs no credentials (fetching a bare sha over https would ask
# for a username).
if git init -q "$bearssl_tmp" 2>/dev/null &&
   git -C "$bearssl_tmp" fetch -q --depth 1 \
        https://www.bearssl.org/git/BearSSL "$bearssl_sha" 2>/dev/null; then
    git -C "$bearssl_tmp" checkout -q FETCH_HEAD
    [[ "$(git -C "$bearssl_tmp" rev-parse HEAD)" == "$bearssl_sha" ]]
else
    # bearssl.org's git endpoint has been answering 502; a plain mirror on
    # GitHub carries the same tree (the AES and hash sources are stable).
    echo "bearssl.org unreachable, taking the mirror tarball"
    rm -rf "$bearssl_tmp"
    mkdir -p "$bearssl_tmp"
    ok=0
    for attempt in 1 2 3 4 5; do
        if curl -sSL --max-time 120 -o "$bearssl_tmp/src.tar.gz" \
                "https://codeload.github.com/unkaktus/bearssl/tar.gz/refs/heads/master" &&
           [ "$(stat -c %s "$bearssl_tmp/src.tar.gz" 2>/dev/null || stat -f %z "$bearssl_tmp/src.tar.gz")" -gt 300000 ] &&
           tar -xzf "$bearssl_tmp/src.tar.gz" -C "$bearssl_tmp" --strip-components=1 2>/dev/null; then
            ok=1
            break
        fi
        sleep 5
    done
    rm -f "$bearssl_tmp/src.tar.gz"
    [ "$ok" = 1 ] || { echo "could not obtain BearSSL sources"; exit 1; }
    [[ -d "$bearssl_tmp/inc" && -f "$bearssl_tmp/src/inner.h" ]]
fi
python3 - "$bearssl_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('libkern/crypto/sweet_bearssl')
if target.exists():
    raise SystemExit(f'refusing to overwrite existing AES source: {target}')
shutil.copytree(source / 'inc', target / 'inc')
(target / 'src/symcipher').mkdir(parents=True)
for file in ('inner.h', 'config.h'):
    shutil.copy2(source / 'src' / file, target / 'src' / file)
# XNU exports machine/limits.h instead of the userspace limits.h.
header = target / 'src/inner.h'
text = header.read_text()
assert text.count('#include <limits.h>') == 1
header.write_text(text.replace('#include <limits.h>',
    '#if defined(KERNEL)\n#include <machine/limits.h>\n'
    '#else\n#include <limits.h>\n#endif'))
for name in ('aes_ct', 'aes_ct_enc', 'aes_ct_dec', 'aes_ct_cbcenc', 'aes_ct_cbcdec'):
    shutil.copy2(source / 'src/symcipher' / (name + '.c'),
                 target / 'src/symcipher' / (name + '.c'))
# MD5, SHA-1 and SHA-384/512 for the legacy hash APIs, with their codecs.
for name in ('hash/md5.c', 'codec/dec32le.c', 'codec/enc32le.c',
             'hash/sha1.c', 'codec/dec32be.c', 'codec/enc32be.c',
             'hash/sha2big.c', 'codec/dec64be.c', 'codec/enc64be.c'):
    (target / 'src' / name).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / 'src' / name, target / 'src' / name)
shutil.copy2(source / 'LICENSE.txt', target / 'LICENSE.txt')
PY
echo "Staged BearSSL portable AES, MD5, SHA-1 and SHA-384/512 ($bearssl_sha) for SWEET"
