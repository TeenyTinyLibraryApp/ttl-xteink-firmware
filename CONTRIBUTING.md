# Contributing

TTL firmware is an independent community fork of CrossPoint Reader. It is not
affiliated with Xteink. `v0.1.0` is the recovery baseline: reading, plus the
full-page TTL Sync screen. Do not add firmware OTA checks, update prompts, or
flashing UI to this release.

## License

- CrossPoint's original files stay under the MIT `LICENSE`, with their
  copyright notices intact.
- New TTL files are MIT, copyright 2026 Teeny Tiny Library contributors.
- The combined firmware is GPL-3.0-or-later because it links wolfSSL. See
  `NOTICE` and `licenses/README.md`.
- Do not commit secrets, tokens, personal credentials, passwords, or
  proprietary SDKs. `open-x4-sdk` is gitignored on purpose.

## Build the X4 firmware

The X4 (and X3, detected at runtime) profile is PlatformIO environment
`default`: ESP32-C3, `pioarduino` core 6.1.19.

```sh
git clone --recursive https://github.com/TeenyTinyLibraryApp/ttl-xteink-firmware
cd ttl-xteink-firmware
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade 'https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip'
pio pkg install -e default
pio run -e default
```

The binary is `.pio/build/default/firmware.bin`. The on-device version string
is `TTL 0.1.0` on every environment. The build log prints the git branch and
short SHA; they are not shown on the device.

More toolchain detail, including clang-format and `pio check`, is in
[docs/contributing/getting-started.md](docs/contributing/getting-started.md).

## Flash an X4

1. Put the device in USB boot mode the way you already flash CrossPoint
   (hold the boot button while connecting USB, or use the board's usual
   download mode).
2. From this repo, with the venv active:

```sh
pio run -e default --target upload
```

3. Reboot. The boot screen and Settings → About show `TTL 0.1.0`.
   The serial log line is `Starting TTL 0.1.0`.

There is no SD-card firmware updater and no "check for updates" row in this
build. USB flashing this binary is the recovery path.

## Try TTL Sync on a device

1. Boot to Home. Open **TTL Sync**. That screen replaces Home so the TLS
   session is not started from the home screen.
2. Confirm the server, or set another `http`/`https` origin. The default is
   `https://teenytinylibrary.app`.
3. Enter the portal email and password, then choose **Sync** and join Wi-Fi.
4. New EPUBs land in `/TTL/` on the SD card. A file that is already there with
   the same size is skipped.
5. Pull the card and read `/TTL/ttl-sync-log.txt`. It records start, login,
   skips, saves, and errors. It does not record the password or the token.

Sign out clears the password and the device token and keeps the email and
server. Changing the server, email, or password drops the saved token.

## Host tests

The TTL sync rules (manifest parser, login JSON, filename sanitizer, version
label, download skip rule) run on the host. They do not need the ESP32
toolchain or the `freeink-sdk` submodule.

```sh
cmake -S test -B test/build
cmake --build test/build --target TtlSyncTest
ctest --test-dir test/build -R 'TtlSync|ProtectedPaths' --output-on-failure
```

`TtlSyncTest` is compiled with `-Wall -Wextra -pedantic -Werror`.

## Commit style

Use [Conventional Commits](https://www.conventionalcommits.org/), for example
`feat: add TTL sync screen` or `fix: skip unchanged EPUB downloads`.
