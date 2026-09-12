# D04 — Delivery recipes

Implemented on branch `beta-1.0-fix` (phase 5 D04).

## Behavior

- Deliver panel **Save recipe…** stores preset, destination, creator codec, sidecar format,
  override indices, and hardware preference in `delivery/recipes` QSettings JSON.
- **Queue recipe** applies the saved settings and enqueues export jobs for the primary destination
  plus any extra destinations recorded in the recipe. The confirmation counts jobs that actually
  entered the queue.
- Recipes survive application restart via window settings.

## Tests

- `EditorControllerTest::deliveryRecipesPersistInSettings`

## Residual limitations

- Extra destinations are manifest-only today (no multi-destination editor in the save dialog).
- Recipes bind to preset indices; preset list changes may require re-saving.
