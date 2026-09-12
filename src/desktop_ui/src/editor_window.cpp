// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/editor_window.hpp"

#include "video_editor/desktop_ui/command_palette.hpp"
#include "video_editor/desktop_ui/export_dialog.hpp"
#include "video_editor/desktop_ui/keyboard_shortcuts_dialog.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_output_window.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/scope_widget.hpp"
#include "video_editor/desktop_ui/shortcut_bindings.hpp"
#include "video_editor/desktop_ui/timeline_cursors.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace video_editor::desktop_ui {
namespace {

constexpr int kUiStateVersion = 1;
constexpr auto kOrganization = "VideoEditor";
constexpr auto kApplication = "VideoEditor";
constexpr int kDefaultViewerHeight = 470;
constexpr int kDefaultTimelineHeight = 350;
constexpr int kCompactViewerHeight = 260;
constexpr int kCompactTimelineHeight = 360;
constexpr int kComfortableViewerHeight = 420;
constexpr int kComfortableTimelineHeight = 320;

bool isEditorChrome(const QWidget* widget, const QWidget* root) {
  for (const QWidget* current = widget; current != nullptr && current != root;
       current = current->parentWidget()) {
    if (qobject_cast<const QMenu*>(current) != nullptr ||
        qobject_cast<const QMenuBar*>(current) != nullptr ||
        qobject_cast<const QDialog*>(current) != nullptr ||
        qobject_cast<const QToolBar*>(current) != nullptr ||
        qobject_cast<const QStatusBar*>(current) != nullptr) {
      return true;
    }
  }
  return false;
}

QDockWidget* dockAncestor(QWidget* widget) {
  while (widget != nullptr) {
    if (auto* dock = qobject_cast<QDockWidget*>(widget)) {
      return dock;
    }
    if (qobject_cast<QMenu*>(widget) != nullptr || qobject_cast<QMenuBar*>(widget) != nullptr ||
        qobject_cast<QDialog*>(widget) != nullptr) {
      return nullptr;
    }
    widget = widget->parentWidget();
  }
  return nullptr;
}

void resizeShownDock(QMainWindow* window, QDockWidget* dock, int size,
                     Qt::Orientation orientation) {
  if (window == nullptr || dock == nullptr || dock->isHidden() || dock->isFloating()) {
    return;
  }
  window->resizeDocks({dock}, {size}, orientation);
}

QString strippedActionText(const QAction* action) {
  return action == nullptr ? QString{} : action->text().remove(u'&').trimmed();
}

QToolButton* makeActionButton(QAction* action, QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setDefaultAction(action);
  button->setAutoRaise(true);
  button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  if (action != nullptr) {
    const auto name = strippedActionText(action);
    if (!name.isEmpty()) {
      button->setAccessibleName(name);
    }
    if (!action->toolTip().isEmpty()) {
      button->setAccessibleDescription(action->toolTip());
    }
    const auto commandId = action->property("commandId").toString();
    if (!commandId.isEmpty()) {
      button->setObjectName(QStringLiteral("button.%1").arg(commandId));
    }
  }
  return button;
}

void labelToolButtonsFromActions(QWidget* root) {
  for (auto* button : root->findChildren<QToolButton*>()) {
    if (auto* action = button->defaultAction()) {
      if (button->accessibleName().trimmed().isEmpty()) {
        const auto name = strippedActionText(action);
        if (!name.isEmpty()) {
          button->setAccessibleName(name);
        }
      }
      if (button->accessibleDescription().trimmed().isEmpty() && !action->toolTip().isEmpty()) {
        button->setAccessibleDescription(action->toolTip());
      }
      if (button->objectName().isEmpty()) {
        const auto commandId = action->property("commandId").toString();
        if (!commandId.isEmpty()) {
          button->setObjectName(QStringLiteral("button.%1").arg(commandId));
        }
      }
      continue;
    }
    if (button->accessibleName().trimmed().isEmpty() &&
        qobject_cast<QToolBar*>(button->parentWidget()) != nullptr &&
        button->text().trimmed().isEmpty()) {
      button->setAccessibleName(QObject::tr("More toolbar actions"));
    }
  }
}

QDockWidget* makeDock(const QString& objectName, const QString& title, QWidget* panel,
                      QMainWindow* window) {
  auto* dock = new QDockWidget(title, window);
  dock->setObjectName(objectName);
  dock->setAccessibleName(title);
  dock->setAccessibleDescription(QObject::tr("Dockable %1 panel").arg(title));
  dock->setWidget(panel);
  dock->setAllowedAreas(Qt::AllDockWidgetAreas);
  dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable);
  dock->toggleViewAction()->setObjectName(QStringLiteral("action.toggle.%1").arg(objectName));
  dock->toggleViewAction()->setToolTip(QObject::tr("Show or hide %1").arg(title));
  return dock;
}

} // namespace

EditorWindow::EditorWindow(QSettings* settings, QWidget* parent) : QMainWindow(parent) {
  setObjectName(QStringLiteral("editorWindow"));
  setAccessibleName(tr("Video Editor"));
  setDockNestingEnabled(true);
  setAnimated(false);
  setMinimumSize(980, 680);
  resize(1440, 900);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setStyleSheet(darkStyleSheet());

  if (settings != nullptr) {
    settings_ = settings;
  } else {
    owned_settings_ = std::make_unique<QSettings>(QString::fromLatin1(kOrganization),
                                                  QString::fromLatin1(kApplication));
    settings_ = owned_settings_.get();
  }

  createActions();
  loadShortcutOverrides();
  createCentralArea();
  createPanels();
  connectScopesDock();
  createMenus();
  createToolBars();
  createStatusBar();
  connectControllerSurface();

  command_palette_ = new CommandPalette(this);
  command_palette_->setActions(actions_.values());
  connect(action(QStringLiteral("commandPalette")), &QAction::triggered, command_palette_,
          &CommandPalette::openPalette);

  initialized_ = true;
  restoreUiState();
  labelInteractiveChrome();
  setProjectDisplayName(project_display_name_);
}

EditorWindow::~EditorWindow() {
  if (initialized_) {
    saveUiState();
  }
}

QAction* EditorWindow::action(const QString& id) const {
  return actions_.value(id, nullptr);
}

QKeySequence EditorWindow::shortcutDefault(const QString& commandId) const {
  return shortcut_defaults_.value(commandId);
}

QString EditorWindow::applyShortcutBinding(const QString& commandId, const QKeySequence& shortcut,
                                           bool replaceConflicts) {
  const auto error = ShortcutBindings::applyBinding(settings_, actions_, shortcut_defaults_,
                                                    commandId, shortcut, replaceConflicts, this);
  if (error.isEmpty() && command_palette_ != nullptr) {
    command_palette_->setActions(actions_.values());
  }
  return error;
}

void EditorWindow::resetShortcutBinding(const QString& commandId) {
  ShortcutBindings::resetBinding(settings_, actions_, shortcut_defaults_, commandId);
  if (command_palette_ != nullptr) {
    command_palette_->setActions(actions_.values());
  }
}

void EditorWindow::setProjectDisplayName(const QString& displayName) {
  project_display_name_ = displayName.trimmed().isEmpty() ? tr("Untitled Project") : displayName;
  setWindowTitle(QStringLiteral("%1%2 — %3")
                     .arg(project_dirty_ ? QStringLiteral("● ") : QString{}, project_display_name_,
                          tr("Video Editor")));
  setAccessibleDescription(tr("Editing project %1").arg(project_display_name_));
}

void EditorWindow::setProjectDirty(bool dirty) {
  if (project_dirty_ == dirty) {
    return;
  }
  project_dirty_ = dirty;
  setProjectDisplayName(project_display_name_);
}

void EditorWindow::setMediaItems(const QVector<MediaItemView>& items) {
  media_bin_->setItems(items);
}

void EditorWindow::setMediaBins(const QVector<MediaBinView>& bins) {
  media_bin_->setBins(bins);
}

void EditorWindow::setTimelineView(qint64 duration, qint64 timeScale,
                                   QVector<TimelineTrackView> tracks,
                                   QVector<TimelineClipView> clips) {
  timeline_->setTimeline(duration, timeScale, std::move(tracks), std::move(clips));
}

void EditorWindow::setTimelineView(qint64 duration, qint64 timeScale,
                                   QVector<TimelineTrackView> tracks,
                                   QVector<TimelineClipView> clips,
                                   QVector<TimelineMarkerView> markers,
                                   QVector<TimelineGapView> gaps) {
  timeline_->setTimeline(duration, timeScale, std::move(tracks), std::move(clips),
                         std::move(markers), std::move(gaps));
}

void EditorWindow::setSequenceTabs(const QVector<SequenceTabView>& tabs) {
  if (sequence_tab_bar_ == nullptr) {
    return;
  }
  QSignalBlocker blocker(sequence_tab_bar_);
  while (sequence_tab_bar_->count() > 0) {
    sequence_tab_bar_->removeTab(0);
  }
  int active_index = 0;
  for (int index = 0; index < tabs.size(); ++index) {
    const auto& tab = tabs.at(index);
    sequence_tab_bar_->addTab(tab.displayName);
    sequence_tab_bar_->setTabToolTip(index, tab.displayName);
    sequence_tab_bar_->setTabData(index, tab.id);
    const QString accessible = tab.displayName.isEmpty() ? tr("Sequence tab") : tab.displayName;
    sequence_tab_bar_->setTabText(index, tab.displayName);
    if (auto* tab_button = sequence_tab_bar_->tabButton(index, QTabBar::LeftSide)) {
      tab_button->setAccessibleName(accessible);
    }
    if (tabs.at(index).active) {
      active_index = index;
    }
  }
  if (sequence_tab_bar_->count() > 0) {
    sequence_tab_bar_->setCurrentIndex(active_index);
  }
}

void EditorWindow::showTransientMessage(const QString& message, int timeoutMs) {
  statusBar()->showMessage(message, timeoutMs);
}

void EditorWindow::setCommandContext(CommandContext context) {
  command_context_ = context;
  if (command_palette_ != nullptr && command_palette_->isVisible()) {
    command_palette_->setActions(actions_.values());
  }
}

QString EditorWindow::commandUnavailableReason(const QAction* action) const {
  if (action == nullptr) {
    return {};
  }
  const QString id = action->property("commandId").toString();
  const auto needsSelection = [this](const QString& message) {
    return command_context_.hasClipSelection ? QString{} : message;
  };
  if (id == QStringLiteral("splitClip") || id == QStringLiteral("copyClips") ||
      id == QStringLiteral("cutClips") || id == QStringLiteral("duplicateClips") ||
      id == QStringLiteral("unlinkClips") || id == QStringLiteral("disableClip") ||
      id == QStringLiteral("enableClip") || id == QStringLiteral("deleteSelection") ||
      id == QStringLiteral("rippleDelete") || id == QStringLiteral("liftSelection") ||
      id == QStringLiteral("extractSelection") || id == QStringLiteral("nestSelectedClips") ||
      id == QStringLiteral("zoomToSelection") || id == QStringLiteral("replaceClipMedia")) {
    return needsSelection(tr("Select clips before using this command"));
  }
  if (id == QStringLiteral("freezeFrame")) {
    return command_context_.hasClipsAtPlayhead
               ? QString{}
               : tr("Place the playhead on a video clip to freeze a frame");
  }
  if (id == QStringLiteral("pasteClipsInsert") || id == QStringLiteral("pasteClipsOverwrite")) {
    return command_context_.hasClipboard ? QString{} : tr("Copy clips before pasting");
  }
  if (id == QStringLiteral("pasteClipAttributes")) {
    if (!command_context_.hasAttributeClipboard) {
      return tr("Copy a clip before pasting attributes");
    }
    return needsSelection(tr("Select clips before pasting attributes"));
  }
  if (id == QStringLiteral("sourceMarkIn") || id == QStringLiteral("sourceMarkOut") ||
      id == QStringLiteral("sourceRippleInsert") || id == QStringLiteral("sourceOverwriteInsert")) {
    return command_context_.hasSource ? QString{} : tr("Load a source clip first");
  }
  if (id == QStringLiteral("trimHeadToPlayhead") || id == QStringLiteral("trimTailToPlayhead") ||
      id == QStringLiteral("overwriteTrimHeadToPlayhead") ||
      id == QStringLiteral("overwriteTrimTailToPlayhead")) {
    return command_context_.hasClipsAtPlayhead ? QString{}
                                               : tr("No clips under the playhead to trim");
  }
  if (id == QStringLiteral("sequenceSettings") || id == QStringLiteral("duplicateSequence") ||
      id == QStringLiteral("gotoTimecode") || id == QStringLiteral("playAround") ||
      id == QStringLiteral("defaultTransition") || id == QStringLiteral("selectAtPlayhead") ||
      id == QStringLiteral("seekPreviousEdit") || id == QStringLiteral("seekNextEdit") ||
      id == QStringLiteral("matchFrame")) {
    return command_context_.hasSequence ? QString{} : tr("Open a sequence first");
  }
  return {};
}

void EditorWindow::setAudioSyncStatus(const QString& text) {
  if (av_sync_label_ != nullptr) {
    av_sync_label_->setText(text);
  }
}

void EditorWindow::setSequenceFormatStatus(const QString& text) {
  if (sequence_format_label_ != nullptr) {
    sequence_format_label_->setText(text);
  }
}

void EditorWindow::setJobActivitySummary(const QString& summary) {
  if (job_activity_label_ != nullptr) {
    job_activity_label_->setText(summary);
  }
}

void EditorWindow::showExportDialog(const QString& presetId) {
  if (deliver_panel_ != nullptr && !deliver_panel_->exportReady()) {
    showTransientMessage(deliver_panel_->exportUnavailableReason());
    return;
  }
  QString effective_preset = presetId;
  if (effective_preset.isEmpty()) {
    effective_preset = deliver_panel_->selectedPresetId();
  }
  export_dialog_->setSelectedPreset(effective_preset);
  export_dialog_->setDestinationPath(deliver_panel_->destinationPath());
  if (export_dialog_->exec() != QDialog::Accepted) {
    return;
  }
  const QString destination = export_dialog_->destinationPath();
  deliver_panel_->setDestinationPath(destination);
  emit exportConfirmed(destination, export_dialog_->selectedPresetId());
}

void EditorWindow::focusInspector() {
  if (inspector_dock_ == nullptr) {
    return;
  }
  inspector_dock_->show();
  inspector_dock_->raise();
}

void EditorWindow::setWorkspace(Workspace workspace) {
  if (initialized_) {
    session_layouts_.insert(workspace_, restorableLayoutState());
  }
  if (workspace_ == workspace && initialized_) {
    updateWorkspaceActions();
    updateWorkspaceLabel();
    return;
  }

  workspace_ = workspace;
  const auto saved = session_layouts_.value(workspace_);
  if (!saved.isEmpty() && restoreState(saved, kUiStateVersion)) {
    // The saved state includes panel visibility and placement.
  } else {
    applyDefaultLayout(workspace_);
  }
  panel_maximized_ = false;
  layout_before_maximize_.clear();
  compact_tier_ = CompactTier::Normal;
  updateWorkspaceActions();
  updateWorkspaceLabel();
  labelInteractiveChrome();
  applyCompactLayoutForCurrentSize();
  emit workspaceChanged(workspace_);
}

void EditorWindow::setSourceMonitorVisible(bool visible) {
  source_container_->setVisible(visible);
  if (auto* toggle = action(QStringLiteral("sourceMonitor"))) {
    const QSignalBlocker blocker(toggle);
    toggle->setChecked(visible);
  }
  updateFocusedMonitorLabel();
}

void EditorWindow::setPrecisionTrimVisible(bool visible) {
  precision_trim_->setVisible(visible);
  if (auto* toggle = action(QStringLiteral("precisionTrim"))) {
    const QSignalBlocker blocker(toggle);
    toggle->setChecked(visible);
  }
}

void EditorWindow::restoreUiState() {
  if (settings_ == nullptr) {
    applyDefaultLayout(workspace_);
    return;
  }

  const auto geometry = settings_->value(QStringLiteral("ui/mainWindowGeometry")).toByteArray();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  }

  const std::array workspaces{Workspace::Import, Workspace::Edit, Workspace::AudioCaptions,
                              Workspace::Deliver};
  for (const auto candidate : workspaces) {
    const auto state = settings_->value(settingsKeyForWorkspace(candidate)).toByteArray();
    if (!state.isEmpty()) {
      session_layouts_.insert(candidate, state);
    }
  }

  const auto rawWorkspace =
      settings_->value(QStringLiteral("ui/lastWorkspace"), static_cast<int>(Workspace::Edit))
          .toInt();
  if (rawWorkspace >= static_cast<int>(Workspace::Import) &&
      rawWorkspace <= static_cast<int>(Workspace::Deliver)) {
    workspace_ = static_cast<Workspace>(rawWorkspace);
  } else {
    workspace_ = Workspace::Edit;
  }

  const auto state = session_layouts_.value(workspace_);
  if (state.isEmpty() || !restoreState(state, kUiStateVersion)) {
    applyDefaultLayout(workspace_);
  }
  setSourceMonitorVisible(
      settings_->value(QStringLiteral("ui/sourceMonitorVisible"), false).toBool());
  setPrecisionTrimVisible(
      settings_->value(QStringLiteral("ui/precisionTrimVisible"), false).toBool());
  updateWorkspaceActions();
  updateWorkspaceLabel();
  restoreProgramOutputScreen();
  compact_tier_ = CompactTier::Normal;
  compact_height_ = false;
  applyCompactLayoutForCurrentSize();
}

void EditorWindow::saveUiState() {
  if (settings_ == nullptr) {
    return;
  }
  session_layouts_.insert(workspace_, restorableLayoutState());
  settings_->setValue(QStringLiteral("ui/mainWindowGeometry"), saveGeometry());
  settings_->setValue(QStringLiteral("ui/lastWorkspace"), static_cast<int>(workspace_));
  settings_->setValue(QStringLiteral("ui/sourceMonitorVisible"), !source_container_->isHidden());
  settings_->setValue(QStringLiteral("ui/precisionTrimVisible"), !precision_trim_->isHidden());
  for (auto it = session_layouts_.cbegin(); it != session_layouts_.cend(); ++it) {
    settings_->setValue(settingsKeyForWorkspace(it.key()), it.value());
  }
  settings_->sync();
}

void EditorWindow::closeEvent(QCloseEvent* event) {
  if (program_fullscreen_active_) {
    exitProgramFullscreen();
  }
  if (program_output_window_ != nullptr) {
    program_output_window_->close();
    program_output_window_.reset();
  }
  saveUiState();
  QMainWindow::closeEvent(event);
}

void EditorWindow::createCentralArea() {
  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("editorCentralArea"));
  central->setAccessibleName(tr("Editor workspace"));
  auto* centralLayout = new QVBoxLayout(central);
  centralLayout->setContentsMargins(0, 0, 0, 0);
  centralLayout->setSpacing(0);

  viewer_timeline_splitter_ = new QSplitter(Qt::Vertical, central);
  auto* vertical = viewer_timeline_splitter_;
  vertical->setObjectName(QStringLiteral("viewerTimelineSplitter"));
  vertical->setAccessibleName(tr("Viewer and timeline divider"));
  vertical->setChildrenCollapsible(false);

  auto* viewerArea = new QWidget(vertical);
  auto* viewerLayout = new QVBoxLayout(viewerArea);
  viewerLayout->setContentsMargins(0, 0, 0, 0);
  viewerLayout->setSpacing(0);

  auto* viewerSplitter = new QSplitter(Qt::Horizontal, viewerArea);
  viewerSplitter->setObjectName(QStringLiteral("monitorSplitter"));
  viewerSplitter->setAccessibleName(tr("Source and program monitors"));
  viewerSplitter->setChildrenCollapsible(false);

  source_container_ = new QFrame(viewerSplitter);
  source_container_->setObjectName(QStringLiteral("sourceMonitorContainer"));
  source_container_->setAccessibleName(tr("Source monitor"));
  auto* sourceLayout = new QVBoxLayout(source_container_);
  sourceLayout->setContentsMargins(0, 0, 0, 0);
  source_viewer_ = new ProgramViewer(source_container_);
  source_viewer_->setObjectName(QStringLiteral("sourceViewer"));
  source_viewer_->setAccessibleName(tr("Source viewer"));
  source_viewer_->setAccessibleDescription(
      tr("Preview of the loaded source clip. Shows source time. Keyboard transport and mark "
         "commands target this monitor while it has focus."));
  source_viewer_->setTitle(tr("Source"));
  source_viewer_->setTimecodeCaption(tr("Source time"));
  source_viewer_->setSourceEditKeysEnabled(true);
  sourceLayout->addWidget(source_viewer_);

  program_viewer_ = new ProgramViewer(viewerSplitter);
  program_viewer_->setObjectName(QStringLiteral("programViewer"));
  program_viewer_->setAccessibleDescription(
      tr("Preview of the current sequence. Shows sequence time. Press Space to play or pause. "
         "Keyboard transport targets this monitor while it has focus. Media files can be dropped "
         "here."));
  program_viewer_->setTitle(tr("Program"));
  program_viewer_->setTimecodeCaption(tr("Sequence time"));
  program_viewer_->setNativePresentationEnabled(true);
  program_viewer_splitter_ = viewerSplitter;
  viewerSplitter->addWidget(source_container_);
  viewerSplitter->addWidget(program_viewer_);
  viewerSplitter->setStretchFactor(0, 1);
  viewerSplitter->setStretchFactor(1, 1);
  source_container_->hide();
  viewerLayout->addWidget(viewerSplitter, 1);

  auto* transport = new QFrame(viewerArea);
  transport->setObjectName(QStringLiteral("transportBar"));
  transport->setAccessibleName(tr("Playback controls"));
  transport->setFrameShape(QFrame::StyledPanel);
  auto* transportLayout = new QHBoxLayout(transport);
  transportLayout->setContentsMargins(8, 3, 8, 3);
  transportLayout->setSpacing(3);
  monitor_focus_label_ = new QLabel(tr("Commands target: Program"), transport);
  monitor_focus_label_->setObjectName(QStringLiteral("monitorFocusLabel"));
  monitor_focus_label_->setAccessibleName(tr("Focused monitor"));
  monitor_focus_label_->setAccessibleDescription(tr(
      "Shows whether keyboard transport and mark commands target the source or program monitor"));
  monitor_focus_label_->setFocusPolicy(Qt::NoFocus);
  monitor_focus_label_->setMinimumWidth(
      monitor_focus_label_->fontMetrics().horizontalAdvance(tr("Commands target: Program")) + 8);
  transportLayout->addWidget(monitor_focus_label_);
  transportLayout->addSpacing(8);
  for (const auto* id : {"sourceRippleInsert", "sourceOverwriteInsert"}) {
    auto* button = makeActionButton(action(QString::fromLatin1(id)), transport);
    button->setObjectName(QStringLiteral("transport.%1").arg(QString::fromLatin1(id)));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFocusPolicy(Qt::NoFocus);
    transportLayout->addWidget(button);
  }
  transportLayout->addSpacing(12);
  transportLayout->addStretch();
  for (const auto* id : {"previousFrame", "reverse", "stop", "playPause", "forward", "nextFrame"}) {
    auto* button = makeActionButton(action(QString::fromLatin1(id)), transport);
    button->setFocusPolicy(Qt::NoFocus);
    transportLayout->addWidget(button);
  }
  transportLayout->addSpacing(8);
  transport_label_ = new QLabel(tr("Stopped"), transport);
  transport_label_->setObjectName(QStringLiteral("transportStatus"));
  transport_label_->setAccessibleName(tr("Playback state"));
  transport_label_->setMinimumWidth(80);
  transportLayout->addWidget(transport_label_);
  transportLayout->addStretch();
  viewerLayout->addWidget(transport);

  auto* timelineArea = new QWidget(vertical);
  auto* timelineLayout = new QVBoxLayout(timelineArea);
  timelineLayout->setContentsMargins(0, 0, 0, 0);
  timelineLayout->setSpacing(0);

  sequence_tab_bar_ = new QTabBar(timelineArea);
  sequence_tab_bar_->setObjectName(QStringLiteral("sequenceTabBar"));
  sequence_tab_bar_->setAccessibleName(tr("Sequence tabs"));
  sequence_tab_bar_->setAccessibleDescription(tr("Switch between project sequences"));
  sequence_tab_bar_->setExpanding(false);
  sequence_tab_bar_->setDrawBase(true);
  timelineLayout->addWidget(sequence_tab_bar_);

  precision_trim_ = new QFrame(timelineArea);
  precision_trim_->setObjectName(QStringLiteral("precisionTrimPanel"));
  precision_trim_->setAccessibleName(tr("Precision trim controls"));
  precision_trim_->setFrameShape(QFrame::StyledPanel);
  auto* trimLayout = new QHBoxLayout(precision_trim_);
  trimLayout->setContentsMargins(10, 5, 10, 5);
  auto* trimLabel = new QLabel(tr("Precision Trim"), precision_trim_);
  QFont trimFont = trimLabel->font();
  trimFont.setWeight(QFont::DemiBold);
  trimLabel->setFont(trimFont);
  trimLayout->addWidget(trimLabel);
  trimLayout->addSpacing(12);
  for (const auto* id :
       {"tool.select", "tool.rippleTrim", "tool.overwriteTrim", "tool.roll", "tool.slip",
        "tool.slide", "tool.razor", "tool.pen", "tool.hand", "tool.zoom"}) {
    auto* mode = makeActionButton(action(QString::fromLatin1(id)), precision_trim_);
    mode->setObjectName(QStringLiteral("precision.%1").arg(QString::fromLatin1(id)));
    mode->setToolButtonStyle(Qt::ToolButtonIconOnly);
    trimLayout->addWidget(mode);
  }
  trimLayout->addSpacing(8);
  const auto addNudgeButton = [trimLayout, this](const char* suffix, const QString& label,
                                                 const QString& accessibleName) {
    auto* button = new QToolButton(precision_trim_);
    button->setObjectName(QStringLiteral("precision.nudge.%1").arg(QString::fromLatin1(suffix)));
    button->setText(label);
    button->setAccessibleName(accessibleName);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    trimLayout->addWidget(button);
  };
  addNudgeButton("minus10", tr("−10"), tr("Nudge selected clip earlier by ten frames"));
  addNudgeButton("minus1", tr("−1"), tr("Nudge selected clip earlier by one frame"));
  addNudgeButton("plus1", tr("+1"), tr("Nudge selected clip later by one frame"));
  addNudgeButton("plus10", tr("+10"), tr("Nudge selected clip later by ten frames"));
  trimLayout->addSpacing(8);
  auto* split = makeActionButton(action(QStringLiteral("splitClip")), precision_trim_);
  split->setObjectName(QStringLiteral("precision.splitClip"));
  split->setToolButtonStyle(Qt::ToolButtonTextOnly);
  trimLayout->addWidget(split);
  trimLayout->addStretch();
  auto* hint = new QLabel(tr("Alt+←/→ nudge · Shift=10 · Ctrl=ripple · V/C/P/H/Z/R/W/N/Y/U tools"),
                          precision_trim_);
  hint->setProperty("muted", true);
  trimLayout->addWidget(hint);
  precision_trim_->hide();
  timelineLayout->addWidget(precision_trim_);

  timeline_ = new TimelineWidget(timelineArea);
  timeline_->setTimeline(5 * 60 * 48'000LL, 48'000,
                         {
                             {QStringLiteral("video-2"), tr("V2 · Overlay"), TrackKind::Video},
                             {QStringLiteral("video-1"), tr("V1 · Primary"), TrackKind::Video},
                             {QStringLiteral("audio-1"), tr("A1 · Dialogue"), TrackKind::Audio},
                             {QStringLiteral("audio-2"), tr("A2 · Music"), TrackKind::Audio},
                             {QStringLiteral("audio-3"), tr("A3 · Effects"), TrackKind::Audio},
                             {QStringLiteral("audio-4"), tr("A4 · Ambience"), TrackKind::Audio},
                         },
                         {});
  marker_list_ = new MarkerListWidget(timelineArea);
  marker_list_->setMaximumHeight(110);
  timelineLayout->addWidget(marker_list_);
  timelineLayout->addWidget(timeline_, 1);

  vertical->addWidget(viewerArea);
  vertical->addWidget(timelineArea);
  vertical->setStretchFactor(0, 5);
  vertical->setStretchFactor(1, 4);
  vertical->setSizes({kDefaultViewerHeight, kDefaultTimelineHeight});
  centralLayout->addWidget(vertical, 1);
  setCentralWidget(central);
}

void EditorWindow::createPanels() {
  media_bin_ = new MediaBinWidget(this);
  inspector_ = new InspectorWidget(this);
  effects_panel_ = new EffectsPanelWidget(this);
  audio_mixer_ = new AudioMixerWidget(this);
  captions_panel_ = new CaptionsPanelWidget(this);
  deliver_panel_ = new DeliverPanelWidget(this);
  deliver_panel_->setExportEnabled(false);
  scopes_widget_ = new ScopeWidget(this);
  cache_browser_ = new CacheBrowserDialog(this);
  export_dialog_ = new ExportDialog(this);

  media_dock_ = makeDock(QStringLiteral("mediaDock"), tr("Media Bin"), media_bin_, this);
  inspector_dock_ = makeDock(QStringLiteral("inspectorDock"), tr("Inspector"), inspector_, this);
  effects_dock_ = makeDock(QStringLiteral("effectsDock"), tr("Effects"), effects_panel_, this);
  mixer_dock_ = makeDock(QStringLiteral("mixerDock"), tr("Audio Mixer"), audio_mixer_, this);
  captions_dock_ =
      makeDock(QStringLiteral("captionsDock"), tr("Captions & Transcript"), captions_panel_, this);
  deliver_dock_ = makeDock(QStringLiteral("deliverDock"), tr("Deliver"), deliver_panel_, this);
  scopes_dock_ = makeDock(QStringLiteral("scopesDock"), tr("Scopes"), scopes_widget_, this);
  scopes_dock_->setAccessibleName(tr("Scopes"));

  addDockWidget(Qt::LeftDockWidgetArea, media_dock_);
  addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
  addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, mixer_dock_);
  addDockWidget(Qt::RightDockWidgetArea, captions_dock_);
  addDockWidget(Qt::RightDockWidgetArea, deliver_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, scopes_dock_);
  tabifyDockWidget(inspector_dock_, effects_dock_);
  tabifyDockWidget(effects_dock_, captions_dock_);
  tabifyDockWidget(captions_dock_, deliver_dock_);
}

void EditorWindow::connectScopesDock() {
  if (scopes_dock_ == nullptr) {
    return;
  }
  if (auto* scopes = action(QStringLiteral("scopes"))) {
    connect(scopes, &QAction::toggled, scopes_dock_, &QWidget::setVisible);
  }
  connect(scopes_dock_, &QDockWidget::visibilityChanged, this, [this](const bool visible) {
    if (auto* toggle = action(QStringLiteral("scopes"))) {
      const QSignalBlocker blocker(toggle);
      toggle->setChecked(visible);
    }
  });
}

void EditorWindow::createActions() {
  const auto create = [this](const QString& id, const QString& text, const QString& toolTip,
                             const QKeySequence& shortcut = {}) {
    auto* created = new QAction(text, this);
    created->setToolTip(toolTip);
    created->setStatusTip(toolTip);
    shortcut_defaults_.insert(id, shortcut);
    if (!shortcut.isEmpty()) {
      created->setShortcut(shortcut);
      created->setShortcutContext(Qt::WindowShortcut);
    }
    addAction(id, created);
    return created;
  };

  auto* newProject = create(QStringLiteral("newProject"), tr("New Project"),
                            tr("Create a new project"), QKeySequence::New);
  newProject->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
  auto* openProject = create(QStringLiteral("openProject"), tr("Open Project…"),
                             tr("Open an existing project"), QKeySequence::Open);
  openProject->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  auto* saveProject = create(QStringLiteral("saveProject"), tr("Save Project"),
                             tr("Save the current project"), QKeySequence::Save);
  saveProject->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
  create(QStringLiteral("saveProjectAs"), tr("Save Project As…"), tr("Save a copy of this project"),
         QKeySequence::SaveAs);
  auto* import = create(QStringLiteral("importMedia"), tr("Import Media…"),
                        tr("Import video, audio, or images"), QKeySequence{tr("Ctrl+I")});
  import->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  create(QStringLiteral("importOtio"), tr("Import OpenTimelineIO…"),
         tr("Import an OpenTimelineIO JSON timeline into this project"));
  create(QStringLiteral("exportOtio"), tr("Export OpenTimelineIO…"),
         tr("Export the current sequence as OpenTimelineIO JSON"));
  create(QStringLiteral("manageMediaCache"), tr("Manage Media Cache…"),
         tr("Review cache use and set the media cache budget"));
  auto* exportAction = create(QStringLiteral("export"), tr("Export Video…"),
                              tr("Export the current sequence"), QKeySequence{tr("Ctrl+E")});
  exportAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
  create(QStringLiteral("quit"), tr("Quit"), tr("Close the application"), QKeySequence::Quit);

  create(QStringLiteral("undo"), tr("Undo"), tr("Undo the last edit"), QKeySequence::Undo);
  create(QStringLiteral("redo"), tr("Redo"), tr("Redo the last undone edit"), QKeySequence::Redo);
  create(QStringLiteral("splitClip"), tr("Split Clip"), tr("Split selected clips at the playhead"),
         QKeySequence{tr("Ctrl+B")});
  create(QStringLiteral("selectAtPlayhead"), tr("Select at Playhead"),
         tr("Select clips under the playhead on targeted unlocked tracks"),
         QKeySequence{Qt::Key_D});
  create(QStringLiteral("seekPreviousEdit"), tr("Previous Edit"),
         tr("Seek to the previous edit point"), QKeySequence{Qt::Key_Up});
  create(QStringLiteral("seekNextEdit"), tr("Next Edit"), tr("Seek to the next edit point"),
         QKeySequence{Qt::Key_Down});
  create(QStringLiteral("matchFrame"), tr("Match Frame"),
         tr("Open source media at the frame under the playhead"), QKeySequence{Qt::Key_F});
  create(QStringLiteral("trimHeadToPlayhead"), tr("Ripple Trim Head to Playhead"),
         tr("Trim clip heads to the playhead and ripple"), QKeySequence{Qt::Key_Q});
  create(QStringLiteral("trimTailToPlayhead"), tr("Ripple Trim Tail to Playhead"),
         tr("Trim clip tails to the playhead and ripple"), QKeySequence{Qt::Key_E});
  create(QStringLiteral("overwriteTrimHeadToPlayhead"), tr("Overwrite Trim Head to Playhead"),
         tr("Trim clip heads to the playhead without rippling"), QKeySequence{tr("Shift+Q")});
  create(QStringLiteral("overwriteTrimTailToPlayhead"), tr("Overwrite Trim Tail to Playhead"),
         tr("Trim clip tails to the playhead without rippling"), QKeySequence{tr("Shift+E")});
  create(QStringLiteral("copyClips"), tr("Copy"), tr("Copy the selected clips"),
         QKeySequence::Copy);
  create(QStringLiteral("cutClips"), tr("Cut"), tr("Cut the selected clips"), QKeySequence::Cut);
  create(QStringLiteral("pasteClipsInsert"), tr("Paste Insert"),
         tr("Paste copied clips at the playhead and ripple"), QKeySequence::Paste);
  create(QStringLiteral("pasteClipsOverwrite"), tr("Paste Overwrite"),
         tr("Paste copied clips at the playhead and overwrite"), QKeySequence{tr("Ctrl+Alt+V")});
  create(QStringLiteral("duplicateClips"), tr("Duplicate"),
         tr("Duplicate the selection at the playhead"), QKeySequence{tr("Ctrl+Shift+D")});
  create(QStringLiteral("defaultTransition"), tr("Apply Default Transition"),
         tr("Add a cross dissolve at the playhead or selected cuts"), QKeySequence{tr("Ctrl+D")});
  create(QStringLiteral("pasteClipAttributes"), tr("Paste Attributes"),
         tr("Paste copied clip attributes onto the selection"), QKeySequence{tr("Ctrl+Alt+A")});
  create(QStringLiteral("replaceClipMedia"), tr("Replace Clip Media"),
         tr("Replace the selected clip media from the loaded source"));
  auto* toggleLinkedSelection =
      create(QStringLiteral("toggleLinkedSelection"), tr("Linked Selection"),
             tr("Expand selections to linked audio/video clips"), QKeySequence{tr("Ctrl+L")});
  toggleLinkedSelection->setCheckable(true);
  toggleLinkedSelection->setChecked(true);
  create(QStringLiteral("unlinkClips"), tr("Unlink Clips"),
         tr("Clear linked audio/video groups on the selection"), QKeySequence{tr("Ctrl+Shift+U")});
  create(QStringLiteral("disableClip"), tr("Disable Clip"),
         tr("Disable the selected clip for playback and export"));
  create(QStringLiteral("enableClip"), tr("Enable Clip"),
         tr("Re-enable the selected clip for playback and export"));
  create(QStringLiteral("grabFrame"), tr("Grab Frame"),
         tr("Save the current program monitor frame as an image"),
         QKeySequence{tr("Ctrl+Shift+E")});
  create(QStringLiteral("freezeFrame"), tr("Freeze Frame"),
         tr("Hold the frame under the playhead for the rest of the clip"),
         QKeySequence{tr("Ctrl+Shift+H")});
  create(QStringLiteral("sequenceSettings"), tr("Sequence Settings…"),
         tr("Edit the active sequence name and format"));
  create(QStringLiteral("duplicateSequence"), tr("Duplicate Sequence"),
         tr("Duplicate the active sequence with new clip IDs"));
  create(QStringLiteral("gotoTimecode"), tr("Go to Timecode"), tr("Seek to a typed timecode"),
         QKeySequence{tr("Ctrl+G")});
  create(QStringLiteral("toggleLoopPlayback"), tr("Toggle Loop Playback"),
         tr("Loop playback between program In and Out"), QKeySequence{tr("Ctrl+Shift+L")});
  create(QStringLiteral("playAround"), tr("Play Around"),
         tr("Play a short range around the playhead"), QKeySequence{tr("Shift+K")});
  create(QStringLiteral("clearProgramIn"), tr("Clear Program In"),
         tr("Clear the program monitor in point"), QKeySequence{tr("Alt+I")});
  create(QStringLiteral("clearProgramOut"), tr("Clear Program Out"),
         tr("Clear the program monitor out point"), QKeySequence{tr("Alt+O")});
  create(QStringLiteral("clearProgramMarks"), tr("Clear Program In and Out"),
         tr("Clear both program monitor marks"), QKeySequence{tr("Alt+X")});
  create(QStringLiteral("deleteSelection"), tr("Delete / Lift"),
         tr("Remove the selection and leave a gap"), QKeySequence{Qt::Key_Delete});
  create(QStringLiteral("rippleDelete"), tr("Ripple Delete / Extract"),
         tr("Remove the selection and close the gap"), QKeySequence{tr("Shift+Delete")});
  create(QStringLiteral("liftSelection"), tr("Lift"), tr("Remove the selection and leave a gap"),
         QKeySequence{Qt::Key_Semicolon});
  create(QStringLiteral("extractSelection"), tr("Extract"),
         tr("Remove the selection and close the gap"), QKeySequence{Qt::Key_Apostrophe});
  create(QStringLiteral("nestSelectedClips"), tr("Nest Selected Clips"),
         tr("Create a nested sequence from the selected clips"));

  auto* previous = create(QStringLiteral("previousFrame"), tr("Previous Frame"),
                          tr("Move one frame backward"), QKeySequence{Qt::Key_Comma});
  previous->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
  auto* reverse =
      create(QStringLiteral("reverse"), tr("Play Reverse"),
             tr("Play backward; press repeatedly to increase speed"), QKeySequence{Qt::Key_J});
  reverse->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
  auto* stop =
      create(QStringLiteral("stop"), tr("Stop"), tr("Stop playback"), QKeySequence{Qt::Key_K});
  stop->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
  auto* playPause = create(QStringLiteral("playPause"), tr("Play / Pause"),
                           tr("Play or pause the sequence"), QKeySequence{Qt::Key_Space});
  playPause->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  auto* forward =
      create(QStringLiteral("forward"), tr("Play Forward"),
             tr("Play forward; press repeatedly to increase speed"), QKeySequence{Qt::Key_L});
  forward->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
  auto* next = create(QStringLiteral("nextFrame"), tr("Next Frame"), tr("Move one frame forward"),
                      QKeySequence{Qt::Key_Period});
  next->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));

  create(QStringLiteral("zoomInTimeline"), tr("Zoom In Timeline"),
         tr("Increase timeline magnification"), QKeySequence::ZoomIn);
  create(QStringLiteral("zoomOutTimeline"), tr("Zoom Out Timeline"),
         tr("Decrease timeline magnification"), QKeySequence::ZoomOut);
  create(QStringLiteral("zoomFitTimeline"), tr("Fit Timeline"),
         tr("Fit the sequence in the timeline"), QKeySequence{tr("Shift+Z")});
  create(QStringLiteral("zoomToSelection"), tr("Zoom to Selection"),
         tr("Magnify the timeline to the selected clips"));
  auto* toggleSnap = create(QStringLiteral("toggleSnap"), tr("Snap"),
                            tr("Snap edits to edit points and markers"), QKeySequence{Qt::Key_S});
  toggleSnap->setCheckable(true);
  toggleSnap->setChecked(true);
  auto* toggleFollowPlayhead =
      create(QStringLiteral("toggleFollowPlayhead"), tr("Follow Playhead"),
             tr("Keep the playhead visible while scrubbing"), QKeySequence{tr("Ctrl+Shift+F")});
  toggleFollowPlayhead->setCheckable(true);
  toggleFollowPlayhead->setChecked(true);

  auto* timelineToolGroup = new QActionGroup(this);
  timelineToolGroup->setExclusive(true);
  const auto addTimelineTool = [create, timelineToolGroup](const QString& id, const QString& text,
                                                           const QString& description,
                                                           const QKeySequence& shortcut) {
    auto* tool = create(id, text, description, shortcut);
    tool->setCheckable(true);
    timelineToolGroup->addAction(tool);
    return tool;
  };
  auto* selectTool =
      addTimelineTool(QStringLiteral("tool.select"), tr("Select"),
                      tr("Select, move, and edge-trim clips"), QKeySequence{tr("V")});
  selectTool->setChecked(true);
  addTimelineTool(QStringLiteral("tool.rippleTrim"), tr("Ripple Trim"),
                  tr("Trim and ripple following material"), QKeySequence{tr("R")});
  addTimelineTool(QStringLiteral("tool.overwriteTrim"), tr("Overwrite Trim"),
                  tr("Trim without moving following material"), QKeySequence{tr("W")});
  addTimelineTool(QStringLiteral("tool.roll"), tr("Roll"),
                  tr("Roll an edit between adjacent clips"), QKeySequence{tr("N")});
  addTimelineTool(QStringLiteral("tool.slip"), tr("Slip"),
                  tr("Change source timing without moving the clip"), QKeySequence{tr("Y")});
  addTimelineTool(QStringLiteral("tool.slide"), tr("Slide"),
                  tr("Move a clip and trim its neighbours"), QKeySequence{tr("U")});
  auto* trackSelectForward = addTimelineTool(
      QStringLiteral("tool.trackSelectForward"), tr("Track Select Forward"),
      tr("Select this clip and all later clips on the track"), QKeySequence{Qt::Key_A});
  addTimelineTool(QStringLiteral("tool.razor"), tr("Razor"),
                  tr("Split clips by clicking; Shift splits every unlocked track"),
                  QKeySequence{Qt::Key_C});
  addTimelineTool(QStringLiteral("tool.pen"), tr("Pen"),
                  tr("Edit clip volume or opacity envelopes; Alt-click removes a keyframe"),
                  QKeySequence{Qt::Key_P});
  addTimelineTool(QStringLiteral("tool.hand"), tr("Hand"),
                  tr("Pan the timeline; middle-mouse pans in any tool"), QKeySequence{Qt::Key_H});
  addTimelineTool(QStringLiteral("tool.zoom"), tr("Zoom"),
                  tr("Click to zoom in, Alt-click to zoom out, drag to frame a range"),
                  QKeySequence{Qt::Key_Z});

  auto* sourceMonitor =
      create(QStringLiteral("sourceMonitor"), tr("Source Monitor"),
             tr("Show a second monitor for source media"), QKeySequence{tr("Shift+2")});
  sourceMonitor->setCheckable(true);
  create(QStringLiteral("sourceMarkIn"), tr("Mark In"),
         tr("Set the in point at the focused monitor playhead"), QKeySequence{Qt::Key_I});
  create(QStringLiteral("sourceMarkOut"), tr("Mark Out"),
         tr("Set the out point at the focused monitor playhead"), QKeySequence{Qt::Key_O});
  create(QStringLiteral("sourceRippleInsert"), tr("Insert from Source"),
         tr("Ripple-insert the marked source range at the program playhead"),
         QKeySequence{Qt::SHIFT | Qt::Key_Comma});
  create(QStringLiteral("sourceOverwriteInsert"), tr("Overwrite from Source"),
         tr("Overwrite the marked source range at the program playhead"),
         QKeySequence{Qt::SHIFT | Qt::Key_Period});
  auto* precisionTrim = create(QStringLiteral("precisionTrim"), tr("Precision Trim Controls"),
                               tr("Show precision trim controls"), QKeySequence{tr("T")});
  precisionTrim->setCheckable(true);
  auto* scopes =
      create(QStringLiteral("scopes"), tr("Scopes"),
             tr("Show Rec.709 waveform, vectorscope, and histogram"), QKeySequence{tr("Shift+3")});
  scopes->setCheckable(true);
  auto* safeGuides = create(QStringLiteral("safeGuides"), tr("Safe Guides"),
                            tr("Show title and action safe guides"));
  safeGuides->setCheckable(true);
  auto* viewerClipInfo = create(QStringLiteral("viewerClipInfo"), tr("Clip Info Overlay"),
                                tr("Show the active clip name on the program monitor"));
  viewerClipInfo->setCheckable(true);
  viewerClipInfo->setChecked(true);
  auto* viewerSourceTc = create(QStringLiteral("viewerSourceTimecode"), tr("Source Timecode"),
                                tr("Show source timecode in the program monitor overlay"));
  viewerSourceTc->setCheckable(true);
  auto* programFullscreen =
      create(QStringLiteral("programFullscreen"), tr("Program Monitor Fullscreen"),
             tr("Show the program monitor fullscreen on this display"), QKeySequence{Qt::Key_F11});
  programFullscreen->setCheckable(true);
  create(QStringLiteral("maximizeFocusedPanel"), tr("Maximize Focused Panel"),
         tr("Expand the focused dock or central workspace; press again to restore"),
         QKeySequence{QStringLiteral("`")});
  create(QStringLiteral("resetWorkspaceLayout"), tr("Reset Workspace Layout"),
         tr("Restore the default dock layout for the current workspace"),
         QKeySequence{QStringLiteral("Ctrl+Alt+R")});
  create(QStringLiteral("commandPalette"), tr("Command Palette…"), tr("Search and run any command"),
         QKeySequence{tr("Ctrl+Shift+P")});

  program_fullscreen_escape_ = new QShortcut(QKeySequence{Qt::Key_Escape}, this);
  program_fullscreen_escape_->setObjectName(QStringLiteral("programFullscreenEscape"));
  program_fullscreen_escape_->setContext(Qt::ApplicationShortcut);
  program_fullscreen_escape_->setEnabled(false);
  connect(program_fullscreen_escape_, &QShortcut::activated, this,
          &EditorWindow::exitProgramFullscreen);

  auto* workspaceGroup = new QActionGroup(this);
  workspaceGroup->setExclusive(true);
  const std::array workspaceDefinitions{
      std::pair{Workspace::Import, QKeySequence{tr("Ctrl+1")}},
      std::pair{Workspace::Edit, QKeySequence{tr("Ctrl+2")}},
      std::pair{Workspace::AudioCaptions, QKeySequence{tr("Ctrl+3")}},
      std::pair{Workspace::Deliver, QKeySequence{tr("Ctrl+4")}},
  };
  for (const auto& [workspace, shortcut] : workspaceDefinitions) {
    const auto id = QStringLiteral("workspace.%1").arg(static_cast<int>(workspace));
    auto* workspaceAction =
        create(id, workspaceDisplayName(workspace),
               tr("Switch to the %1 workspace").arg(workspaceDisplayName(workspace)), shortcut);
    workspaceAction->setCheckable(true);
    workspaceAction->setData(static_cast<int>(workspace));
    workspaceGroup->addAction(workspaceAction);
    workspace_actions_.insert(workspace, workspaceAction);
    connect(workspaceAction, &QAction::triggered, this,
            [this, workspace] { setWorkspace(workspace); });
  }

  connect(newProject, &QAction::triggered, this, &EditorWindow::newProjectRequested);
  connect(openProject, &QAction::triggered, this, &EditorWindow::openProjectRequested);
  connect(saveProject, &QAction::triggered, this, &EditorWindow::saveProjectRequested);
  connect(action(QStringLiteral("saveProjectAs")), &QAction::triggered, this,
          &EditorWindow::saveProjectAsRequested);
  connect(import, &QAction::triggered, this, &EditorWindow::importMediaRequested);
  connect(action(QStringLiteral("importOtio")), &QAction::triggered, this,
          &EditorWindow::importOtioRequested);
  connect(action(QStringLiteral("exportOtio")), &QAction::triggered, this,
          &EditorWindow::exportOtioRequested);
  connect(exportAction, &QAction::triggered, this, [this] { showExportDialog(); });
  connect(action(QStringLiteral("quit")), &QAction::triggered, this, &QWidget::close);
  connect(action(QStringLiteral("undo")), &QAction::triggered, this, &EditorWindow::undoRequested);
  connect(action(QStringLiteral("redo")), &QAction::triggered, this, &EditorWindow::redoRequested);
  connect(action(QStringLiteral("splitClip")), &QAction::triggered, this,
          &EditorWindow::splitClipRequested);
  connect(action(QStringLiteral("selectAtPlayhead")), &QAction::triggered, this,
          &EditorWindow::selectAtPlayheadRequested);
  connect(action(QStringLiteral("seekPreviousEdit")), &QAction::triggered, this,
          &EditorWindow::seekPreviousEditRequested);
  connect(action(QStringLiteral("seekNextEdit")), &QAction::triggered, this,
          &EditorWindow::seekNextEditRequested);
  connect(action(QStringLiteral("matchFrame")), &QAction::triggered, this,
          &EditorWindow::matchFrameRequested);
  connect(action(QStringLiteral("trimHeadToPlayhead")), &QAction::triggered, this,
          &EditorWindow::trimHeadToPlayheadRequested);
  connect(action(QStringLiteral("trimTailToPlayhead")), &QAction::triggered, this,
          &EditorWindow::trimTailToPlayheadRequested);
  connect(action(QStringLiteral("overwriteTrimHeadToPlayhead")), &QAction::triggered, this,
          &EditorWindow::overwriteTrimHeadToPlayheadRequested);
  connect(action(QStringLiteral("overwriteTrimTailToPlayhead")), &QAction::triggered, this,
          &EditorWindow::overwriteTrimTailToPlayheadRequested);
  connect(action(QStringLiteral("copyClips")), &QAction::triggered, this,
          &EditorWindow::copyClipsRequested);
  connect(action(QStringLiteral("cutClips")), &QAction::triggered, this,
          &EditorWindow::cutClipsRequested);
  connect(action(QStringLiteral("pasteClipsInsert")), &QAction::triggered, this,
          &EditorWindow::pasteClipsInsertRequested);
  connect(action(QStringLiteral("pasteClipsOverwrite")), &QAction::triggered, this,
          &EditorWindow::pasteClipsOverwriteRequested);
  connect(action(QStringLiteral("duplicateClips")), &QAction::triggered, this,
          &EditorWindow::duplicateClipsRequested);
  connect(action(QStringLiteral("defaultTransition")), &QAction::triggered, this,
          &EditorWindow::defaultTransitionRequested);
  connect(action(QStringLiteral("pasteClipAttributes")), &QAction::triggered, this,
          &EditorWindow::pasteClipAttributesRequested);
  connect(action(QStringLiteral("replaceClipMedia")), &QAction::triggered, this,
          [this] { emit replaceClipMediaRequested(QString{}); });
  connect(action(QStringLiteral("toggleLinkedSelection")), &QAction::triggered, this,
          &EditorWindow::toggleLinkedSelectionRequested);
  connect(action(QStringLiteral("unlinkClips")), &QAction::triggered, this,
          &EditorWindow::unlinkClipsRequested);
  connect(action(QStringLiteral("disableClip")), &QAction::triggered, this,
          [this] { emit setClipEnabledRequested(false); });
  connect(action(QStringLiteral("enableClip")), &QAction::triggered, this,
          [this] { emit setClipEnabledRequested(true); });
  connect(action(QStringLiteral("toggleSnap")), &QAction::triggered, this,
          &EditorWindow::toggleSnapRequested);
  connect(action(QStringLiteral("toggleFollowPlayhead")), &QAction::triggered, this,
          &EditorWindow::toggleFollowPlayheadRequested);
  connect(action(QStringLiteral("grabFrame")), &QAction::triggered, this,
          &EditorWindow::grabFrameRequested);
  connect(action(QStringLiteral("freezeFrame")), &QAction::triggered, this,
          &EditorWindow::freezeFrameRequested);
  connect(action(QStringLiteral("sequenceSettings")), &QAction::triggered, this,
          &EditorWindow::sequenceSettingsRequested);
  connect(action(QStringLiteral("duplicateSequence")), &QAction::triggered, this,
          &EditorWindow::duplicateSequenceRequested);
  connect(action(QStringLiteral("gotoTimecode")), &QAction::triggered, this,
          &EditorWindow::gotoTimecodeRequested);
  connect(action(QStringLiteral("toggleLoopPlayback")), &QAction::triggered, this,
          &EditorWindow::toggleLoopPlaybackRequested);
  connect(action(QStringLiteral("playAround")), &QAction::triggered, this,
          &EditorWindow::playAroundRequested);
  connect(action(QStringLiteral("clearProgramIn")), &QAction::triggered, this,
          &EditorWindow::clearProgramInRequested);
  connect(action(QStringLiteral("clearProgramOut")), &QAction::triggered, this,
          &EditorWindow::clearProgramOutRequested);
  connect(action(QStringLiteral("clearProgramMarks")), &QAction::triggered, this,
          &EditorWindow::clearProgramMarksRequested);
  connect(trackSelectForward, &QAction::triggered, this, [this] {
    if (timeline_->toolMode() == TimelineWidget::ToolMode::Select) {
      emit selectForwardOnTargetedTrackRequested();
      return;
    }
    timeline_->setToolMode(TimelineWidget::ToolMode::TrackSelectForward);
  });
  connect(action(QStringLiteral("deleteSelection")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(false); });
  connect(action(QStringLiteral("rippleDelete")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(true); });
  connect(action(QStringLiteral("liftSelection")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(false); });
  connect(action(QStringLiteral("extractSelection")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(true); });

  connect(reverse, &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourceStepShuttleRequested(-1);
      return;
    }
    stepShuttle(-1);
  });
  connect(stop, &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourcePlaybackRateRequested(0.0);
      return;
    }
    setShuttleRate(0.0);
  });
  connect(playPause, &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourcePlaybackRateRequested(shuttle_rate_ == 0.0 ? 1.0 : 0.0);
      return;
    }
    setShuttleRate(shuttle_rate_ == 0.0 ? 1.0 : 0.0);
  });
  connect(forward, &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourceStepShuttleRequested(1);
      return;
    }
    stepShuttle(1);
  });
  connect(sourceMonitor, &QAction::toggled, this, &EditorWindow::setSourceMonitorVisible);
  connect(precisionTrim, &QAction::toggled, this, &EditorWindow::setPrecisionTrimVisible);
  connect(action(QStringLiteral("sourceMarkIn")), &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      markSourceIn();
      return;
    }
    emit programMarkInRequested();
  });
  connect(action(QStringLiteral("sourceMarkOut")), &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      markSourceOut();
      return;
    }
    emit programMarkOutRequested();
  });
  connect(action(QStringLiteral("sourceRippleInsert")), &QAction::triggered, this,
          &EditorWindow::rippleInsertFromSource);
  connect(action(QStringLiteral("sourceOverwriteInsert")), &QAction::triggered, this,
          &EditorWindow::overwriteInsertFromSource);
  connect(programFullscreen, &QAction::triggered, this, &EditorWindow::toggleProgramFullscreen);
  connect(action(QStringLiteral("maximizeFocusedPanel")), &QAction::triggered, this,
          &EditorWindow::maximizeFocusedPanel);
  connect(action(QStringLiteral("resetWorkspaceLayout")), &QAction::triggered, this,
          &EditorWindow::resetWorkspaceLayout);
  applyTimelineToolIcons();
}

void EditorWindow::loadShortcutOverrides() {
  ShortcutBindings::loadOverrides(settings_, actions_);
}

void EditorWindow::showKeyboardShortcutsPreferences() {
  KeyboardShortcutsDialog dialog(settings_, actions_, shortcut_defaults_, this);
  connect(&dialog, &KeyboardShortcutsDialog::shortcutsChanged, this, [this] {
    if (command_palette_ != nullptr) {
      command_palette_->setActions(actions_.values());
    }
  });
  dialog.exec();
}

void EditorWindow::refreshRecentProjectsMenu(const QStringList& paths,
                                             const bool reopenLastOnStartup) {
  if (recent_projects_menu_ == nullptr) {
    return;
  }
  recent_projects_menu_->clear();
  if (paths.isEmpty()) {
    auto* empty = recent_projects_menu_->addAction(tr("(No recent projects)"));
    empty->setEnabled(false);
  } else {
    for (const QString& path : paths) {
      const QString label = QFileInfo(path).fileName();
      auto* recent = recent_projects_menu_->addAction(label);
      recent->setToolTip(path);
      recent->setStatusTip(path);
      connect(recent, &QAction::triggered, this,
              [this, path] { emit openRecentProjectRequested(path); });
    }
  }
  if (reopen_last_on_startup_action_ != nullptr) {
    reopen_last_on_startup_action_->setChecked(reopenLastOnStartup);
  }
}

void EditorWindow::createMenus() {
  menuBar()->setAccessibleName(tr("Application menu"));
  auto* file = menuBar()->addMenu(tr("&File"));
  file->setObjectName(QStringLiteral("fileMenu"));
  file->setAccessibleName(tr("File"));
  file->addAction(action(QStringLiteral("newProject")));
  file->addAction(action(QStringLiteral("openProject")));
  recent_projects_menu_ = file->addMenu(tr("Open Recent"));
  recent_projects_menu_->setObjectName(QStringLiteral("recentProjectsMenu"));
  recent_projects_menu_->setAccessibleName(tr("Open Recent"));
  file->addSeparator();
  file->addAction(action(QStringLiteral("saveProject")));
  file->addAction(action(QStringLiteral("saveProjectAs")));
  file->addSeparator();
  file->addAction(action(QStringLiteral("importMedia")));
  file->addAction(action(QStringLiteral("importOtio")));
  file->addAction(action(QStringLiteral("exportOtio")));
  file->addSeparator();
  file->addAction(action(QStringLiteral("manageMediaCache")));
  file->addAction(action(QStringLiteral("export")));
  file->addAction(action(QStringLiteral("grabFrame")));
  file->addSeparator();
  file->addAction(action(QStringLiteral("quit")));

  auto* edit = menuBar()->addMenu(tr("&Edit"));
  edit->setObjectName(QStringLiteral("editMenu"));
  edit->setAccessibleName(tr("Edit"));
  edit->addAction(action(QStringLiteral("undo")));
  edit->addAction(action(QStringLiteral("redo")));
  edit->addSeparator();
  edit->addAction(action(QStringLiteral("splitClip")));
  edit->addAction(action(QStringLiteral("deleteSelection")));
  edit->addAction(action(QStringLiteral("rippleDelete")));
  edit->addAction(action(QStringLiteral("liftSelection")));
  edit->addAction(action(QStringLiteral("extractSelection")));
  edit->addAction(action(QStringLiteral("nestSelectedClips")));
  edit->addSeparator();
  edit->addAction(action(QStringLiteral("pasteClipAttributes")));
  edit->addAction(action(QStringLiteral("replaceClipMedia")));
  edit->addAction(action(QStringLiteral("toggleLinkedSelection")));
  edit->addAction(action(QStringLiteral("unlinkClips")));
  edit->addAction(action(QStringLiteral("disableClip")));
  edit->addAction(action(QStringLiteral("enableClip")));
  edit->addAction(action(QStringLiteral("freezeFrame")));
  edit->addSeparator();
  edit->addAction(action(QStringLiteral("commandPalette")));

  auto* timelineMenu = menuBar()->addMenu(tr("&Timeline"));
  timelineMenu->setObjectName(QStringLiteral("timelineMenu"));
  timelineMenu->setAccessibleName(tr("Timeline"));
  timelineMenu->addAction(action(QStringLiteral("previousFrame")));
  timelineMenu->addAction(action(QStringLiteral("reverse")));
  timelineMenu->addAction(action(QStringLiteral("stop")));
  timelineMenu->addAction(action(QStringLiteral("playPause")));
  timelineMenu->addAction(action(QStringLiteral("forward")));
  timelineMenu->addAction(action(QStringLiteral("nextFrame")));
  timelineMenu->addSeparator();
  timelineMenu->addAction(action(QStringLiteral("sourceMarkIn")));
  timelineMenu->addAction(action(QStringLiteral("sourceMarkOut")));
  timelineMenu->addAction(action(QStringLiteral("sourceRippleInsert")));
  timelineMenu->addAction(action(QStringLiteral("sourceOverwriteInsert")));
  timelineMenu->addSeparator();
  timelineMenu->addAction(action(QStringLiteral("defaultTransition")));
  timelineMenu->addAction(action(QStringLiteral("zoomInTimeline")));
  timelineMenu->addAction(action(QStringLiteral("zoomOutTimeline")));
  timelineMenu->addAction(action(QStringLiteral("zoomFitTimeline")));
  timelineMenu->addAction(action(QStringLiteral("zoomToSelection")));
  timelineMenu->addAction(action(QStringLiteral("toggleSnap")));
  timelineMenu->addAction(action(QStringLiteral("toggleFollowPlayhead")));
  timelineMenu->addSeparator();
  timelineMenu->addAction(action(QStringLiteral("duplicateSequence")));
  timelineMenu->addAction(action(QStringLiteral("sequenceSettings")));
  timelineMenu->addSeparator();
  for (const auto* id : {"tool.select", "tool.rippleTrim", "tool.overwriteTrim", "tool.roll",
                         "tool.slip", "tool.slide", "tool.razor", "tool.pen", "tool.hand",
                         "tool.zoom", "tool.trackSelectForward"}) {
    timelineMenu->addAction(action(QString::fromLatin1(id)));
  }

  auto* view = menuBar()->addMenu(tr("&View"));
  view->setObjectName(QStringLiteral("viewMenu"));
  view->setAccessibleName(tr("View"));
  auto* workspaces = view->addMenu(tr("Workspaces"));
  workspaces->setObjectName(QStringLiteral("workspacesMenu"));
  workspaces->setAccessibleName(tr("Workspaces"));
  for (const auto workspace :
       {Workspace::Import, Workspace::Edit, Workspace::AudioCaptions, Workspace::Deliver}) {
    workspaces->addAction(workspace_actions_.value(workspace));
  }
  view->addSeparator();
  view->addAction(action(QStringLiteral("sourceMonitor")));
  view->addAction(action(QStringLiteral("precisionTrim")));
  view->addAction(action(QStringLiteral("scopes")));
  view->addAction(action(QStringLiteral("safeGuides")));
  view->addAction(action(QStringLiteral("viewerClipInfo")));
  view->addAction(action(QStringLiteral("viewerSourceTimecode")));
  view->addAction(action(QStringLiteral("programFullscreen")));
  view->addAction(action(QStringLiteral("maximizeFocusedPanel")));
  view->addAction(action(QStringLiteral("resetWorkspaceLayout")));
  program_output_menu_ = view->addMenu(tr("Program monitor on display…"));
  program_output_menu_->setObjectName(QStringLiteral("programOutputMenu"));
  program_output_menu_->setAccessibleName(tr("Program monitor on display"));
  connect(program_output_menu_, &QMenu::aboutToShow, this, &EditorWindow::rebuildProgramOutputMenu);
  auto* panels = view->addMenu(tr("Panels"));
  panels->setObjectName(QStringLiteral("panelsMenu"));
  panels->setAccessibleName(tr("Panels"));
  for (auto* dock : {media_dock_, inspector_dock_, effects_dock_, mixer_dock_, captions_dock_,
                     deliver_dock_, scopes_dock_}) {
    panels->addAction(dock->toggleViewAction());
  }

  auto* preferences = menuBar()->addMenu(tr("&Preferences"));
  preferences->setObjectName(QStringLiteral("preferencesMenu"));
  preferences->setAccessibleName(tr("Preferences"));
  auto* keyboardPreferences = preferences->addAction(tr("Keyboard Shortcuts…"));
  keyboardPreferences->setObjectName(QStringLiteral("action.keyboardShortcutsPreferences"));
  connect(keyboardPreferences, &QAction::triggered, this,
          &EditorWindow::showKeyboardShortcutsPreferences);
  reopen_last_on_startup_action_ = preferences->addAction(tr("Reopen Last Project on Startup"));
  reopen_last_on_startup_action_->setObjectName(QStringLiteral("action.reopenLastOnStartup"));
  reopen_last_on_startup_action_->setCheckable(true);
  reopen_last_on_startup_action_->setChecked(true);
  connect(reopen_last_on_startup_action_, &QAction::toggled, this,
          &EditorWindow::reopenLastOnStartupToggled);

  auto* help = menuBar()->addMenu(tr("&Help"));
  help->setObjectName(QStringLiteral("helpMenu"));
  help->setAccessibleName(tr("Help"));
  auto* gettingStarted = help->addAction(tr("Getting Started"));
  gettingStarted->setObjectName(QStringLiteral("action.gettingStarted"));
  connect(gettingStarted, &QAction::triggered, this, [this] {
    setWorkspace(Workspace::Import);
    showTransientMessage(
        tr("Start by importing media. The first clip can define your sequence settings."), 7000);
  });
  auto* shortcuts = help->addAction(tr("Keyboard Shortcuts"));
  shortcuts->setObjectName(QStringLiteral("action.keyboardShortcuts"));
  connect(shortcuts, &QAction::triggered, this, [this] {
    QMessageBox::information(this, tr("Keyboard Shortcuts"),
                             ShortcutBindings::formatShortcutHelp(actions_));
  });
}

void EditorWindow::createToolBars() {
  auto* project = addToolBar(tr("Project"));
  project->setObjectName(QStringLiteral("projectToolBar"));
  project->setAccessibleName(tr("Project toolbar"));
  project->setMovable(true);
  project->addAction(action(QStringLiteral("newProject")));
  project->addAction(action(QStringLiteral("openProject")));
  project->addAction(action(QStringLiteral("saveProject")));
  project->addSeparator();
  project->addAction(action(QStringLiteral("importMedia")));

  workspace_toolbar_ = addToolBar(tr("Workspaces"));
  workspace_toolbar_->setObjectName(QStringLiteral("workspaceToolBar"));
  workspace_toolbar_->setAccessibleName(tr("Workspace toolbar"));
  workspace_toolbar_->setMovable(true);
  for (const auto workspace :
       {Workspace::Import, Workspace::Edit, Workspace::AudioCaptions, Workspace::Deliver}) {
    workspace_toolbar_->addAction(workspace_actions_.value(workspace));
  }

  auto* timelineTools = addToolBar(tr("Timeline Tools"));
  timelineTools->setObjectName(QStringLiteral("timelineToolBar"));
  timelineTools->setAccessibleName(tr("Timeline tools"));
  timelineTools->setMovable(true);
  timelineTools->setIconSize(QSize(22, 22));
  timelineTools->setToolButtonStyle(Qt::ToolButtonIconOnly);
  timelineTools->addAction(action(QStringLiteral("splitClip")));
  timelineTools->addSeparator();
  for (const auto* id : {"tool.select", "tool.rippleTrim", "tool.overwriteTrim", "tool.roll",
                         "tool.slip", "tool.slide", "tool.razor", "tool.pen", "tool.hand",
                         "tool.zoom", "tool.trackSelectForward"}) {
    timelineTools->addAction(action(QString::fromLatin1(id)));
  }
  timelineTools->addSeparator();
  timelineTools->addAction(action(QStringLiteral("zoomOutTimeline")));
  timelineTools->addAction(action(QStringLiteral("zoomFitTimeline")));
  timelineTools->addAction(action(QStringLiteral("zoomInTimeline")));
  timelineTools->addSeparator();
  timelineTools->addAction(action(QStringLiteral("commandPalette")));
}

void EditorWindow::createStatusBar() {
  statusBar()->setObjectName(QStringLiteral("editorStatusBar"));
  statusBar()->setAccessibleName(tr("Editor status"));
  workspace_label_ = new QLabel(statusBar());
  workspace_label_->setObjectName(QStringLiteral("workspaceStatus"));
  workspace_label_->setAccessibleName(tr("Current workspace"));
  statusBar()->addPermanentWidget(workspace_label_);
  auto* tool_separator = new QLabel(QStringLiteral("  •  "), statusBar());
  tool_separator->setProperty("muted", true);
  statusBar()->addPermanentWidget(tool_separator);
  tool_label_ = new QLabel(tr("Tool: Select"), statusBar());
  tool_label_->setObjectName(QStringLiteral("timelineToolStatus"));
  tool_label_->setAccessibleName(tr("Active timeline tool"));
  statusBar()->addPermanentWidget(tool_label_);
  auto* separator = new QLabel(QStringLiteral("  •  "), statusBar());
  separator->setProperty("muted", true);
  statusBar()->addPermanentWidget(separator);
  auto* format = new QLabel(tr("Rec.709 SDR · 48 kHz Stereo"), statusBar());
  format->setObjectName(QStringLiteral("sequenceFormatStatus"));
  format->setAccessibleName(tr("Sequence output format"));
  sequence_format_label_ = format;
  statusBar()->addPermanentWidget(format);
  auto* jobs_separator = new QLabel(QStringLiteral("  •  "), statusBar());
  jobs_separator->setProperty("muted", true);
  statusBar()->addPermanentWidget(jobs_separator);
  job_activity_label_ = new QLabel(tr("Jobs: idle"), statusBar());
  job_activity_label_->setObjectName(QStringLiteral("jobActivityStatus"));
  job_activity_label_->setAccessibleName(tr("Background job activity"));
  statusBar()->addPermanentWidget(job_activity_label_);
  auto* sync_separator = new QLabel(QStringLiteral("  •  "), statusBar());
  sync_separator->setProperty("muted", true);
  statusBar()->addPermanentWidget(sync_separator);
  av_sync_label_ = new QLabel(tr("A/V: idle"), statusBar());
  av_sync_label_->setObjectName(QStringLiteral("audioSyncStatus"));
  av_sync_label_->setAccessibleName(tr("Audio and video sync status"));
  statusBar()->addPermanentWidget(av_sync_label_);
  updateWorkspaceLabel();
}

void EditorWindow::connectControllerSurface() {
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
    updateFocusedMonitorLabel();
    if (now != nullptr && !isEditorChrome(now, this)) {
      last_content_focus_ = now;
    }
  });
  updateFocusedMonitorLabel();
  connect(media_bin_, &MediaBinWidget::importRequested, this, &EditorWindow::importMediaRequested);
  connect(media_bin_, &MediaBinWidget::insertRequested, this, &EditorWindow::mediaInsertRequested);
  connect(media_bin_, &MediaBinWidget::mediaActivated, this, &EditorWindow::mediaActivated);
  connect(media_bin_, &MediaBinWidget::mediaSelectionChanged, this,
          &EditorWindow::mediaSelectionChanged);
  connect(action(QStringLiteral("manageMediaCache")), &QAction::triggered, this, [this] {
    emit manageMediaCacheRequested();
    cache_browser_->exec();
  });
  connect(program_viewer_, &ProgramViewer::filesDropped, this,
          [this](const QStringList&) { emit importMediaRequested(); });
  connect(program_viewer_, &ProgramViewer::togglePlaybackRequested,
          action(QStringLiteral("playPause")), &QAction::trigger);
  connect(program_viewer_, &ProgramViewer::viewerTransformPressed, this,
          &EditorWindow::viewerTransformPressed);
  connect(program_viewer_, &ProgramViewer::viewerTransformMoved, this,
          &EditorWindow::viewerTransformMoved);
  connect(program_viewer_, &ProgramViewer::viewerTransformReleased, this,
          &EditorWindow::viewerTransformReleased);
  connect(source_viewer_, &ProgramViewer::togglePlaybackRequested, this,
          [this] { emit sourcePlaybackRateRequested(shuttle_rate_ == 0.0 ? 1.0 : 0.0); });
  connect(source_viewer_, &ProgramViewer::markInRequested, this, &EditorWindow::markSourceIn);
  connect(source_viewer_, &ProgramViewer::markOutRequested, this, &EditorWindow::markSourceOut);
  connect(action(QStringLiteral("nestSelectedClips")), &QAction::triggered, this,
          &EditorWindow::nestSelectedClipsRequested);
  connect(sequence_tab_bar_, &QTabBar::currentChanged, this, [this](const int index) {
    if (sequence_tab_bar_ == nullptr || index < 0 || index >= sequence_tab_bar_->count()) {
      return;
    }
    const QString sequence_id = sequence_tab_bar_->tabData(index).toString();
    if (!sequence_id.isEmpty()) {
      emit sequenceActivated(sequence_id);
    }
  });
  connect(timeline_, &TimelineWidget::seekRequested, this, &EditorWindow::seekRequested);
  connect(effects_panel_, &EffectsPanelWidget::effectAddRequested, this,
          &EditorWindow::effectAddRequested);
  connect(inspector_, &InspectorWidget::parameterEdited, this, &EditorWindow::parameterEdited);
  connect(inspector_, &InspectorWidget::assetMetadataEdited, this,
          &EditorWindow::assetMetadataEdited);
  connect(inspector_, &InspectorWidget::keyframeToggleRequested, this,
          &EditorWindow::keyframeToggleRequested);
  connect(inspector_, &InspectorWidget::effectParameterEdited, this,
          &EditorWindow::effectParameterEdited);
  connect(inspector_, &InspectorWidget::effectKeyframeToggleRequested, this,
          &EditorWindow::effectKeyframeToggleRequested);
  connect(inspector_, &InspectorWidget::effectKeyframeSelected, this,
          &EditorWindow::effectKeyframeSelected);
  connect(inspector_, &InspectorWidget::effectKeyframeValueEdited, this,
          &EditorWindow::effectKeyframeValueEdited);
  connect(inspector_, &InspectorWidget::effectKeyframeInterpolationEdited, this,
          &EditorWindow::effectKeyframeInterpolationEdited);
  connect(inspector_, &InspectorWidget::effectKeyframeRemoved, this,
          &EditorWindow::effectKeyframeRemoved);
  connect(inspector_, &InspectorWidget::effectKeyframeControlPointsEdited, this,
          &EditorWindow::effectKeyframeControlPointsEdited);
  connect(inspector_, &InspectorWidget::pickWhiteBalanceRequested, this,
          &EditorWindow::pickWhiteBalanceRequested);
  connect(inspector_, &InspectorWidget::effectLutBrowseRequested, this,
          &EditorWindow::effectLutBrowseRequested);
  connect(inspector_, &InspectorWidget::addTitleRequested, this, &EditorWindow::addTitleRequested);
  connect(inspector_, &InspectorWidget::deleteClipRequested, this,
          [this] { emit deleteSelectionRequested(false); });
  connect(timeline_, &TimelineWidget::transitionActivated, this,
          &EditorWindow::transitionActivated);
  connect(timeline_, &TimelineWidget::transitionDurationEdited, this,
          &EditorWindow::transitionDurationEdited);
  connect(timeline_, &TimelineWidget::transitionRemoved, this, &EditorWindow::transitionRemoved);
  connect(timeline_, &TimelineWidget::transitionPresetChanged, this,
          &EditorWindow::transitionPresetChanged);
  connect(deliver_panel_, &DeliverPanelWidget::exportRequested, this,
          [this](const QString& presetId) { showExportDialog(presetId); });
  connect(action(QStringLiteral("safeGuides")), &QAction::toggled, program_viewer_,
          &ProgramViewer::setSafeGuidesVisible);
  connect(action(QStringLiteral("viewerClipInfo")), &QAction::toggled, this,
          &EditorWindow::programClipInfoToggled);
  connect(action(QStringLiteral("viewerSourceTimecode")), &QAction::toggled, this,
          &EditorWindow::sourceTimecodeToggled);
  connect(marker_list_, &MarkerListWidget::markerActivated, this,
          &EditorWindow::markerListJumpRequested);
  connect(
      media_bin_, &MediaBinWidget::revealInFilesRequested, this, [this](const QString& mediaId) {
        if (media_bin_ != nullptr) {
          const auto items = media_bin_->items();
          const auto found =
              std::find_if(items.begin(), items.end(),
                           [&mediaId](const MediaItemView& item) { return item.id == mediaId; });
          if (found != items.end() && !found->filePath.isEmpty()) {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(found->filePath).absolutePath()));
          }
        }
      });
  connect(action(QStringLiteral("zoomInTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomIn);
  connect(action(QStringLiteral("zoomOutTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomOut);
  connect(action(QStringLiteral("zoomFitTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomToFit);
  connect(action(QStringLiteral("zoomToSelection")), &QAction::triggered, this,
          &EditorWindow::zoomToSelectionRequested);
  const auto bindTool = [this](const char* actionId, TimelineWidget::ToolMode mode) {
    connect(action(QString::fromLatin1(actionId)), &QAction::triggered, this,
            [this, mode] { timeline_->setToolMode(mode); });
  };
  bindTool("tool.select", TimelineWidget::ToolMode::Select);
  bindTool("tool.rippleTrim", TimelineWidget::ToolMode::RippleTrim);
  bindTool("tool.overwriteTrim", TimelineWidget::ToolMode::OverwriteTrim);
  bindTool("tool.roll", TimelineWidget::ToolMode::Roll);
  bindTool("tool.slip", TimelineWidget::ToolMode::Slip);
  bindTool("tool.slide", TimelineWidget::ToolMode::Slide);
  bindTool("tool.razor", TimelineWidget::ToolMode::Razor);
  bindTool("tool.pen", TimelineWidget::ToolMode::Pen);
  bindTool("tool.hand", TimelineWidget::ToolMode::Hand);
  bindTool("tool.zoom", TimelineWidget::ToolMode::Zoom);
  connect(timeline_, &TimelineWidget::toolModeChanged, this, [this] { syncTimelineToolActions(); });
  syncTimelineToolActions();
  const auto bindNudge = [this](const char* objectName, int frames) {
    if (auto* button = precision_trim_->findChild<QToolButton*>(QString::fromLatin1(objectName))) {
      connect(button, &QToolButton::clicked, this, [this, frames] {
        timeline_->nudgeActiveClipByFrames(frames, TimelineWidget::EditIntent::Normal);
      });
    }
  };
  bindNudge("precision.nudge.minus10", -10);
  bindNudge("precision.nudge.minus1", -1);
  bindNudge("precision.nudge.plus1", 1);
  bindNudge("precision.nudge.plus10", 10);
  connect(action(QStringLiteral("previousFrame")), &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourceStepFrameRequested(-1);
      return;
    }
    const auto frame = qMax<qint64>(1, timeline_->timeScale() / 30);
    timeline_->setPlayhead(timeline_->playhead() - frame);
    emit seekRequested(timeline_->playhead());
  });
  connect(action(QStringLiteral("nextFrame")), &QAction::triggered, this, [this] {
    if (sourceMonitorHasFocus()) {
      emit sourceStepFrameRequested(1);
      return;
    }
    const auto frame = qMax<qint64>(1, timeline_->timeScale() / 30);
    timeline_->setPlayhead(timeline_->playhead() + frame);
    emit seekRequested(timeline_->playhead());
  });
}

void EditorWindow::labelInteractiveChrome() {
  labelToolButtonsFromActions(this);
  int tabIndex = 0;
  for (auto* tabBar : findChildren<QTabBar*>()) {
    if (tabBar->objectName().startsWith(QLatin1String("qt_"))) {
      continue;
    }
    if (tabBar->objectName().isEmpty()) {
      tabBar->setObjectName(QStringLiteral("editorPanelTabBar.%1").arg(tabIndex));
    }
    if (tabBar->accessibleName().trimmed().isEmpty()) {
      tabBar->setAccessibleName(tr("Editor panel tabs"));
      tabBar->setAccessibleDescription(
          tr("Switch between inspector, effects, captions, and deliver panels"));
    }
    ++tabIndex;
  }
}

void EditorWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (initialized_) {
    applyCompactLayoutForCurrentSize();
  }
}

QList<QDockWidget*> EditorWindow::dockWidgets() const {
  return {media_dock_,    inspector_dock_, effects_dock_, mixer_dock_,
          captions_dock_, deliver_dock_,   scopes_dock_};
}

QDockWidget* EditorWindow::focusedDockWidget() const {
  if (QDockWidget* dock = dockAncestor(QApplication::focusWidget())) {
    return dock->isHidden() ? nullptr : dock;
  }
  QDockWidget* dock = dockAncestor(last_content_focus_);
  if (dock == nullptr || dock->isHidden()) {
    return nullptr;
  }
  return dock;
}

QByteArray EditorWindow::restorableLayoutState() const {
  if (panel_maximized_ && !layout_before_maximize_.isEmpty()) {
    return layout_before_maximize_;
  }
  return saveState(kUiStateVersion);
}

void EditorWindow::applyCompactLayoutForCurrentSize() {
  if (panel_maximized_) {
    return;
  }
  const int window_width = width();
  const int window_height = height();
  const auto tier = window_width < 1040   ? CompactTier::Compact
                    : window_width < 1280 ? CompactTier::Medium
                                          : CompactTier::Normal;
  if (tier != compact_tier_) {
    compact_tier_ = tier;
    switch (workspace_) {
    case Workspace::Import: {
      const int media_width =
          tier == CompactTier::Compact ? 210 : (tier == CompactTier::Medium ? 250 : 320);
      const int inspector_width =
          tier == CompactTier::Compact ? 220 : (tier == CompactTier::Medium ? 260 : 290);
      resizeShownDock(this, media_dock_, media_width, Qt::Horizontal);
      resizeShownDock(this, inspector_dock_, inspector_width, Qt::Horizontal);
      break;
    }
    case Workspace::Edit: {
      const int media_width =
          tier == CompactTier::Compact ? 200 : (tier == CompactTier::Medium ? 235 : 285);
      const int inspector_width =
          tier == CompactTier::Compact ? 220 : (tier == CompactTier::Medium ? 250 : 310);
      resizeShownDock(this, media_dock_, media_width, Qt::Horizontal);
      QDockWidget* right_dock = nullptr;
      for (auto* dock : {inspector_dock_, effects_dock_, captions_dock_, deliver_dock_}) {
        if (dock != nullptr && dock->isVisible()) {
          right_dock = dock;
          break;
        }
      }
      if (right_dock == nullptr) {
        for (auto* dock : {inspector_dock_, effects_dock_, captions_dock_, deliver_dock_}) {
          if (dock != nullptr && !dock->isHidden()) {
            right_dock = dock;
            break;
          }
        }
      }
      resizeShownDock(this, right_dock, inspector_width, Qt::Horizontal);
      break;
    }
    case Workspace::AudioCaptions: {
      const int captions_width =
          tier == CompactTier::Compact ? 300 : (tier == CompactTier::Medium ? 340 : 370);
      const int mixer_height =
          tier == CompactTier::Compact ? 210 : (tier == CompactTier::Medium ? 235 : 260);
      resizeShownDock(this, captions_dock_, captions_width, Qt::Horizontal);
      resizeShownDock(this, mixer_dock_, mixer_height, Qt::Vertical);
      break;
    }
    case Workspace::Deliver: {
      const int deliver_width =
          tier == CompactTier::Compact ? 320 : (tier == CompactTier::Medium ? 350 : 380);
      resizeShownDock(this, deliver_dock_, deliver_width, Qt::Horizontal);
      break;
    }
    }
  }

  if (viewer_timeline_splitter_ == nullptr) {
    return;
  }
  const bool compact_height = window_height < 760;
  if (compact_height != compact_height_) {
    compact_height_ = compact_height;
    viewer_timeline_splitter_->setSizes(
        compact_height ? QList<int>{kCompactViewerHeight, kCompactTimelineHeight}
                       : QList<int>{kComfortableViewerHeight, kComfortableTimelineHeight});
  }
}

void EditorWindow::maximizeFocusedPanel() {
  if (panel_maximized_) {
    if (!layout_before_maximize_.isEmpty()) {
      restoreState(layout_before_maximize_, kUiStateVersion);
    }
    layout_before_maximize_.clear();
    panel_maximized_ = false;
    compact_tier_ = CompactTier::Normal;
    compact_height_ = false;
    applyCompactLayoutForCurrentSize();
    return;
  }

  layout_before_maximize_ = saveState(kUiStateVersion);
  if (QDockWidget* focused_dock = focusedDockWidget()) {
    for (auto* dock : dockWidgets()) {
      dock->setVisible(dock == focused_dock);
    }
    focused_dock->raise();
  } else {
    for (auto* dock : dockWidgets()) {
      dock->hide();
    }
  }
  panel_maximized_ = true;
}

void EditorWindow::resetWorkspaceLayout() {
  panel_maximized_ = false;
  layout_before_maximize_.clear();
  session_layouts_.remove(workspace_);
  applyDefaultLayout(workspace_);
  compact_tier_ = CompactTier::Normal;
  compact_height_ = false;
  if (viewer_timeline_splitter_ != nullptr) {
    viewer_timeline_splitter_->setSizes({kDefaultViewerHeight, kDefaultTimelineHeight});
  }
  applyCompactLayoutForCurrentSize();
  showTransientMessage(
      tr("Restored the default %1 workspace layout").arg(workspaceDisplayName(workspace_)));
}

void EditorWindow::applyDefaultLayout(Workspace workspace) {
  for (auto* dock : {media_dock_, inspector_dock_, effects_dock_, mixer_dock_, captions_dock_,
                     deliver_dock_, scopes_dock_}) {
    dock->hide();
    dock->setFloating(false);
  }

  addDockWidget(Qt::LeftDockWidgetArea, media_dock_);
  addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
  addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, mixer_dock_);
  addDockWidget(Qt::RightDockWidgetArea, captions_dock_);
  addDockWidget(Qt::RightDockWidgetArea, deliver_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, scopes_dock_);
  tabifyDockWidget(inspector_dock_, effects_dock_);
  tabifyDockWidget(effects_dock_, captions_dock_);
  tabifyDockWidget(captions_dock_, deliver_dock_);

  switch (workspace) {
  case Workspace::Import:
    media_dock_->show();
    inspector_dock_->show();
    inspector_dock_->raise();
    resizeDocks({media_dock_, inspector_dock_}, {320, 290}, Qt::Horizontal);
    break;
  case Workspace::Edit:
    media_dock_->show();
    inspector_dock_->show();
    effects_dock_->show();
    inspector_dock_->raise();
    resizeDocks({media_dock_, inspector_dock_}, {285, 310}, Qt::Horizontal);
    break;
  case Workspace::AudioCaptions:
    captions_dock_->show();
    mixer_dock_->show();
    captions_dock_->raise();
    resizeDocks({captions_dock_}, {370}, Qt::Horizontal);
    resizeDocks({mixer_dock_}, {260}, Qt::Vertical);
    break;
  case Workspace::Deliver:
    deliver_dock_->show();
    deliver_dock_->raise();
    resizeDocks({deliver_dock_}, {380}, Qt::Horizontal);
    break;
  }
  labelInteractiveChrome();
}

void EditorWindow::updateWorkspaceActions() {
  if (auto* current = workspace_actions_.value(workspace_)) {
    const QSignalBlocker blocker(current);
    current->setChecked(true);
  }
}

void EditorWindow::updateWorkspaceLabel() {
  if (workspace_label_ != nullptr) {
    workspace_label_->setText(tr("Workspace: %1").arg(workspaceDisplayName(workspace_)));
  }
}

void EditorWindow::rippleInsertFromSource() {
  emit sourceRippleInsertRequested();
}

void EditorWindow::overwriteInsertFromSource() {
  emit sourceOverwriteInsertRequested();
}

void EditorWindow::markSourceIn() {
  emit sourceMarkInRequested();
}

void EditorWindow::markSourceOut() {
  emit sourceMarkOutRequested();
}

void EditorWindow::seekSource(qint64 position) {
  emit sourceSeekRequested(position);
}

bool EditorWindow::sourceMonitorHasFocus() const {
  if (source_container_ == nullptr || !source_container_->isVisible()) {
    return false;
  }
  QWidget* focus = QApplication::focusWidget();
  return focus != nullptr && (focus == source_viewer_ || source_container_->isAncestorOf(focus));
}

void EditorWindow::updateFocusedMonitorLabel() {
  if (monitor_focus_label_ == nullptr) {
    return;
  }
  monitor_focus_label_->setText(sourceMonitorHasFocus() ? tr("Commands target: Source")
                                                        : tr("Commands target: Program"));
}

void EditorWindow::setShuttleRate(double rate) {
  if (qFuzzyCompare(shuttle_rate_ + 1.0, rate + 1.0)) {
    return;
  }
  shuttle_rate_ = rate;
  if (transport_label_ != nullptr) {
    if (qFuzzyIsNull(rate)) {
      transport_label_->setText(tr("Stopped"));
    } else if (rate > 0.0) {
      transport_label_->setText(tr("Forward %1×").arg(rate, 0, 'g', 2));
    } else {
      transport_label_->setText(tr("Reverse %1×").arg(-rate, 0, 'g', 2));
    }
  }
  emit playbackRateRequested(rate);
}

void EditorWindow::stepShuttle(int direction) {
  const auto sameDirection = shuttle_rate_ * static_cast<double>(direction) > 0.0;
  const auto magnitude = sameDirection ? qMin(8.0, qAbs(shuttle_rate_) * 2.0) : 1.0;
  setShuttleRate(static_cast<double>(direction) * magnitude);
}

void EditorWindow::addAction(const QString& id, QAction* actionToAdd) {
  actionToAdd->setObjectName(QStringLiteral("action.%1").arg(id));
  actionToAdd->setProperty("commandId", id);
  actions_.insert(id, actionToAdd);
  QMainWindow::addAction(actionToAdd);
}

QString EditorWindow::settingsKeyForWorkspace(Workspace workspace) const {
  return QStringLiteral("ui/workspaces/v1/%1").arg(static_cast<int>(workspace));
}

void EditorWindow::applyTimelineToolIcons() {
  const auto setIcon = [this](const char* id, TimelineCursorKind kind) {
    if (auto* tool = action(QString::fromLatin1(id))) {
      tool->setIcon(timelineToolIcon(kind));
    }
  };
  setIcon("tool.select", TimelineCursorKind::Arrow);
  setIcon("tool.rippleTrim", TimelineCursorKind::RippleTrim);
  setIcon("tool.overwriteTrim", TimelineCursorKind::OverwriteTrim);
  setIcon("tool.roll", TimelineCursorKind::Roll);
  setIcon("tool.slip", TimelineCursorKind::Slip);
  setIcon("tool.slide", TimelineCursorKind::Slide);
  setIcon("tool.razor", TimelineCursorKind::Razor);
  setIcon("tool.pen", TimelineCursorKind::Pen);
  setIcon("tool.hand", TimelineCursorKind::HandOpen);
  setIcon("tool.zoom", TimelineCursorKind::ZoomIn);
  setIcon("tool.trackSelectForward", TimelineCursorKind::TrackSelectForward);
}

void EditorWindow::syncTimelineToolActions() {
  if (timeline_ == nullptr) {
    return;
  }
  const auto mode = timeline_->toolMode();
  const auto check = [this](const char* id, const bool on) {
    if (auto* tool = action(QString::fromLatin1(id))) {
      QSignalBlocker blocker(tool);
      tool->setChecked(on);
    }
  };
  check("tool.select", mode == TimelineWidget::ToolMode::Select);
  check("tool.rippleTrim", mode == TimelineWidget::ToolMode::RippleTrim);
  check("tool.overwriteTrim", mode == TimelineWidget::ToolMode::OverwriteTrim);
  check("tool.roll", mode == TimelineWidget::ToolMode::Roll);
  check("tool.slip", mode == TimelineWidget::ToolMode::Slip);
  check("tool.slide", mode == TimelineWidget::ToolMode::Slide);
  check("tool.razor", mode == TimelineWidget::ToolMode::Razor);
  check("tool.pen", mode == TimelineWidget::ToolMode::Pen);
  check("tool.hand", mode == TimelineWidget::ToolMode::Hand);
  check("tool.zoom", mode == TimelineWidget::ToolMode::Zoom);
  check("tool.trackSelectForward", mode == TimelineWidget::ToolMode::TrackSelectForward);

  QString name = tr("Select");
  switch (mode) {
  case TimelineWidget::ToolMode::RippleTrim:
    name = tr("Ripple Trim");
    break;
  case TimelineWidget::ToolMode::OverwriteTrim:
    name = tr("Overwrite Trim");
    break;
  case TimelineWidget::ToolMode::Roll:
    name = tr("Roll");
    break;
  case TimelineWidget::ToolMode::Slip:
    name = tr("Slip");
    break;
  case TimelineWidget::ToolMode::Slide:
    name = tr("Slide");
    break;
  case TimelineWidget::ToolMode::TrackSelectForward:
    name = tr("Track Select");
    break;
  case TimelineWidget::ToolMode::Razor:
    name = tr("Razor");
    break;
  case TimelineWidget::ToolMode::Pen:
    name = tr("Pen");
    break;
  case TimelineWidget::ToolMode::Hand:
    name = tr("Hand");
    break;
  case TimelineWidget::ToolMode::Zoom:
    name = tr("Zoom");
    break;
  case TimelineWidget::ToolMode::Select:
    break;
  }
  if (tool_label_ != nullptr) {
    tool_label_->setText(tr("Tool: %1").arg(name));
  }
}

QString EditorWindow::workspaceDisplayName(Workspace workspace) {
  switch (workspace) {
  case Workspace::Import:
    return tr("Import");
  case Workspace::Edit:
    return tr("Edit");
  case Workspace::AudioCaptions:
    return tr("Audio & Captions");
  case Workspace::Deliver:
    return tr("Deliver");
  }
  return tr("Edit");
}

QString EditorWindow::darkStyleSheet() {
  return QStringLiteral(R"(
        QMainWindow, QDialog, QWidget {
            background: #141618;
            color: #d8dce4;
            font-family: "Noto Sans", "Cantarell", "Source Sans 3", sans-serif;
            font-size: 10pt;
        }
        QMainWindow::separator { background: #2a2e34; width: 4px; height: 4px; }
        QMenuBar { background: #1a1d21; border-bottom: 1px solid #2f343b; padding: 1px; }
        QMenuBar::item { padding: 4px 8px; border-radius: 2px; }
        QMenuBar::item:selected, QMenu::item:selected { background: #3a2e22; }
        QMenu { background: #1c2025; border: 1px solid #3a4048; padding: 4px; }
        QMenu::item { padding: 5px 26px 5px 10px; border-radius: 2px; }
        QToolBar { background: #1a1d21; border: 0; border-bottom: 1px solid #2f343b; spacing: 2px; padding: 2px; }
        QToolButton { background: transparent; color: #d8dce4; border: 1px solid transparent; border-radius: 3px; padding: 4px; }
        QToolButton:hover { background: #2a2f36; border-color: #3d444d; }
        QToolButton:pressed, QToolButton:checked { background: #4a3420; border-color: #c4783a; }
        QToolButton:disabled { color: #6d7380; }
        QPushButton { background: #2c333c; border: 1px solid #4a5360; border-radius: 3px; padding: 6px 11px; }
        QPushButton:hover { background: #384049; }
        QPushButton:pressed { background: #252b32; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
            background: #101214; border: 1px solid #3a4048; border-radius: 3px; padding: 4px; selection-background-color: #4a3420;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus { border-color: #c4783a; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QDockWidget { color: #d8dce4; font-weight: 600; }
        QDockWidget::title { background: #1c2025; border-bottom: 1px solid #2f343b; padding: 6px 8px; text-align: left; }
        QGroupBox { border: 1px solid #2f343b; border-radius: 4px; margin-top: 12px; padding-top: 6px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; color: #c4cad4; }
        QTableWidget, QListWidget { background: #16191d; alternate-background-color: #1a1e22; border: 1px solid #2f343b; border-radius: 3px; outline: 0; }
        QTableWidget::item, QListWidget::item { padding: 4px; }
        QTableWidget::item:selected, QListWidget::item:selected { background: #4a3420; color: #ffffff; }
        QHeaderView::section { background: #1c2025; color: #b8bfc9; border: 0; border-right: 1px solid #2f343b; border-bottom: 1px solid #2f343b; padding: 4px; }
        QScrollBar:vertical { background: #16191d; width: 11px; margin: 0; }
        QScrollBar:horizontal { background: #16191d; height: 11px; margin: 0; }
        QScrollBar::handle { background: #4a515c; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 2px; }
        QScrollBar::handle:hover { background: #5c6572; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QSplitter::handle { background: #2a2e34; }
        QStatusBar { background: #1a1d21; border-top: 1px solid #2f343b; color: #b3bac4; }
        QLabel[muted="true"] { color: #8b929e; }
        QProgressBar { background: #101214; border: 1px solid #2f343b; border-radius: 3px; }
        QProgressBar::chunk { background: #c4783a; }
        QSlider::groove:horizontal { background: #101214; height: 5px; border-radius: 2px; }
        QSlider::handle:horizontal { background: #c4cad4; border: 1px solid #d8dce4; width: 12px; margin: -5px 0; border-radius: 3px; }
        QSlider::groove:vertical { background: #101214; width: 5px; border-radius: 2px; }
        QSlider::handle:vertical { background: #c4cad4; border: 1px solid #d8dce4; height: 12px; margin: 0 -5px; border-radius: 3px; }
        QCheckBox, QRadioButton { spacing: 6px; }
        QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid #4a5360; background: #101214; }
        QCheckBox::indicator { border-radius: 2px; }
        QRadioButton::indicator { border-radius: 7px; }
        QCheckBox::indicator:checked, QRadioButton::indicator:checked { background: #c4783a; border-color: #e08a45; }
        QTreeWidget, QTreeView { background: #16191d; alternate-background-color: #1a1e22; border: 1px solid #2f343b; }
        QTreeWidget::item:selected, QTreeView::item:selected { background: #4a3420; color: #ffffff; }
        QTabWidget::pane { border: 1px solid #2f343b; background: #16191d; }
        QAbstractSpinBox { background: #101214; border: 1px solid #3a4048; border-radius: 3px; padding: 3px; }
        QToolTip { background: #0e1013; color: #eef1f6; border: 1px solid #c4783a; padding: 4px; }
        QTabBar::tab { background: #1a1d21; color: #b3bac4; padding: 6px 12px; border: 1px solid #2f343b; }
        QTabBar::tab:selected { background: #24292f; color: #eef1f6; border-bottom-color: #c4783a; }
    )");
}

ProgramViewer* EditorWindow::programOutputViewer() const noexcept {
  return program_output_window_ != nullptr ? program_output_window_->viewer() : nullptr;
}

void EditorWindow::toggleProgramFullscreen() {
  if (program_fullscreen_active_) {
    exitProgramFullscreen();
    return;
  }
  if (program_viewer_ == nullptr || program_viewer_splitter_ == nullptr) {
    return;
  }

  QScreen* screen = this->screen();
  if (screen == nullptr) {
    screen = QGuiApplication::primaryScreen();
  }
  if (screen == nullptr) {
    return;
  }

  program_viewer_original_parent_ = program_viewer_->parentWidget();
  program_viewer_splitter_index_ = program_viewer_splitter_->indexOf(program_viewer_);
  if (program_fullscreen_shell_ == nullptr) {
    program_fullscreen_shell_ = new QWidget(nullptr, Qt::Window);
    program_fullscreen_shell_->setObjectName(QStringLiteral("programFullscreenShell"));
    program_fullscreen_shell_->setAccessibleName(tr("Program monitor fullscreen"));
    program_fullscreen_shell_->setWindowTitle(tr("Program Monitor"));
    program_fullscreen_shell_->setAttribute(Qt::WA_QuitOnClose, false);
    auto* layout = new QVBoxLayout(program_fullscreen_shell_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
  }

  program_viewer_->setParent(program_fullscreen_shell_);
  program_fullscreen_shell_->layout()->addWidget(program_viewer_);
  program_fullscreen_shell_->setScreen(screen);
  program_fullscreen_shell_->showFullScreen();
  program_viewer_->show();
  program_viewer_->updateGeometry();

  program_fullscreen_active_ = true;
  if (auto* fullscreenAction = action(QStringLiteral("programFullscreen"))) {
    fullscreenAction->setChecked(true);
  }
  if (program_fullscreen_escape_ != nullptr) {
    program_fullscreen_escape_->setEnabled(true);
  }
}

void EditorWindow::exitProgramFullscreen() {
  if (!program_fullscreen_active_ || program_viewer_ == nullptr ||
      program_viewer_splitter_ == nullptr) {
    return;
  }

  program_viewer_->setParent(program_viewer_original_parent_);
  program_viewer_splitter_->insertWidget(program_viewer_splitter_index_, program_viewer_);
  program_viewer_->show();
  program_viewer_->updateGeometry();
  if (program_fullscreen_shell_ != nullptr) {
    program_fullscreen_shell_->hide();
  }

  program_fullscreen_active_ = false;
  if (auto* fullscreenAction = action(QStringLiteral("programFullscreen"))) {
    fullscreenAction->setChecked(false);
  }
  if (program_fullscreen_escape_ != nullptr) {
    program_fullscreen_escape_->setEnabled(false);
  }
}

void EditorWindow::setProgramOutputScreen(QScreen* screen) {
  if (screen == nullptr) {
    if (program_output_window_ != nullptr) {
      program_output_window_->close();
      program_output_window_.reset();
    }
    if (settings_ != nullptr) {
      settings_->remove(QStringLiteral("display/programOutputScreen"));
    }
    return;
  }

  if (program_output_window_ == nullptr) {
    program_output_window_ = std::make_unique<ProgramOutputWindow>();
    connect(program_output_window_.get(), &ProgramOutputWindow::nativePresentationReady, this,
            &EditorWindow::programOutputPresentationReady);
    connect(program_output_window_.get(), &ProgramOutputWindow::nativePresentationResized, this,
            &EditorWindow::programOutputPresentationResized);
    connect(program_output_window_.get(), &ProgramOutputWindow::nativePresentationLost, this,
            &EditorWindow::programOutputPresentationLost);
    connect(program_output_window_.get(), &ProgramOutputWindow::outputClosed, this, [this] {
      program_output_window_.reset();
      if (settings_ != nullptr) {
        settings_->remove(QStringLiteral("display/programOutputScreen"));
      }
      emit programOutputClosed();
    });
  }

  program_output_window_->showOnScreen(screen);
  if (settings_ != nullptr) {
    settings_->setValue(QStringLiteral("display/programOutputScreen"), screen->name());
  }
}

void EditorWindow::rebuildProgramOutputMenu() {
  if (program_output_menu_ == nullptr) {
    return;
  }
  program_output_menu_->clear();

  auto* noneAction = program_output_menu_->addAction(tr("None"));
  noneAction->setObjectName(QStringLiteral("programOutputScreenNone"));
  connect(noneAction, &QAction::triggered, this, [this] { setProgramOutputScreen(nullptr); });

  const auto screens = QGuiApplication::screens();
  for (int index = 0; index < screens.size(); ++index) {
    QScreen* screen = screens.at(index);
    if (screen == nullptr) {
      continue;
    }
    const QString label = tr("Display %1 — %2").arg(index + 1).arg(screen->name());
    auto* screenAction = program_output_menu_->addAction(label);
    screenAction->setObjectName(QStringLiteral("programOutputScreen.%1").arg(index));
    connect(screenAction, &QAction::triggered, this,
            [this, screen] { setProgramOutputScreen(screen); });
  }
}

void EditorWindow::restoreProgramOutputScreen() {
  if (settings_ == nullptr) {
    return;
  }
  const QString stored =
      settings_->value(QStringLiteral("display/programOutputScreen")).toString().trimmed();
  if (stored.isEmpty()) {
    return;
  }
  for (QScreen* screen : QGuiApplication::screens()) {
    if (screen != nullptr && screen->name() == stored) {
      setProgramOutputScreen(screen);
      return;
    }
  }
}

} // namespace video_editor::desktop_ui
