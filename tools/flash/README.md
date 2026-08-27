# HiveInside flashing tools

Flash a **prebuilt** HiveInside image onto a Seeed XIAO nRF54LM20A Sense with
nothing but OpenOCD and a USB-C cable — no Zephyr toolchain, no west workspace,
no external probe. The board's on-board SAMD11 CMSIS-DAP debugger does the work.

These scripts also ship as `hiveinside-flash-tools.zip` on every
[release](https://github.com/MacNite/HiveInside/releases), together with the
board's OpenOCD config taken from the exact Zephyr revision the images were
built against.

If you already have a west workspace, keep using `west flash` — see
[`docs/flashing.md`](../../docs/flashing.md). This is for everyone else.

## Which file do I flash?

Each release carries two images per variant. They are not interchangeable:

| File | Use it for | How |
| --- | --- | --- |
| `hiveinside-nrf54lm20a-v<version>-<variant>-factory.hex` | A **new or bricked board**, over USB | the scripts here |
| `hiveinside-nrf54lm20a-v<version>-<variant>.signed.bin` | **Updating a running node**, over the air | upload it in HiveHub |

> ⚠️ Never flash the `.signed.bin` over SWD. It is the application alone, so it
> leaves nothing at `0x0`: the CPU faults before `main()` and the device goes
> completely silent — no console, no BLE. The scripts refuse it for you.

And pick the right **variant**:

* **`lowpower`** — the deployment profile. No console, no LED, minimum idle
  current. This is what goes into a hive.
* **`bringup`** — console on at 115200 8N1. Use it on the bench, while wiring up
  an SHT40 or watching the readout.

Both advertise identically, so a `bringup` image in a sealed hive looks fine
from HiveHub and simply drains the battery. Check the suffix before you flash.

## Prerequisites

OpenOCD 0.12 or newer:

```bash
sudo apt install openocd      # Debian/Ubuntu
brew install open-ocd         # macOS
sudo pacman -S openocd        # Arch
winget install OpenOCD.OpenOCD   # Windows (or the xPack OpenOCD build)
```

On Linux, allow non-root access to the debugger (Seeed VID `2886`):

```bash
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="2886", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/99-seeed-cmsis-dap.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Replug the board afterwards.

## Flashing

Unzip the release bundle, put the factory hex next to the scripts (or pass its
path), and run:

```bash
./flash.sh                                     # picks the single factory hex it finds
./flash.sh hiveinside-nrf54lm20a-v0.5.0-lowpower-factory.hex
```

```powershell
.\flash.ps1
.\flash.ps1 -Image hiveinside-nrf54lm20a-v0.5.0-lowpower-factory.hex
```

Both take `--openocd-cfg` / `-OpenocdCfg`, `--openocd` / `-Openocd` and
`--no-verify` / `-NoVerify`; `--help` prints the details.

The scripts run the same OpenOCD sequence `west flash --verify` runs for this
board, and verify the image by reading it back. **Leave the verification on** —
see below.

## Why the scripts patch the board config

The board's OpenOCD flash loader upstream is:

```tcl
proc nrf54lm20a-load {file} {
    mww 0x5004e500 0x101
    load_image $file
}
```

`0x5004e500` is `RRAMC.CONFIG`; `0x101` sets `WEN=1` **and a one-line (16-byte)
write buffer**, and the proc never commits that buffer. The nRF54L RRAM
controller only writes a 128-bit line out once the line fills, so any tail that
does not reach a 16-byte boundary stays in the buffer and never lands in RRAM.
On a signed image that truncates the signature TLV, and MCUboot then reports
`E: Image in the primary slot is not valid!` — with a silent, dark device to
show for it, because the bootloader console is disabled for size.

The scripts copy the config to a temp file and write `0x1` instead: same
register, write buffer off, every write commits. Flashing is slower, and
correct. Nothing on your disk is modified, and if a future Zephyr fixes the
loader upstream the scripts detect that and use the config unchanged.

This is upstream board support, not a HiveInside bug, and it affects any image
whose length is not a multiple of 16 bytes.

## Verifying what you downloaded

```bash
sha256sum -c SHA256SUMS      # shasum -a 256 -c SHA256SUMS on macOS
```

`manifest.txt` in the same release lists each OTA payload's byte size and
CRC-32 — the two values the OTA `BEGIN` frame carries — plus the Zephyr
revision and the commit the images were built from.

**On authenticity:** release images are signed with MCUboot's **development**
key, which is public. That gives image formatting, not proof of origin: a
device running these images will accept any image signed with the same public
key. Check the SHA-256 above to know you got the file we built.

## When it does not work

`docs/flashing.md` in the repository has the full troubleshooting path —
a silent device after flashing, `E: Image in the primary slot is not valid!`,
serial-port names per OS, and using an external probe. Start there.

## Contents of the release bundle

```
flash.sh, flash.ps1              these scripts
README.md                        this file
openocd/xiao_nrf54lm20a.cfg      the board's OpenOCD config, unmodified,
                                 from zephyrproject-rtos/zephyr (Apache-2.0)
openocd/ZEPHYR_REVISION          the Zephyr commit it was taken from — the same
                                 revision the images in the release were built
                                 against
```
