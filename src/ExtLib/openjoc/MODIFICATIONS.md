# OpenJOC (vendored)

Source: https://github.com/chyinan/OpenJOC, commit 71c515ec04d66a4e2624376863791892d9448993, Apache-2.0 (see LICENSE).

Only the crates needed for decoding are kept (api, eac3, emdf, joc, oamd, render,
scene, sofa, qmf, bitio); tests, examples, fixtures and the HRTF assets are removed
(binaural rendering is not used: openjoc-api is built without default features).

Changes made for MPC-BE Atmos Edition:

- openjoc-api: a default reference screen (whole front wall, listener level) is
  supplied to the OAMD payload decoder and to the speaker bridge, so that
  screen-anchored objects can be rendered (the public API had no way to set one).
