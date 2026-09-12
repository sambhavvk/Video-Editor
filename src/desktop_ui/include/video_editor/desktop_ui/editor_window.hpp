// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/cache_browser_dialog.hpp"
#include "video_editor/desktop_ui/export_dialog.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/ui_types.hpp"

#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <memory>

class QAction;
class QCloseEvent;
class QDockWidget;
class QFrame;
class QLabel;
class QScreen;
class QSettings;
class QShortcut;
class QSplitter;
class QStackedWidget;
class QToolBar;

namespace video_editor::desktop_ui {

class AudioMixerWidget;
class CaptionsPanelWidget;
class CommandPalette;
class DeliverPanelWidget;
class EffectsPanelWidget;
class InspectorWidget;
class MediaBinWidget;
class MarkerListWidget;
class ProgramOutputWindow;
class ProgramViewer;
class ScopeWidget;
class TimelineWidget;

class EditorWindow final : public QMainWindow {
  Q_OBJECT
  Q_PROPERTY(Workspace workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)

public:
  // When settings is null, an application-scoped QSettings instance is used.
  // A supplied settings object remains owned by its caller.
  explicit EditorWindow(QSettings* settings = nullptr, QWidget* parent = nullptr);
  ~EditorWindow() override;

  [[nodiscard]] Workspace workspace() const noexcept {
    return workspace_;
  }
  [[nodiscard]] QAction* action(const QString& id) const;
  [[nodiscard]] QKeySequence shortcutDefault(const QString& commandId) const;
  [[nodiscard]] QString applyShortcutBinding(const QString& commandId,
                                             const QKeySequence& shortcut,
                                             bool replaceConflicts = false);
  void resetShortcutBinding(const QString& commandId);
  [[nodiscard]] ProgramViewer* programViewer() const noexcept {
    return program_viewer_;
  }
  [[nodiscard]] ProgramViewer* programOutputViewer() const noexcept;
  [[nodiscard]] bool isProgramFullscreen() const noexcept {
    return program_fullscreen_active_;
  }
  [[nodiscard]] ProgramViewer* sourceViewer() const noexcept {
    return source_viewer_;
  }
  [[nodiscard]] TimelineWidget* timeline() const noexcept {
    return timeline_;
  }
  [[nodiscard]] MarkerListWidget* markerList() const noexcept {
    return marker_list_;
  }
  [[nodiscard]] MediaBinWidget* mediaBin() const noexcept {
    return media_bin_;
  }
  [[nodiscard]] InspectorWidget* inspector() const noexcept {
    return inspector_;
  }
  [[nodiscard]] EffectsPanelWidget* effectsPanel() const noexcept {
    return effects_panel_;
  }
  [[nodiscard]] AudioMixerWidget* audioMixer() const noexcept {
    return audio_mixer_;
  }
  [[nodiscard]] CaptionsPanelWidget* captionsPanel() const noexcept {
    return captions_panel_;
  }
  [[nodiscard]] DeliverPanelWidget* deliverPanel() const noexcept {
    return deliver_panel_;
  }
  [[nodiscard]] ScopeWidget* scopesWidget() const noexcept {
    return scopes_widget_;
  }
  [[nodiscard]] CacheBrowserDialog* cacheBrowser() const noexcept {
    return cache_browser_;
  }
  [[nodiscard]] QSettings* settings() const noexcept {
    return settings_;
  }

  void setProjectDisplayName(const QString& displayName);
  void setProjectDirty(bool dirty);
  void setMediaItems(const QVector<MediaItemView>& items);
  void setMediaBins(const QVector<MediaBinView>& bins);
  void setTimelineView(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                       QVector<TimelineClipView> clips);
  void setTimelineView(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                       QVector<TimelineClipView> clips, QVector<TimelineMarkerView> markers,
                       QVector<TimelineGapView> gaps);
  void setSequenceTabs(const QVector<SequenceTabView>& tabs);
  struct CommandContext {
    bool hasSequence{false};
    bool hasClipSelection{false};
    bool hasSource{false};
    bool hasClipboard{false};
    bool hasAttributeClipboard{false};
    bool hasClipsAtPlayhead{false};
  };

  void setCommandContext(CommandContext context);
  [[nodiscard]] QString commandUnavailableReason(const QAction* action) const;
  void showTransientMessage(const QString& message, int timeoutMs = 4000);
  void setAudioSyncStatus(const QString& text);
  void setSequenceFormatStatus(const QString& text);
  void setJobActivitySummary(const QString& summary);
  void showExportDialog(const QString& presetId = {});
  void focusInspector();
  void refreshRecentProjectsMenu(const QStringList& paths, bool reopenLastOnStartup);

public slots:
  void setWorkspace(Workspace workspace);
  void setSourceMonitorVisible(bool visible);
  void setPrecisionTrimVisible(bool visible);
  void restoreUiState();
  void saveUiState();
  void rippleInsertFromSource();
  void overwriteInsertFromSource();
  void markSourceIn();
  void markSourceOut();
  void seekSource(qint64 position);
  void toggleProgramFullscreen();
  void exitProgramFullscreen();
  void setProgramOutputScreen(QScreen* screen);
  [[nodiscard]] bool sourceMonitorHasFocus() const;

signals:
  void workspaceChanged(Workspace workspace);
  void newProjectRequested();
  void openProjectRequested();
  void openRecentProjectRequested(const QString& path);
  void reopenLastOnStartupToggled(bool enabled);
  void saveProjectRequested();
  void saveProjectAsRequested();
  void importMediaRequested();
  void exportOtioRequested();
  void importOtioRequested();
  void manageMediaCacheRequested();
  void exportConfirmed(const QString& destination, const QString& presetId);
  void undoRequested();
  void redoRequested();
  void splitClipRequested();
  void selectAtPlayheadRequested();
  void seekPreviousEditRequested();
  void seekNextEditRequested();
  void matchFrameRequested();
  void trimHeadToPlayheadRequested();
  void trimTailToPlayheadRequested();
  void overwriteTrimHeadToPlayheadRequested();
  void overwriteTrimTailToPlayheadRequested();
  void selectForwardRequested();
  void selectForwardOnTargetedTrackRequested();
  void copyClipsRequested();
  void cutClipsRequested();
  void pasteClipsInsertRequested();
  void pasteClipsOverwriteRequested();
  void duplicateClipsRequested();
  void pasteClipAttributesRequested();
  void replaceClipMediaRequested(const QString& clipId = QString());
  void toggleLinkedSelectionRequested();
  void unlinkClipsRequested();
  void setClipEnabledRequested(bool enabled);
  void toggleSnapRequested();
  void toggleFollowPlayheadRequested();
  void zoomToSelectionRequested();
  void defaultTransitionRequested();
  void grabFrameRequested();
  void freezeFrameRequested();
  void revealMediaInFilesRequested();
  void sequenceSettingsRequested();
  void duplicateSequenceRequested();
  void markerListJumpRequested(const QString& markerId);
  void programClipInfoToggled(bool enabled);
  void sourceTimecodeToggled(bool enabled);
  void gotoTimecodeRequested();
  void toggleLoopPlaybackRequested();
  void playAroundRequested();
  void programMarkInRequested();
  void programMarkOutRequested();
  void clearProgramInRequested();
  void clearProgramOutRequested();
  void clearProgramMarksRequested();
  void deleteSelectionRequested(bool ripple);
  void playbackRateRequested(double rate);
  void seekRequested(qint64 position);
  void sourcePlaybackRateRequested(double rate);
  void sourceStepShuttleRequested(int direction);
  void sourceStepFrameRequested(int direction);
  void sourceSeekRequested(qint64 position);
  void sourceRippleInsertRequested();
  void sourceOverwriteInsertRequested();
  void sourceMarkInRequested();
  void sourceMarkOutRequested();
  void mediaActivated(const QString& mediaId);
  void mediaInsertRequested(const QString& mediaId);
  void mediaSelectionChanged(const QString& mediaId);
  void assetMetadataEdited(const AssetMetadataView& metadata);
  void effectAddRequested(const QString& effectId);
  void parameterEdited(const QString& parameterId, const QVariant& value);
  void viewerTransformPressed(const QString& handle, QPointF sequencePos);
  void viewerTransformMoved(QPointF sequencePos);
  void viewerTransformReleased();
  void programOutputPresentationReady(NativePresentationHandles handles);
  void programOutputPresentationResized(int width, int height);
  void programOutputPresentationLost();
  void programOutputClosed();
  void keyframeToggleRequested(const QString& parameterId);
  void effectParameterEdited(const QString& effectId, const QString& parameterId,
                             const QVariant& value);
  void effectKeyframeToggleRequested(const QString& effectId, const QString& parameterId);
  void effectKeyframeSelected(const QString& effectId, const QString& parameterId, qint64 time);
  void effectKeyframeValueEdited(const QString& effectId, const QString& parameterId,
                                 const QString& keyframeId, qint64 time, double value);
  void effectKeyframeInterpolationEdited(const QString& effectId, const QString& parameterId,
                                         const QString& keyframeId,
                                         KeyframeInterpolationView interpolation);
  void effectKeyframeRemoved(const QString& effectId, const QString& parameterId,
                             const QString& keyframeId);
  void effectKeyframeControlPointsEdited(const QString& effectId, const QString& parameterId,
                                         const QString& keyframeId, const QPointF& incoming,
                                         const QPointF& outgoing);
  void pickWhiteBalanceRequested();
  void effectLutBrowseRequested(const QString& effectId, const QString& parameterId);
  void sequenceActivated(const QString& sequenceId);
  void addTitleRequested();
  void nestSelectedClipsRequested();
  void transitionActivated(const QString& transitionId);
  void transitionDurationEdited(const QString& transitionId, qint64 duration);
  void transitionRemoved(const QString& transitionId);
  void transitionPresetChanged(const QString& transitionId, const QString& kind);

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void createCentralArea();
  void createPanels();
  void createActions();
  void loadShortcutOverrides();
  void showKeyboardShortcutsPreferences();
  void createMenus();
  void createToolBars();
  void createStatusBar();
  void connectControllerSurface();
  void connectScopesDock();
  void labelInteractiveChrome();
  void applyDefaultLayout(Workspace workspace);
  void updateWorkspaceActions();
  void updateWorkspaceLabel();
  void syncTimelineToolActions();
  void applyTimelineToolIcons();
  void rebuildProgramOutputMenu();
  void restoreProgramOutputScreen();
  void setShuttleRate(double rate);
  void stepShuttle(int direction);
  void addAction(const QString& id, QAction* action);
  [[nodiscard]] QString settingsKeyForWorkspace(Workspace workspace) const;
  [[nodiscard]] static QString workspaceDisplayName(Workspace workspace);
  [[nodiscard]] static QString darkStyleSheet();

  QSettings* settings_{nullptr};
  std::unique_ptr<QSettings> owned_settings_;
  Workspace workspace_{Workspace::Edit};
  bool initialized_{false};
  bool project_dirty_{false};
  QString project_display_name_{QStringLiteral("Untitled Project")};
  double shuttle_rate_{0.0};

  QHash<QString, QAction*> actions_;
  QHash<QString, QKeySequence> shortcut_defaults_;
  QHash<Workspace, QAction*> workspace_actions_;
  QHash<Workspace, QByteArray> session_layouts_;

  ProgramViewer* program_viewer_{nullptr};
  ProgramViewer* source_viewer_{nullptr};
  QSplitter* program_viewer_splitter_{nullptr};
  QWidget* program_viewer_original_parent_{nullptr};
  int program_viewer_splitter_index_{0};
  QWidget* program_fullscreen_shell_{nullptr};
  QShortcut* program_fullscreen_escape_{nullptr};
  bool program_fullscreen_active_{false};
  std::unique_ptr<ProgramOutputWindow> program_output_window_;
  QMenu* program_output_menu_{nullptr};
  QMenu* recent_projects_menu_{nullptr};
  QAction* reopen_last_on_startup_action_{nullptr};
  TimelineWidget* timeline_{nullptr};
  MarkerListWidget* marker_list_{nullptr};
  QWidget* source_container_{nullptr};
  QFrame* precision_trim_{nullptr};
  QTabBar* sequence_tab_bar_{nullptr};

  MediaBinWidget* media_bin_{nullptr};
  InspectorWidget* inspector_{nullptr};
  EffectsPanelWidget* effects_panel_{nullptr};
  AudioMixerWidget* audio_mixer_{nullptr};
  CaptionsPanelWidget* captions_panel_{nullptr};
  DeliverPanelWidget* deliver_panel_{nullptr};
  ScopeWidget* scopes_widget_{nullptr};
  CacheBrowserDialog* cache_browser_{nullptr};
  ExportDialog* export_dialog_{nullptr};

  QDockWidget* media_dock_{nullptr};
  QDockWidget* inspector_dock_{nullptr};
  QDockWidget* effects_dock_{nullptr};
  QDockWidget* mixer_dock_{nullptr};
  QDockWidget* captions_dock_{nullptr};
  QDockWidget* deliver_dock_{nullptr};
  QDockWidget* scopes_dock_{nullptr};

  CommandPalette* command_palette_{nullptr};
  CommandContext command_context_{};
  QLabel* workspace_label_{nullptr};
  QLabel* tool_label_{nullptr};
  QLabel* av_sync_label_{nullptr};
  QLabel* transport_label_{nullptr};
  QLabel* sequence_format_label_{nullptr};
  QLabel* job_activity_label_{nullptr};
  QToolBar* workspace_toolbar_{nullptr};
};

} // namespace video_editor::desktop_ui
