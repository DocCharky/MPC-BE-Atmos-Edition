# OpenJOC (vendored)

Source: https://github.com/chyinan/OpenJOC, commit 71c515ec04d66a4e2624376863791892d9448993, Apache-2.0 (see LICENSE).

Only the crates needed for decoding are kept (api, eac3, emdf, joc, oamd, render,
scene, sofa, qmf, bitio); tests, examples, fixtures and the HRTF assets are removed
(binaural rendering is not used: openjoc-api is built without default features).

Changes made for MPC-BE Atmos Edition:

- openjoc-api: a neutral reference screen (coded positions unchanged) is supplied
  to the OAMD payload decoder and to the speaker bridge, so that screen-anchored
  objects can be rendered (the public API had no way to set one).
- openjoc-scene (bridge_control): a base channel that no OAMD element is bound to
  is no longer rendered. It is only the downmix of the reconstructed objects, so
  every bed signal was rendered twice: a 5.1 render measured exactly 2.0x the
  reference core decode (FFmpeg), i.e. beds +6 dB against the height objects.
  After the change the 5.1 render matches the core at 1.00x. Base channels bound
  to an OAMD element are still rendered.
- openjoc-api: object output (`OpenJocConfig::object_output`): instead of a speaker
  render, each frame carries the reconstructed object signals (and bound base
  channels, and the LFE) with per-object state changes (position, gain, active)
  at their sample offsets (`OpenJocPcmFrame::object_tracks`). Used to hand the
  objects to Windows Spatial Sound, so the AVR renders them as with a native
  Atmos stream. Small supporting accessor: `BridgeControlAssembler::records`.
