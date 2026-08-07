# Contributing

## Contract-first changes

The edit model, project schema, worker protocol, public headers, and dependency/license set are
protected contracts. Changes to them require an Architecture Decision Record and review by two
module owners.

Implementation work should remain inside one module whenever possible. Cross-module features are
split into a contract change followed by independent implementations. Every behavior change must
include automated tests.

## Local checks

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Format changed C++ files with the repository `.clang-format`. Do not run a broad formatting rewrite
in a feature change. Keep media fixtures small and document their provenance and license.

## Commit policy

- Keep commits focused and buildable.
- Never commit generated media caches, project WAL files, downloaded models, or build outputs.
- Do not update a pinned dependency, project schema, protocol version, or license without its ADR.
- Use `Signed-off-by` trailers; the project follows the Developer Certificate of Origin process.

