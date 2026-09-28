# Bracket resin regression boundary

Frozen reproduction input, retained after reverting the experimental resin implementation.

`--resin-model` exports effective configuration, G-code, timing and diagnostics.
`check_fiber_resin.py` independently checks fiber intrusion and (with `--baseline`) fiber coordinate identity. Its exit status does **not** accept coverage completeness, pattern semantics or performance. Coverage metrics are diagnostic (`not_evaluated`); the previous output-tuned percentage thresholds were removed.

The restored algorithm is expected to fail the internal-channel regression. Keep that failure visible: no `WILL_FAIL`, blanket skip or tolerance change. The old infill-only diagnostics do not expose the source domain, so that mode currently fails the source-availability check. G2/G3 is unsupported and rejected explicitly.

Required future acceptance: no false internal channels, preserve printable true residuals, retain pattern/density semantics, account for actual width and derived GapFill, check final simplification/arc output, compare ordinary output and serial timing. These requirements must be established before implementing a new solver.

See `provenance.json` for model/config provenance. The original application temporary G-code is no longer available; this fixture is not asserted to be a byte-identical copy of that deleted output.
