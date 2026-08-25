// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/cache_browser_dialog.hpp"
#include "video_editor/desktop_ui/ui_types.hpp"

#include <QHash>
#include <QMainWindow>
#include <memory>

class QAction;
class QCloseEvent;
class QDockWidget;
class QFrame;
class QLabel;
class QSettings;
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
class ProgramViewer;
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
  [[nodiscard]] ProgramViewer* programViewer() const noexcept {
    return program_viewer_;
  }
  [[nodiscard]] ProgramViewer* sourceViewer() const noexcept {
    return source_viewer_;
  }
  [[nodiscard]] TimelineWidget* timeline() const noexcept {
    return timeline_;
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
  [[nodiscard]] CacheBrowserDialog* cacheBrowser() const noexcept {
    return cache_browser_;
  }

  void setProjectDisplayName(const QString& displayName);
  void setProjectDirty(bool dirty);
  void setMediaItems(const QVector<MediaItemView>& items);
  void setTimelineView(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                       QVector<TimelineClipView> clips);
  void setTimelineView(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                       QVector<TimelineClipView> clips, QVector<TimelineMarkerView> markers,
                       QVector<TimelineGapView> gaps);
  void showTransientMessage(const QString& message, int timeoutMs = 4000);

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

signals:
  void workspaceChanged(Workspace workspace);
  void newProjectRequested();
  void openProjectRequested();
  void saveProjectRequested();
  void saveProjectAsRequested();
  void importMediaRequested();
  void manageMediaCacheRequested();
  void exportRequested(const QString& presetId);
  void undoRequested();
  void redoRequested();
  void splitClipRequested();
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
  void mediaSelectionChanged(const QString& mediaId);
  void assetMetadataEdited(const AssetMetadataView& metadata);
  void effectAddRequested(const QString& effectId);
  void parameterEdited(const QString& parameterId, const QVariant& value);
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
  void addTitleRequested();
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
  void createMenus();
  void createToolBars();
  void createStatusBar();
  void connectControllerSurface();
  void labelInteractiveChrome();
  void applyDefaultLayout(Workspace workspace);
  void updateWorkspaceActions();
  void updateWorkspaceLabel();
  void setShuttleRate(double rate);
  void stepShuttle(int direction);
  [[nodiscard]] bool sourceMonitorHasFocus() const;
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
  QHash<Workspace, QAction*> workspace_actions_;
  QHash<Workspace, QByteArray> session_layouts_;

  ProgramViewer* program_viewer_{nullptr};
  ProgramViewer* source_viewer_{nullptr};
  TimelineWidget* timeline_{nullptr};
  QWidget* source_container_{nullptr};
  QFrame* precision_trim_{nullptr};

  MediaBinWidget* media_bin_{nullptr};
  InspectorWidget* inspector_{nullptr};
  EffectsPanelWidget* effects_panel_{nullptr};
  AudioMixerWidget* audio_mixer_{nullptr};
  CaptionsPanelWidget* captions_panel_{nullptr};
  DeliverPanelWidget* deliver_panel_{nullptr};
  CacheBrowserDialog* cache_browser_{nullptr};

  QDockWidget* media_dock_{nullptr};
  QDockWidget* inspector_dock_{nullptr};
  QDockWidget* effects_dock_{nullptr};
  QDockWidget* mixer_dock_{nullptr};
  QDockWidget* captions_dock_{nullptr};
  QDockWidget* deliver_dock_{nullptr};

  CommandPalette* command_palette_{nullptr};
  QLabel* workspace_label_{nullptr};
  QLabel* transport_label_{nullptr};
  QToolBar* workspace_toolbar_{nullptr};
};

} // namespace video_editor::desktop_ui
