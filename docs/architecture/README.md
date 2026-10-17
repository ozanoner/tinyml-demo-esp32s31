# Architecture diagrams

Two diagrams, both generated from this repository and both source-backed: every component carries
the file and line range that proves it. They are **generated files** — edit the JSON, never the
HTML.

| Diagram | Static render | Interactive | What it answers |
|---|---|---|---|
| `architecture` | [architecture.png](architecture.png) | [architecture.html](architecture.html) | What the system is made of: the application layer, `components/devkit_api` as the only board seam, the two devkit implementations, `esp_video`, and the third-party stack |
| `runtime` | [runtime.png](runtime.png) | [runtime.html](runtime.html) | Where data crosses tasks: feed → AFE → detect → command → countdown → capture → conversions → inference → screen |

GitHub does not render an `.html` blob, so the interactive files are linked through
htmlpreview — replace `github.com` with `htmlpreview.github.io/?` in the blob URL:

- https://htmlpreview.github.io/?https://github.com/ozanoner/tinyml-demo-esp32s31/blob/main/docs/architecture/architecture.html
- https://htmlpreview.github.io/?https://github.com/ozanoner/tinyml-demo-esp32s31/blob/main/docs/architecture/runtime.html

Downloading the file and opening it locally works too, and needs no network: it is one
self-contained HTML page.

## Regenerating

Requirements: Node.js ≥ 18, and a Chrome/Chromium on `PATH` (the `browser-check` gate drives a
real browser). The generator is the `archify` skill installed in this Hermes profile, not a
dependency of this repository:

```bash
A=~/.hermes/profiles/esp32-developer/skills/archify

node $A/bin/archify.mjs finalize architecture docs/architecture/architecture.json \
  docs/architecture/architecture.html --repo-root . --quality showcase
node $A/bin/archify.mjs finalize sequence docs/architecture/runtime.json \
  docs/architecture/runtime.html --repo-root . --quality showcase

python3 tools/render_architecture_png.py docs/architecture/architecture.html docs/architecture/architecture.png
python3 tools/render_architecture_png.py docs/architecture/runtime.html      docs/architecture/runtime.png
```

`finalize` runs all four gates — `validate`, `deliver`, `check`, `browser-check` — and stops at the
first failure. A non-zero exit is a failure, not a warning.

Two operational notes:

- **A second run on the same output needs `--out-dir`.** Once an artifact has a browser-check
  receipt, that output path owns it: give the next run its own directory, e.g.
  `--out-dir docs/architecture/review-2`. Its receipts are gitignored.
- **Chrome needs `--no-sandbox` on a host where unprivileged user namespaces are blocked** (Ubuntu
  23.10+ / AppArmor). archify passes no sandbox flag, so put a wrapper named `chromium` on `PATH`
  that execs the real binary with `--no-sandbox --disable-dev-shm-usage "$@"`.

If the diagram's content is edited after a change to the code, the `meta.repository.revision` in
each JSON should move to the commit the sources were re-read at — archify verifies every cited
path and line range against that revision, and fails if a range runs past the end of the file.

## What the diagrams deliberately leave out

- Per-model detail (wake word, command list, model versions) lives in the cards inside the HTML and
  in [../building.md](../building.md); the architecture diagram names the components, not their
  configuration.
- The runtime diagram starts at the microphone, so `App::start()`'s bring-up order — board, audio,
  display, camera, splash, screen, detector, voice — is not drawn. It is linear, and stated in
  `main/app.cpp`.
- Both diagrams describe one build at a time. Which devkit is compiled in is the preset's choice,
  not a runtime branch.
