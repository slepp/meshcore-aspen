#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build Willow's normal shared libcurl with HTTP, HTTPS and WebSockets."""

import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request


def prepare(source, inside, archive=None):
    pin = json.loads((source / "release/willow-runtime.json").read_text())["curl"]
    work = source / "hosts/willow/build/curl"
    work.mkdir(exist_ok=True)
    cached = work / Path(pin["url"]).name
    if archive:
        shutil.copy2(archive, cached)
    elif not cached.exists():
        with (
            urllib.request.urlopen(pin["url"], timeout=60) as remote,
            cached.open("xb") as output,
        ):
            shutil.copyfileobj(remote, output)
    with cached.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != pin["sha256"]:
            raise ValueError(
                "Willow curl source hash differs from release/willow-runtime.json"
            )
    if not (work / "source").exists():
        with tarfile.open(cached) as package:
            package.extractall(work, filter="data")
        (work / ("curl-" + pin["version"])).rename(work / "source")
    inside(
        [
            "/bin/sh",
            "-eu",
            "-c",
            """
cd build/curl/source
./configure --prefix=/src/hosts/willow/build/curl/install \
  --with-openssl --enable-websockets --enable-shared --disable-static \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs \
  --disable-ldap --disable-ldaps --disable-dict --disable-ftp --disable-file \
  --disable-gopher --disable-imap --disable-ipfs --disable-mqtt --disable-pop3 --disable-rtsp \
  --disable-smb --disable-smtp --disable-telnet --disable-tftp --disable-manual \
  --without-libpsl --without-libidn2 --without-brotli --without-zstd \
  --without-libssh2 --without-librtmp --without-nghttp2 --without-nghttp3 \
  --without-ngtcp2 > ../configure.log 2>&1
make -j2 > ../build.log 2>&1
make install > ../install.log 2>&1
""",
        ]
    )
    library = source / "hosts/willow/build/lib"
    library.mkdir(exist_ok=True)
    shutil.copy2(work / "install/lib/libcurl.so.4", library / "libcurl.so.4")
    return pin
