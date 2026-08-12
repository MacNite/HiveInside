# HiveInside website

A small, dependency-free static site for HiveInside — a single page:

- **`index.html`** — the landing page: what the device is and why it exists
  (the ~20 Hz pre-swarm vibration a microphone cannot capture), the feature
  set, the hardware and wiring, what the BLE beacon actually contains
  (the FFT bands plus the 29-byte frame layout), a build-and-flash walkthrough,
  and where it sits in the ecosystem around
  [HiveHub](https://macnite.github.io/HiveHub/website/).
- **`assets/style.css`**, **`assets/theme.js`** — verbatim copies of the
  HiveHub website's shared stylesheet and dark-mode switcher
  (`website/assets/` in [MacNite/HiveHub](https://github.com/MacNite/HiveHub)),
  so both sites look and behave the same. Re-copy them to pick up changes;
  don't edit them here. Page-specific styles live in an inline `<style>` block
  in `index.html`, the same way HiveHub's `build.html` does it.

There is no build step — it is plain HTML/CSS/JS.

The dark-mode preference is stored under the **`hivehub-theme`** key on
purpose. Both sites are served from the `macnite.github.io` origin and so share
one `localStorage`; using the same key means a visitor who picks dark mode on
either site gets it on the other.

The page's content mirrors the repository documentation — the top-level
[`README.md`](../README.md),
[`firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md) (feature
set and BLE frame), [`docs/wiring.md`](../docs/wiring.md) (hardware) and
[`docs/low-power.md`](../docs/low-power.md) (battery). Update it when those
change; in particular the **status banner** at the bottom of the page says the
device is not yet hardware-validated in the field, and the battery numbers are
described as planning estimates rather than measurements. Both need revisiting
once real hardware figures exist.

## Preview locally

Open `website/index.html` in a browser, or serve the folder:

```bash
cd website
python3 -m http.server 8080
# then open http://localhost:8080/
```

## Publish on GitHub Pages

A workflow at [`.github/workflows/pages.yml`](../.github/workflows/pages.yml)
deploys this folder automatically. Enable it once:

1. Push these files to `main`.
2. In the repository, go to **Settings → Pages**.
3. Under **Build and deployment → Source**, choose **GitHub Actions**.

The workflow publishes the folder at **both** the Pages root and under
`/website/`, so the site answers at

- `https://macnite.github.io/HiveInside/` and
- `https://macnite.github.io/HiveInside/website/`.

That duplication is deliberate. Pages can only serve a branch from `/` or
`/docs`, never from `/website`, so the alternative "Deploy from a branch"
source would serve the repository as-is and put the site at
`…/HiveInside/website/` — a different URL from the one this workflow would
otherwise produce. Publishing both means external links (the HiveHub site
points here) survive either choice. The links **out** of this page are absolute
`https://` URLs, so they are unaffected either way.
