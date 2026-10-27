# Contributing

Changes are welcome as pull requests against `main`. A few rules keep the
firmware deployable; [`claude.md`](claude.md) has the longer background.

## Build and test

* `west --sysbuild` is the only supported firmware build. Layer profiles with
  `EXTRA_CONF_FILE` / `EXTRA_DTC_OVERLAY_FILE`; there is one `prj.conf` and one
  `app.overlay`. See [docs/vscode-build.md](docs/vscode-build.md).
* Protocol, lifecycle, power or DSP changes come with a host regression test in
  [`tests/host/`](tests/host/), and the test should fail on the code before the
  change:

  ```bash
  cmake -S tests/host -B build-host && cmake --build build-host
  ctest --test-dir build-host --output-on-failure
  ```

* CI builds the `debug` and `lowpower` images and runs the host tests on every
  pull request. A green build is compile evidence; behaviour on hardware (OTA,
  rollback, audio, sleep current) still needs a board — see the release
  checklist in [docs/ota-over-ble.md](docs/ota-over-ble.md#production-release-and-recovery-checklist).

## Versions and releases

* Any change to the built image bumps `HIVEINSIDE_FW_VERSION_*` in
  `firmware-nrf54lm20a/src/hive_config.h` — patch by default, minor for a new
  feature or a changed BLE payload or value scale. Every version identifier
  (beacon, banner, artifact name, signed header) derives from it.
* Releases are built by `.github/workflows/release.yml` on a `v*` tag, signed
  with the project key. A release never replaces published assets, and a
  firmware version is published once; a fix is a new version.

## Repository settings (maintainer)

Recommended protection for `main`, set under **Settings → Rules → Rulesets**:
require a pull request, require the `host-tests`, `build (debug)` and
`build (lowpower)` checks, and block force pushes and deletion. Protect `v*`
tags the same way (no deletion or update), since release builds are triggered
by them. Record any deliberate administrator bypass in the pull request.

## Security

Report vulnerabilities privately — see [SECURITY.md](SECURITY.md).
