# Project snapshot codec

This module is the versioned wire boundary between the dependency-free edit
model and persistence/workers. It serializes a complete `edit::Project` into a
deterministic protobuf snapshot.

The current snapshot schema version is 2. Readers accept declared schema
versions 1 and 2 as long as `minimum_reader_version` is non-zero and not newer
than the codec itself.

Schema v2 adds canonical title payloads and sequence-owned transitions:

- title clips serialize validated `Title` payloads including UTF-8 text, font
  family, font size, colors, alignment, and bold/italic flags;
- transitions serialize their own ID, outgoing/incoming clip IDs, exact range,
  kind, and enabled flag.

Backward read behavior is explicit rather than heuristic:

- a declared v1 title clip is upgraded in memory to a deterministic default
  title payload derived from the clip name;
- declared v1 payloads may not smuggle v2-only title or transition data;
- future schema versions fail closed.

Exact rational values retain their numerator-like `value` and `timescale`; IDs
retain all 16 UUID bytes. Unknown edit effects are model data rather than
protobuf unknown fields, so their type, version, enabled/known flags, typed
parameters, and opaque bytes all round-trip.

The public API deliberately does not expose generated protobuf types. This
lets a later schema migrate internally without coupling the UI, project store,
or render workers to generated headers.
