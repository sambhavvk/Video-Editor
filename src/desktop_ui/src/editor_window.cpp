// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/editor_window.hpp"

#include "video_editor/desktop_ui/cache_browser_dialog.hpp"
#include "video_editor/desktop_ui/command_palette.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
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
  createCentralArea();
  createPanels();
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
  deliver_panel_->setExportEnabled(!items.isEmpty());
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

void EditorWindow::showTransientMessage(const QString& message, int timeoutMs) {
  statusBar()->showMessage(message, timeoutMs);
}

void EditorWindow::setWorkspace(Workspace workspace) {
  if (initialized_) {
    session_layouts_.insert(workspace_, saveState(kUiStateVersion));
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
  updateWorkspaceActions();
  updateWorkspaceLabel();
  labelInteractiveChrome();
  emit workspaceChanged(workspace_);
}

void EditorWindow::setSourceMonitorVisible(bool visible) {
  source_container_->setVisible(visible);
  if (auto* toggle = action(QStringLiteral("sourceMonitor"))) {
    const QSignalBlocker blocker(toggle);
    toggle->setChecked(visible);
  }
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
}

void EditorWindow::saveUiState() {
  if (settings_ == nullptr) {
    return;
  }
  session_layouts_.insert(workspace_, saveState(kUiStateVersion));
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

  auto* vertical = new QSplitter(Qt::Vertical, central);
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
  source_viewer_->setTitle(tr("Source"));
  sourceLayout->addWidget(source_viewer_);

  program_viewer_ = new ProgramViewer(viewerSplitter);
  program_viewer_->setObjectName(QStringLiteral("programViewer"));
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
  transportLayout->addStretch();
  for (const auto* id : {"previousFrame", "reverse", "stop", "playPause", "forward", "nextFrame"}) {
    transportLayout->addWidget(makeActionButton(action(QString::fromLatin1(id)), transport));
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
  for (const auto* id : {"tool.rippleTrim", "tool.roll", "tool.slip", "tool.slide"}) {
    auto* mode = makeActionButton(action(QString::fromLatin1(id)), precision_trim_);
    mode->setObjectName(QStringLiteral("precision.%1").arg(QString::fromLatin1(id)));
    mode->setToolButtonStyle(Qt::ToolButtonTextOnly);
    trimLayout->addWidget(mode);
  }
  trimLayout->addStretch();
  auto* hint = new QLabel(tr("Alt + Arrow for fine trim"), precision_trim_);
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
  timelineLayout->addWidget(timeline_, 1);

  vertical->addWidget(viewerArea);
  vertical->addWidget(timelineArea);
  vertical->setStretchFactor(0, 5);
  vertical->setStretchFactor(1, 4);
  vertical->setSizes({470, 350});
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
  cache_browser_ = new CacheBrowserDialog(this);

  media_dock_ = makeDock(QStringLiteral("mediaDock"), tr("Media Bin"), media_bin_, this);
  inspector_dock_ = makeDock(QStringLiteral("inspectorDock"), tr("Inspector"), inspector_, this);
  effects_dock_ = makeDock(QStringLiteral("effectsDock"), tr("Effects"), effects_panel_, this);
  mixer_dock_ = makeDock(QStringLiteral("mixerDock"), tr("Audio Mixer"), audio_mixer_, this);
  captions_dock_ =
      makeDock(QStringLiteral("captionsDock"), tr("Captions & Transcript"), captions_panel_, this);
  deliver_dock_ = makeDock(QStringLiteral("deliverDock"), tr("Deliver"), deliver_panel_, this);

  addDockWidget(Qt::LeftDockWidgetArea, media_dock_);
  addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
  addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, mixer_dock_);
  addDockWidget(Qt::RightDockWidgetArea, captions_dock_);
  addDockWidget(Qt::RightDockWidgetArea, deliver_dock_);
  tabifyDockWidget(inspector_dock_, effects_dock_);
  tabifyDockWidget(effects_dock_, captions_dock_);
  tabifyDockWidget(captions_dock_, deliver_dock_);
}

void EditorWindow::createActions() {
  const auto create = [this](const QString& id, const QString& text, const QString& toolTip,
                             const QKeySequence& shortcut = {}) {
    auto* created = new QAction(text, this);
    created->setToolTip(toolTip);
    created->setStatusTip(toolTip);
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
  create(QStringLiteral("manageMediaCache"), tr("Manage Media Cache…"),
         tr("Review cache use and set the media cache budget"));
  auto* exportAction = create(QStringLiteral("export"), tr("Export Video…"),
                              tr("Open the Deliver workspace"), QKeySequence{tr("Ctrl+E")});
  exportAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
  create(QStringLiteral("quit"), tr("Quit"), tr("Close the application"), QKeySequence::Quit);

  create(QStringLiteral("undo"), tr("Undo"), tr("Undo the last edit"), QKeySequence::Undo);
  create(QStringLiteral("redo"), tr("Redo"), tr("Redo the last undone edit"), QKeySequence::Redo);
  create(QStringLiteral("splitClip"), tr("Split Clip"), tr("Split selected clips at the playhead"),
         QKeySequence{tr("Ctrl+B")});
  create(QStringLiteral("deleteSelection"), tr("Delete"), tr("Delete the selection"),
         QKeySequence{Qt::Key_Delete});
  create(QStringLiteral("rippleDelete"), tr("Ripple Delete"),
         tr("Delete the selection and close the gap"), QKeySequence{tr("Shift+Delete")});

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

  auto* sourceMonitor =
      create(QStringLiteral("sourceMonitor"), tr("Source Monitor"),
             tr("Show a second monitor for source media"), QKeySequence{tr("Shift+2")});
  sourceMonitor->setCheckable(true);
  auto* precisionTrim = create(QStringLiteral("precisionTrim"), tr("Precision Trim Controls"),
                               tr("Show precision trim controls"), QKeySequence{tr("T")});
  precisionTrim->setCheckable(true);
  auto* safeGuides = create(QStringLiteral("safeGuides"), tr("Safe Guides"),
                            tr("Show title and action safe guides"));
  safeGuides->setCheckable(true);
  create(QStringLiteral("commandPalette"), tr("Command Palette…"), tr("Search and run any command"),
         QKeySequence{tr("Ctrl+Shift+P")});

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
  connect(exportAction, &QAction::triggered, this, [this] { setWorkspace(Workspace::Deliver); });
  connect(action(QStringLiteral("quit")), &QAction::triggered, this, &QWidget::close);
  connect(action(QStringLiteral("undo")), &QAction::triggered, this, &EditorWindow::undoRequested);
  connect(action(QStringLiteral("redo")), &QAction::triggered, this, &EditorWindow::redoRequested);
  connect(action(QStringLiteral("splitClip")), &QAction::triggered, this,
          &EditorWindow::splitClipRequested);
  connect(action(QStringLiteral("deleteSelection")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(false); });
  connect(action(QStringLiteral("rippleDelete")), &QAction::triggered, this,
          [this] { emit deleteSelectionRequested(true); });

  connect(reverse, &QAction::triggered, this, [this] { stepShuttle(-1); });
  connect(stop, &QAction::triggered, this, [this] { setShuttleRate(0.0); });
  connect(playPause, &QAction::triggered, this,
          [this] { setShuttleRate(shuttle_rate_ == 0.0 ? 1.0 : 0.0); });
  connect(forward, &QAction::triggered, this, [this] { stepShuttle(1); });
  connect(sourceMonitor, &QAction::toggled, this, &EditorWindow::setSourceMonitorVisible);
  connect(precisionTrim, &QAction::toggled, this, &EditorWindow::setPrecisionTrimVisible);
}

void EditorWindow::createMenus() {
  menuBar()->setAccessibleName(tr("Application menu"));
  auto* file = menuBar()->addMenu(tr("&File"));
  file->setObjectName(QStringLiteral("fileMenu"));
  file->setAccessibleName(tr("File"));
  file->addAction(action(QStringLiteral("newProject")));
  file->addAction(action(QStringLiteral("openProject")));
  file->addSeparator();
  file->addAction(action(QStringLiteral("saveProject")));
  file->addAction(action(QStringLiteral("saveProjectAs")));
  file->addSeparator();
  file->addAction(action(QStringLiteral("importMedia")));
  file->addAction(action(QStringLiteral("manageMediaCache")));
  file->addAction(action(QStringLiteral("export")));
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
  timelineMenu->addAction(action(QStringLiteral("zoomInTimeline")));
  timelineMenu->addAction(action(QStringLiteral("zoomOutTimeline")));
  timelineMenu->addAction(action(QStringLiteral("zoomFitTimeline")));
  timelineMenu->addSeparator();
  for (const auto* id : {"tool.select", "tool.rippleTrim", "tool.overwriteTrim", "tool.roll",
                         "tool.slip", "tool.slide"}) {
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
  view->addAction(action(QStringLiteral("safeGuides")));
  auto* panels = view->addMenu(tr("Panels"));
  panels->setObjectName(QStringLiteral("panelsMenu"));
  panels->setAccessibleName(tr("Panels"));
  for (auto* dock :
       {media_dock_, inspector_dock_, effects_dock_, mixer_dock_, captions_dock_, deliver_dock_}) {
    panels->addAction(dock->toggleViewAction());
  }

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
                             tr("J / K / L — reverse, stop, forward\n"
                                "Space — play or pause\n"
                                "Comma / Period — previous or next frame\n"
                                "Ctrl+1…4 — switch workspace\n"
                                "Ctrl+Shift+P — command palette"));
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
  timelineTools->addAction(action(QStringLiteral("splitClip")));
  timelineTools->addSeparator();
  for (const auto* id : {"tool.select", "tool.rippleTrim", "tool.overwriteTrim", "tool.roll",
                         "tool.slip", "tool.slide"}) {
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
  auto* separator = new QLabel(QStringLiteral("  •  "), statusBar());
  separator->setProperty("muted", true);
  statusBar()->addPermanentWidget(separator);
  auto* format = new QLabel(tr("Rec.709 SDR · 48 kHz Stereo"), statusBar());
  format->setObjectName(QStringLiteral("sequenceFormatStatus"));
  format->setAccessibleName(tr("Sequence output format"));
  statusBar()->addPermanentWidget(format);
  updateWorkspaceLabel();
}

void EditorWindow::connectControllerSurface() {
  connect(media_bin_, &MediaBinWidget::importRequested, this, &EditorWindow::importMediaRequested);
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
  connect(source_viewer_, &ProgramViewer::togglePlaybackRequested,
          action(QStringLiteral("playPause")), &QAction::trigger);
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
  connect(inspector_, &InspectorWidget::addTitleRequested, this, &EditorWindow::addTitleRequested);
  connect(timeline_, &TimelineWidget::transitionActivated, this,
          &EditorWindow::transitionActivated);
  connect(timeline_, &TimelineWidget::transitionDurationEdited, this,
          &EditorWindow::transitionDurationEdited);
  connect(timeline_, &TimelineWidget::transitionRemoved, this, &EditorWindow::transitionRemoved);
  connect(timeline_, &TimelineWidget::transitionPresetChanged, this,
          &EditorWindow::transitionPresetChanged);
  connect(deliver_panel_, &DeliverPanelWidget::exportRequested, this,
          &EditorWindow::exportRequested);
  connect(action(QStringLiteral("safeGuides")), &QAction::toggled, program_viewer_,
          &ProgramViewer::setSafeGuidesVisible);
  connect(action(QStringLiteral("zoomInTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomIn);
  connect(action(QStringLiteral("zoomOutTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomOut);
  connect(action(QStringLiteral("zoomFitTimeline")), &QAction::triggered, timeline_,
          &TimelineWidget::zoomToFit);
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
  connect(action(QStringLiteral("previousFrame")), &QAction::triggered, this, [this] {
    const auto frame = qMax<qint64>(1, timeline_->timeScale() / 30);
    timeline_->setPlayhead(timeline_->playhead() - frame);
    emit seekRequested(timeline_->playhead());
  });
  connect(action(QStringLiteral("nextFrame")), &QAction::triggered, this, [this] {
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

void EditorWindow::applyDefaultLayout(Workspace workspace) {
  for (auto* dock :
       {media_dock_, inspector_dock_, effects_dock_, mixer_dock_, captions_dock_, deliver_dock_}) {
    dock->hide();
    dock->setFloating(false);
  }

  addDockWidget(Qt::LeftDockWidgetArea, media_dock_);
  addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
  addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
  addDockWidget(Qt::BottomDockWidgetArea, mixer_dock_);
  addDockWidget(Qt::RightDockWidgetArea, captions_dock_);
  addDockWidget(Qt::RightDockWidgetArea, deliver_dock_);
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
            background: #1f2228;
            color: #d9dde5;
            font-size: 10pt;
        }
        QMainWindow::separator { background: #353943; width: 4px; height: 4px; }
        QMenuBar { background: #252830; border-bottom: 1px solid #3a3e48; padding: 2px; }
        QMenuBar::item { padding: 5px 9px; border-radius: 3px; }
        QMenuBar::item:selected, QMenu::item:selected { background: #3c506f; }
        QMenu { background: #282c33; border: 1px solid #4a4f5b; padding: 5px; }
        QMenu::item { padding: 6px 28px 6px 10px; border-radius: 3px; }
        QToolBar { background: #282b32; border: 0; border-bottom: 1px solid #3a3e48; spacing: 3px; padding: 3px; }
        QToolButton { background: transparent; color: #dce1e9; border: 1px solid transparent; border-radius: 4px; padding: 5px; }
        QToolButton:hover { background: #373c46; border-color: #4a505c; }
        QToolButton:pressed, QToolButton:checked { background: #415b80; border-color: #6384b3; }
        QToolButton:disabled { color: #737985; }
        QPushButton { background: #35445b; border: 1px solid #526987; border-radius: 4px; padding: 7px 12px; }
        QPushButton:hover { background: #405879; }
        QPushButton:pressed { background: #2f4059; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
            background: #181a1f; border: 1px solid #444a55; border-radius: 4px; padding: 5px; selection-background-color: #476892;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus { border-color: #6d91c4; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QDockWidget { color: #dce1e9; font-weight: 600; }
        QDockWidget::title { background: #2a2e36; border-bottom: 1px solid #3e434e; padding: 7px 8px; text-align: left; }
        QGroupBox { border: 1px solid #3e434e; border-radius: 5px; margin-top: 12px; padding-top: 7px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; color: #c9cfda; }
        QTableWidget, QListWidget { background: #202329; alternate-background-color: #24272e; border: 1px solid #3b404a; border-radius: 4px; outline: 0; }
        QTableWidget::item, QListWidget::item { padding: 5px; }
        QTableWidget::item:selected, QListWidget::item:selected { background: #415f86; color: #ffffff; }
        QHeaderView::section { background: #2b2f37; color: #bfc5d0; border: 0; border-right: 1px solid #3c414b; border-bottom: 1px solid #3c414b; padding: 5px; }
        QScrollBar:vertical { background: #202329; width: 12px; margin: 0; }
        QScrollBar:horizontal { background: #202329; height: 12px; margin: 0; }
        QScrollBar::handle { background: #505662; border-radius: 5px; min-height: 24px; min-width: 24px; margin: 2px; }
        QScrollBar::handle:hover { background: #626a78; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QSplitter::handle { background: #383d46; }
        QStatusBar { background: #252830; border-top: 1px solid #3a3e48; color: #b9c0cb; }
        QLabel[muted="true"] { color: #9299a6; }
        QProgressBar { background: #15171b; border: 1px solid #3a3f49; border-radius: 3px; }
        QProgressBar::chunk { background: #58b27c; }
        QSlider::groove:vertical { background: #15171b; width: 5px; border-radius: 2px; }
        QSlider::handle:vertical { background: #aab5c7; border: 1px solid #d4dae4; height: 12px; margin: 0 -5px; border-radius: 3px; }
        QToolTip { background: #111318; color: #eef1f6; border: 1px solid #5b626f; padding: 4px; }
    )");
}

} // namespace video_editor::desktop_ui
