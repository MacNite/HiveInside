#!/usr/bin/env python3
"""Report which MCUboot signing key a bootloader image trusts.

MCUboot embeds the public half of its signing key in the bootloader. Releases up
to v0.6 were built with the SDK's public development key, and a node whose
bootloader trusts that key accepts firmware signed by anybody (the private half
ships with MCUboot). This script answers the question for a concrete image:

    check_bootloader_key.py --key project.pem --dev-keys <mcuboot>/ boot.bin

It looks for the exact public-key bytes MCUboot compiles in (imgtool's `getpub
-e raw` encoding) and:

  * prints the SHA-256 fingerprint of the expected key, so a release can record
    which key its bootloader trusts;
  * fails if that key is not embedded;
  * fails if any MCUboot development key (`root-*.pem` in --dev-keys) is.

The image may be a raw bootloader `.bin`, a bootloader `.hex`, a release
`-factory.hex` (bootloader plus application) or a bootloader region read back
from a device. Only the first --region bytes (default 64 KiB, the boot
partition) are searched so the application's own data cannot match.

Needs MCUboot's `imgtool` package: either `pip install imgtool`, or point
--imgtool at `<west workspace>/bootloader/mcuboot/scripts`. `.hex` input also
needs `intelhex`.
"""

import argparse
import hashlib
import pathlib
import sys


def load_image(path, region):
    if path.suffix.lower() == ".hex":
        from intelhex import IntelHex

        ih = IntelHex(str(path))
        start = ih.minaddr()
        end = min(ih.maxaddr() + 1, start + region)
        return ih.tobinstr(start=start, end=end - 1)
    return path.read_bytes()[:region]


def public_bytes(key_path):
    from imgtool import keys

    return keys.load(str(key_path)).get_public_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", type=pathlib.Path,
                    help="bootloader .bin/.hex or release -factory.hex")
    ap.add_argument("--key", type=pathlib.Path,
                    help="expected signing key (private or public PEM)")
    ap.add_argument("--dev-keys", type=pathlib.Path,
                    help="MCUboot checkout; its root-*.pem keys must be absent")
    ap.add_argument("--imgtool", type=pathlib.Path,
                    help="directory containing the imgtool package")
    ap.add_argument("--region", type=lambda v: int(v, 0), default=0x10000,
                    help="bytes from the image start to search (default 0x10000)")
    args = ap.parse_args()

    if args.key is None and args.dev_keys is None:
        ap.error("give --key, --dev-keys or both")
    if args.imgtool:
        sys.path.insert(0, str(args.imgtool))

    image = load_image(args.image, args.region)
    ok = True

    if args.key is not None:
        pub = public_bytes(args.key)
        fingerprint = hashlib.sha256(pub).hexdigest()
        if pub in image:
            print(f"trusted key   : sha256:{fingerprint} (embedded)")
        else:
            print(f"trusted key   : sha256:{fingerprint} NOT FOUND in {args.image}",
                  file=sys.stderr)
            ok = False

    if args.dev_keys is not None:
        dev_keys = sorted(args.dev_keys.glob("root-*.pem"))
        if not dev_keys:
            print(f"no root-*.pem development keys under {args.dev_keys}",
                  file=sys.stderr)
            return 2
        for dev_key in dev_keys:
            try:
                pub = public_bytes(dev_key)
            except Exception:  # an unsupported key type cannot be embedded either
                continue
            if pub in image:
                print(f"DEVELOPMENT KEY EMBEDDED: {dev_key.name} — this bootloader "
                      "accepts firmware signed by anyone", file=sys.stderr)
                ok = False
        if ok:
            print(f"dev keys      : none of {len(dev_keys)} MCUboot development keys embedded")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
