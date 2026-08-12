<!-- SPDX-License-Identifier: MPL-2.0 -->

# EditorWindow

Header: `video_editor/desktop_ui/editor_window.hpp`

Namespace: `video_editor::desktop_ui`

## Class overview

`EditorWindow` is the top-level Qt Widgets shell for the Import, Edit, Audio & Captions, and Deliver
workspaces. It owns the program/source viewers, virtualized timeline, dock panels, menus, toolbars,
command palette, status presentation, and persistent workspace layouts. It exposes presentation
surfaces to `EditorController` and emits user intent without editing canonical state itself.

## Project structure and dependencies

The application creates one window and one controller. The window consumes the view values in
`ui_types.hpp`; its child panels and timeline are part of `video_editor_desktop_ui`. Build
requirements are `Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets`.

## Class hierarchy and role

The final class derives from `QMainWindow`, gaining menus, toolbars, dock areas, central widget,
status bar, native window behavior, events, and QObject parent ownership.

## Properties

| Property | Type | READ | WRITE | NOTIFY | Description |
| --- | --- | --- | --- | --- | --- |
| `workspace` | `Workspace` | `workspace` | `setWorkspace` | `workspaceChanged` | Active progressive-disclosure layout. |

## Public methods

### `explicit EditorWindow(QSettings* settings = nullptr, QWidget* parent = nullptr)`

Creates the complete shell. A supplied settings object is borrowed and must outlive the window;
otherwise the window owns an application-scoped `QSettings` instance.

### `~EditorWindow()`

Releases owned settings and normal QObject-owned children.

### Query and panel methods

`workspace()` returns the active workspace. `action(id)` returns a borrowed registered action or
null. `programViewer()`, `timeline()`, `mediaBin()`, `inspector()`, `effectsPanel()`, `audioMixer()`,
`captionsPanel()`, and `deliverPanel()` return borrowed, window-owned child pointers.

### `void setProjectDisplayName(const QString& displayName)` / `void setProjectDirty(bool dirty)`

Update title/status presentation without changing the project model.

### `void setMediaItems(const QVector<MediaItemView>& items)`

Replaces the media-bin view.

### `void setTimelineView(...)`

The four-argument overload replaces duration/timescale/tracks/clips for compatibility. The complete
overload additionally supplies marker and derived-gap views. Both delegate to `TimelineWidget`.

### `void showTransientMessage(const QString& message, int timeoutMs = 4000)`

Shows plain-language status feedback for the bounded duration.

## Public slots

### `void setWorkspace(Workspace workspace)`

Saves the outgoing layout, applies/restores the target layout, updates actions, and emits
`workspaceChanged`.

### `void setSourceMonitorVisible(bool visible)` / `void setPrecisionTrimVisible(bool visible)`

Reveal or hide progressively disclosed precision surfaces.

### `void restoreUiState()` / `void saveUiState()`

Load or persist geometry, workspace layouts, active workspace, and disclosure state through
`QSettings`.

## Signals

### `void workspaceChanged(Workspace workspace)`

Emitted after the active layout changes.

### Project lifecycle signals

`newProjectRequested()`, `openProjectRequested()`, `saveProjectRequested()`,
`saveProjectAsRequested()`, and `importMediaRequested()` request controller workflows.

### `void exportRequested(const QString& presetId)`

Requests export through the chosen presentation preset.

### Timeline/history signals

`undoRequested()`, `redoRequested()`, `splitClipRequested()`, and
`deleteSelectionRequested(bool ripple)` request canonical timeline commands. Professional
multi-selection/tool/header/marker/gap signals are emitted directly by `TimelineWidget`.

### Transport signals

`playbackRateRequested(double rate)` requests shuttle/transport state and
`seekRequested(qint64 position)` requests a timeline-timescale seek.

### Panel signals

`mediaActivated(const QString& mediaId)`, `effectAddRequested(const QString& effectId)`, and
`parameterEdited(const QString& parameterId, const QVariant& value)` pass panel intent to the
controller.

## Protected event handlers

### `void closeEvent(QCloseEvent* event) [override]`

Persists UI state and then delegates normal window-close handling. The controller event filter may
still ask the user about unsaved project edits.

## Ownership and lifecycle

Every panel, dock, toolbar, viewer, and timeline is QObject-parent-owned by the window. Getter
pointers are borrowed. A caller-supplied `QSettings` is not owned; the optional internal instance
uses `std::unique_ptr`.

## Thread safety and interactions

`EditorWindow` and all returned widgets are GUI-thread only. The controller connects lifecycle,
panel, transport, and `TimelineWidget` intent signals and is the only component that mutates the
edit model. QSettings and docking state are presentation data, not project state.

## Usage example

```cpp
desktop_ui::EditorWindow window;
app::EditorController controller{window};
window.resize(1440, 900);
window.show();
```

AI assistance has been used to create this output.
