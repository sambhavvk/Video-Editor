# Project snapshot codec

This module is the versioned wire boundary between the dependency-free edit
model and persistence/workers. It serializes a complete `edit::Project` into a
deterministic protobuf snapshot.

Version 1 readers accept only schema version 1 and require a non-zero
`minimum_reader_version` no newer than themselves. Exact rational values retain
their numerator-like `value` and `timescale`; IDs retain all 16 UUID bytes.
Unknown edit effects are model data rather than protobuf unknown fields, so
their type, version, enabled/known flags, typed parameters, and opaque bytes all
round-trip.

The public API deliberately does not expose generated protobuf types. This
lets a later schema migrate internally without coupling the UI, project store,
or render workers to generated headers.
