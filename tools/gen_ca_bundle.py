#!/usr/bin/env python3
"""Builds certs/x509_crt_bundle.bin - the root CA bundle the bridge uses to
verify api.github.com and GitHub's release-asset host (see src/Updater.cpp).

    python tools/gen_ca_bundle.py                     # from certifi's Mozilla bundle
    python tools/gen_ca_bundle.py path/to/cacert.pem  # from any PEM bundle

Output format is the one the Arduino core's WiFiClientSecure/esp_crt_bundle.c
parses (identical to ESP-IDF's own gen_crt_bundle.py output):

    u16be  number of certificates
    per certificate, sorted by subject name bytes:
        u16be  subject name length
        u16be  public key length
        subject Name, DER (the whole SEQUENCE)
        SubjectPublicKeyInfo, DER (the whole SEQUENCE)

Only the subject and key of each root are kept - the verifier binary-searches
the bundle for the issuer of the first untrusted certificate in the chain and
checks that certificate's signature with the stored key. A full Mozilla bundle
is ~65 KB of flash and costs almost no RAM (just an index), which is why the
whole set is embedded rather than the two or three roots GitHub uses today: a
hand-picked list would break the updater silently the day GitHub changes CA.

The output is committed (the build does not regenerate it) so a firmware build
is reproducible and a change to the trusted set shows up in review. Re-run this
occasionally to pick up Mozilla's additions and removals.

Standard library only - a minimal DER walker instead of the `cryptography`
package, so it runs in PlatformIO's own Python without installing anything.
"""

import base64
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "certs", "x509_crt_bundle.bin")


def default_source():
    try:
        import certifi  # noqa: F401
        return certifi.where()
    except ImportError:
        pass
    # pip always vendors certifi, so PlatformIO's own penv has one.
    from pip._vendor import certifi  # type: ignore
    return certifi.where()


def read_tlv(data, pos):
    """Returns (tag, start_of_value, end_of_value, end_of_element)."""
    tag = data[pos]
    length = data[pos + 1]
    hdr = 2
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(data[pos + 2:pos + 2 + n], "big")
        hdr = 2 + n
    start = pos + hdr
    return tag, start, start + length, start + length


def subject_and_key(der):
    # Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
    _, cert_start, _, _ = read_tlv(der, 0)
    # TBSCertificate ::= SEQUENCE { [0] version OPTIONAL, serialNumber,
    #   signature, issuer, validity, subject, subjectPublicKeyInfo, ... }
    _, pos, _, _ = read_tlv(der, cert_start)
    fields = []
    while len(fields) < 7:
        tag, _, _, end = read_tlv(der, pos)
        fields.append((tag, pos, end))
        pos = end
    if fields[0][0] != 0xA0:  # v1 certificate, no explicit version
        fields.insert(0, None)
    _, s_start, s_end = fields[5]
    _, k_start, k_end = fields[6]
    return der[s_start:s_end], der[k_start:k_end]


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else default_source()
    with open(source, "r", encoding="ascii", errors="replace") as f:
        pem = f.read()
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", pem, re.S)

    entries = {}
    for block in blocks:
        der = base64.b64decode("".join(block.split()))
        name, key = subject_and_key(der)
        # Same subject twice (a re-keyed root) - the verifier can only find one
        # by binary search anyway; keep the first, as ESP-IDF's tool does.
        entries.setdefault(name, key)

    out = bytearray(struct.pack(">H", len(entries)))
    for name in sorted(entries):
        key = entries[name]
        out += struct.pack(">HH", len(name), len(key)) + name + key

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(out)
    print("%s: %d roots, %d bytes (from %s)" % (os.path.normpath(OUT), len(entries), len(out), source))


if __name__ == "__main__":
    main()
