# Bracket resin regression boundary

Frozen reproduction input for the continuous-fiber material adapter. The mesh is
`models/continuous_fiber/bracket1.stl`; `config.json` references it. See
`provenance.json` for its origin. The original application temporary G-code is no
longer available; this fixture is not asserted to copy that deleted output.

`--resin-model` exports effective configuration, G-code, timing and source-domain
diagnostics, including infill-only reinforcement. `check_fiber_resin.py`
independently checks final actual-width resin footprints against protected fiber
material and, with `--baseline`, compares fiber coordinate identity. G2/G3 from
the native XY/relative-IJ writer are expanded with a bounded circle chord error;
quantized endpoint radius mismatch is bounded and reported separately.
Unsupported coordinate modes and malformed arcs fail explicitly.

The adapter keeps Orca's pattern generation, Flow/variable width and GapFill
implementation. Within reinforced resin jobs it constrains actual-width main
paths before calling the original GapFill stage once, then constrains newly
created gap paths. Segment Boolean clipping reuses Orca's subject-bounding-box
vertex pruning to avoid repeatedly processing the entire layer mask; it does
not introduce another solver or change the mask inside the subject box.
Ordinary jobs remain on their original call path. Edge
contact is seeded only by real residual material after unioning fiber coverage;
shrinking individual fiber strands would create false internal channels.

The checker retains the existing 0.003 mm final-output intrusion-depth budget;
there is no area waiver, `WILL_FAIL`, blanket skip or output-tuned coverage pass
percentage. Legal whole native paths, narrow true residuals, butt-cap endpoints,
metadata and source order have separate core regression cases. Partial clipping
is conservative and is not asserted to retain every geometrically legal
subspan. Coverage metrics remain diagnostic (`not_evaluated`), not proof of
complete printable coverage, pattern semantics or performance.

The original frozen modes use `gap_fill_target=nowhere` and disable arc fitting.
The `concentric_resin_arcs` override is a focused integration regression: it
retains one reinforced layer with linear fiber and enables actual concentric
resin arcs. This isolates native resin fitting from expensive curved-fiber
certification. Its original failure was a safe five-point path becoming an
unsafe wide arc after simplification.

Resin-job roots with a hard fiber boundary retain a shared read-only material boundary.
Orca's original linear simplification and arc fitting still run. Results which cannot be certified within
that boundary keep the original path geometry; safe fits remain arcs. The
constraint follows collection copy/move lifetime, and failures restore backups
before rethrowing. Ordinary collections and unrelated GapFill stay unchanged.
The native disk-cache/flatten protocols are not extended by this adapter.

Tests of actual GapFill generation or arc output must explicitly enable those
settings and confirm their output exists; passing the original frozen modes
alone does not cover these branches. Keep ordinary-output, fiber-identity and production timing
comparisons separate from the intrusion result.
