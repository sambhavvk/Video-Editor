<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0008: Read-only bounded startup recovery catalog

- **Status:** accepted
- **Date:** 2026-08-08
- **Owners:** Core/Media, Desktop/Product, and Quality/Platform

## Context

The working SQLite database preserves completed edits after a crash, but startup must discover it
without modifying evidence, recursively opening arbitrary files, migrating damaged databases, or
blocking indefinitely on a large/corrupt directory. Clean shutdown and saved-checkpoint state are
independent signals.

## Decision

- Startup scans only direct `*.working.sqlite` children of the platform recovery directory and uses
  read-only SQLite connections. Scanning does not create directories, update heartbeats, change
  clean-close state, run migrations, or write journals.
- Each candidate receives a bounded quick integrity check plus current-schema, metadata singleton,
  and journal-head validation. Invalid or future-schema candidates stay visible in the catalog with
  bounded diagnostics but cannot be opened as valid recovery projects.
- Recovery is recommended when either `clean_close` is false or `head_revision` differs from
  `saved_revision`.
- Candidates sort deterministically: recommended before clean, then newest heartbeat, then path.
  A configurable maximum bounds inspection and exposes whether results were truncated.
- The desktop offers the first valid recommended candidate other than its active working database.
  It opens the latest committed `project.snapshot.v1`, verifies project identity, and marks the
  recovered project dirty so the user must choose a checkpoint destination.
- Declining recovery leaves the database untouched. Cleanup/retention is a separate future policy.

## Consequences

- Discovery does not destroy or normalize forensic recovery state.
- A clean but unsaved project and an unclean but already-saved project can both be offered for the
  appropriate reason.
- Bounded non-recursive scanning reduces startup risk and prevents unrelated SQLite files from being
  treated as projects.
- The current UI offers one candidate; a multi-project recovery browser and retention cleanup are
  follow-on product work.

## Verification

Project-store tests cover non-mutating clean inspection, both recommendation signals independently,
corrupt and wrong-schema diagnostics, ignored unrelated/nested files, deterministic sorting,
missing directories, and candidate limits. Application tests cover accepting the latest committed
snapshot; full process-termination fault injection remains a release gate.
