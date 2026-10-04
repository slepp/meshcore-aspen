#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write only the clearly labelled host fixture to an explicitly selected HTTPS endpoint."""
import argparse
from pathlib import Path
import ssl
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url")
    parser.add_argument("body", type=Path)
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
        parser.error("A certificate-verified HTTPS write URL without credentials or fragment is required")
    body = args.body.read_bytes()
    if not 0 < len(body) <= 6144 or b"device=host-protocol-fixture" not in body:
        parser.error("Expected the bounded telemetry encoder host fixture")
    request = urllib.request.Request(args.url, body, {"Content-Type": "text/plain"}, method="POST")
    opener = urllib.request.build_opener(
        urllib.request.HTTPSHandler(context=ssl.create_default_context()), NoRedirect())
    try:
        with opener.open(request, timeout=20) as response:
            if not 200 <= response.status < 300:
                raise RuntimeError(f"Unexpected write HTTP {response.status}")
            print(f"PASS verified HTTPS Influx host fixture: HTTP {response.status}, {len(body)} bytes; "
                  "no physical-device delivery implied")
    except urllib.error.HTTPError as error:
        raise SystemExit(f"Write failed: HTTP {error.code}; configure endpoint/auth, no redirect or TLS bypass") from None


if __name__ == "__main__":
    main()
