// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"
#include <QPointF>
#include <QVariant>
#include <QWidget>

#include <cstdint>
#include <optional>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;
class QListWidget;

namespace video_editor::desktop_ui {

class KeyframeCurveWidget;

class MediaBinWidget final : public QWidget {
  Q_OBJECT

public:
  explicit MediaBinWidget(QWidget* parent = nullptr);

  void setBins(const QVector<MediaBinView>& bins);
  void setItems(const QVector<MediaItemView>& items);
  [[nodiscard]] const QVector<MediaItemView>& items() const noexcept {
    return items_;
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

signals:
  void importRequested();
  void insertRequested(const QString& mediaId);
  void relinkRequested(const QString& mediaId);
  void proxyRequested(const QString& mediaId);
  void mediaActivated(const QString& mediaId);
  void mediaSelectionChanged(const QString& mediaId);
  void searchChanged(const QString& query);
  void createBinRequested(const QString& parentBinId);
  void renameBinRequested(const QString& binId, const QString& name);
  void moveBinRequested(const QString& binId, const QString& parentBinId);
  void removeBinRequested(const QString& binId);
  void setAssetBinRequested(const QString& assetId, const QString& binId);
  void revealInFilesRequested(const QString& mediaId);
  void hoverScrubRequested(const QString& mediaId, double normalizedPosition);

private slots:
  void applyFilter(const QString& query);
  void activateCurrent();
  void handleBinSelectionChanged();
  void toggleViewMode();

private:
  void rebuildTree();
  void rebuildTable();
  void rebuildIconView();
  void emitCurrentMediaSelection();
  [[nodiscard]] QString mediaIdAtRow(int row) const;
  [[nodiscard]] bool itemMatchesSelectedBin(const MediaItemView& item) const;
  [[nodiscard]] bool itemMatchesSearch(const MediaItemView& item, const QString& query) const;
  [[nodiscard]] QString selectedBinId() const;
  [[nodiscard]] QVector<MediaItemView> filteredItems() const;

  QLineEdit* search_{nullptr};
  QToolButton* view_mode_{nullptr};
  QSplitter* splitter_{nullptr};
  QTreeWidget* bin_tree_{nullptr};
  QStackedWidget* content_{nullptr};
  QTableWidget* table_{nullptr};
  QListWidget* icon_view_{nullptr};
  QVector<MediaBinView> bins_;
  QVector<MediaItemView> items_;
  bool icon_view_mode_{false};
};

class MarkerListWidget final : public QWidget {
  Q_OBJECT

public:
  explicit MarkerListWidget(QWidget* parent = nullptr);

  void setMarkers(const QVector<TimelineMarkerView>& markers);

signals:
  void markerActivated(const QString& markerId);
  void markerRenameRequested(const QString& markerId, const QString& name);

private:
  QListWidget* list_{nullptr};
};

class TrackNavWidget final : public QWidget {
  Q_OBJECT

public:
  explicit TrackNavWidget(QWidget* parent = nullptr);

  void setTracks(const QVector<TimelineTrackView>& tracks);
  void setActiveTrackId(const QString& trackId);
  void setRestoreAvailable(bool available);

signals:
  void trackActivated(const QString& trackId);
  void visibilityPresetRequested(TrackVisibilityPreset preset);
  void visibilityRestoreRequested();
  void trackHeightPresetRequested(TrackHeightPreset preset);

private:
  void applyFilter(const QString& query);
  [[nodiscard]] bool trackMatchesQuery(const TimelineTrackView& track, const QString& query) const;

  QLineEdit* search_{nullptr};
  QComboBox* visibility_preset_{nullptr};
  QPushButton* restore_visibility_{nullptr};
  QComboBox* height_preset_{nullptr};
  QListWidget* list_{nullptr};
  QVector<TimelineTrackView> tracks_;
  QString active_track_id_;
};

class ColorWheelWidget final : public QWidget {
  Q_OBJECT

public:
  enum class Role { Lift, Gamma, Gain };
  Q_ENUM(Role)

  explicit ColorWheelWidget(Role role, QWidget* parent = nullptr);

  void setRgb(double red, double green, double blue);
  [[nodiscard]] double red() const noexcept {
    return red_;
  }
  [[nodiscard]] double green() const noexcept {
    return green_;
  }
  [[nodiscard]] double blue() const noexcept {
    return blue_;
  }
  [[nodiscard]] Role role() const noexcept {
    return role_;
  }

  [[nodiscard]] QSize sizeHint() const override;

signals:
  void rgbChanged(double red, double green, double blue);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  void applyWheelPosition(const QPoint& position);
  void publish();
  [[nodiscard]] QRect discRect() const;
  [[nodiscard]] QRect masterRect() const;
  [[nodiscard]] double masterValue() const;
  void setMasterValue(double master);

  Role role_{Role::Lift};
  double red_{0.0};
  double green_{0.0};
  double blue_{0.0};
  bool dragging_disc_{false};
  bool dragging_master_{false};
};

class InspectorWidget final : public QWidget {
  Q_OBJECT

public:
  explicit InspectorWidget(QWidget* parent = nullptr);

  void setAssetMetadata(const AssetMetadataView& metadata);
  void clearAssetMetadata();

public slots:
  void setSelectionName(const QString& name);
  void setClipCapabilities(bool visual, bool audio);
  void setParameter(const QString& parameterId, const QVariant& value);
  void setEffectParameters(const QVector<EffectParameterView>& parameters);
  // Title clips show the title controls group; media clips show the speed group.
  void setTitleControlsVisible(bool visible);
  void setSpeedControlsVisible(bool visible);
  void setLinkedAvSync(const QString& statusText, bool canResync);
  void clearSelection();

signals:
  void assetMetadataEdited(const AssetMetadataView& metadata);
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
  void deleteClipRequested();
  void resyncLinkedAvRequested();
  void pickWhiteBalanceRequested();
  void effectLutBrowseRequested(const QString& effectId, const QString& parameterId);
  void addTitleRequested();

private:
  void rebuildEffectParameterFields();
  void selectEffectParameter(int index);
  void selectKeyframe(int index);
  void refreshKeyframeEditor();
  void publishAssetMetadata();
  void syncColorWheels();
  void publishColorWheel(ColorWheelWidget* wheel);

  QLabel* selection_name_{nullptr};
  QPushButton* delete_clip_{nullptr};
  QGroupBox* linked_sync_group_{nullptr};
  QLabel* linked_sync_status_{nullptr};
  QPushButton* resync_linked_av_{nullptr};
  QGroupBox* asset_group_{nullptr};
  QLineEdit* asset_title_{nullptr};
  QLineEdit* asset_tags_{nullptr};
  QPlainTextEdit* asset_notes_{nullptr};
  QSpinBox* asset_rating_{nullptr};
  AssetMetadataView asset_metadata_{};
  QStackedWidget* content_{nullptr};
  QFormLayout* transform_form_{nullptr};
  QWidget* visual_controls_{nullptr};
  QWidget* audio_controls_{nullptr};
  QWidget* advanced_controls_{nullptr};
  QWidget* title_controls_{nullptr};
  QWidget* speed_controls_{nullptr};
  QGroupBox* effects_controls_{nullptr};
  QWidget* color_wheels_{nullptr};
  ColorWheelWidget* lift_wheel_{nullptr};
  ColorWheelWidget* gamma_wheel_{nullptr};
  ColorWheelWidget* gain_wheel_{nullptr};
  QPushButton* pick_white_balance_{nullptr};
  QWidget* effect_parameter_editor_{nullptr};
  QFormLayout* effect_parameter_form_{nullptr};
  QComboBox* effect_parameter_selector_{nullptr};
  QListWidget* keyframe_list_{nullptr};
  QDoubleSpinBox* keyframe_time_{nullptr};
  QDoubleSpinBox* keyframe_value_{nullptr};
  QComboBox* keyframe_interpolation_{nullptr};
  QToolButton* keyframe_delete_{nullptr};
  KeyframeCurveWidget* keyframe_curve_{nullptr};
  QVector<EffectParameterView> effect_parameters_;
  int active_effect_parameter_{-1};
  int active_keyframe_{-1};
};

class EffectsPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit EffectsPanelWidget(QWidget* parent = nullptr);

  void setEffects(const QVector<EffectView>& effects);

signals:
  void effectActivated(const QString& effectId);
  void effectAddRequested(const QString& effectId);

private slots:
  void applyFilter(const QString& query);
  void activateCurrent();

private:
  void rebuild();

  QLineEdit* search_{nullptr};
  QListWidget* list_{nullptr};
  QVector<EffectView> effects_;
};

class AudioMixerWidget final : public QWidget {
  Q_OBJECT

public:
  explicit AudioMixerWidget(QWidget* parent = nullptr);
  void setTracks(const QVector<AudioTrackView>& tracks);
  void setTrackNames(const QStringList& names);
  // Push a live level reading for one strip. Called by the controller from its
  // playback poll. Values are dBFS peak per channel (negative or zero).
  void setMeterLevels(int trackIndex, const QVector<float>& peakDbfs);
  void setTrackMeters(const QVector<AudioTrackMeterView>& meters);
  void setMasterMeter(float peakDbfs, float rmsDbfs, double lufs, bool active,
                      bool lufsValid = false, bool lufsStale = true);
  void setOutputDevices(const QStringList& ids, const QStringList& names, const QString& selectedId,
                        bool available, const QString& status = {});
  void setCalibratedLatencyFrames(std::optional<std::uint64_t> frames);
  void setCalibrationBusy(bool busy);
  void setSyncDiagnostics(std::uint64_t xrunCount, double uncertaintyMs, double estimatedErrorMs,
                          int bufferSize, bool adaptive);
  void setBufferSize(int bufferSize);
  [[nodiscard]] int bufferSize() const;
  void setNormalizationReview(double measuredLufs, double gainDb, double targetLufs);
  void setNormalizationBusy(bool busy);
  void setNormalizationStatus(const QString& status);
  [[nodiscard]] double normalizationTargetLufs() const;
  void setNormalizationTargetLufs(double targetLufs);

signals:
  void gainEdited(int trackIndex, double decibels);
  void panEdited(int trackIndex, double pan);
  void muteToggled(int trackIndex, bool muted);
  void soloToggled(int trackIndex, bool soloed);
  void trackEffectAddRequested(int trackIndex, const QString& effectType);
  void trackEffectRemoveRequested(int trackIndex, const QString& effectId);
  void trackEffectParameterEdited(int trackIndex, const QString& effectId,
                                  const QString& parameterId, const QVariant& value);
  void outputDeviceSelected(const QString& deviceId);
  void calibrateOutputLatencyRequested();
  void bufferSizeChanged(int bufferSize);
  void normalizationAnalyzeRequested();
  void normalizationApplyRequested();
  void normalizationTargetChanged(double targetLufs);

private:
  [[nodiscard]] bool canUpdateStripsInPlace(const QVector<AudioTrackView>& tracks) const;
  void updateStripsInPlace(const QVector<AudioTrackView>& tracks);
  void rebuildStrips(const QVector<AudioTrackView>& tracks);
  void buildStrips(const QVector<AudioTrackView>& tracks);
  void discardStripWidgets();

  QWidget* strips_{nullptr};
  QLabel* master_peak_{nullptr};
  QLabel* master_rms_{nullptr};
  QLabel* master_lufs_{nullptr};
  QLabel* device_status_{nullptr};
  QComboBox* device_selector_{nullptr};
  QPushButton* calibrate_latency_{nullptr};
  QLabel* calibrated_latency_label_{nullptr};
  QComboBox* buffer_size_{nullptr};
  QLabel* sync_status_{nullptr};
  QLabel* normalization_status_{nullptr};
  QPushButton* normalization_analyze_{nullptr};
  QPushButton* normalization_apply_{nullptr};
  QDoubleSpinBox* normalization_target_{nullptr};
  QVector<AudioTrackView> tracks_;
};

class CaptionsPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit CaptionsPanelWidget(QWidget* parent = nullptr);

  void setCaptionRows(const QStringList& timecodes, const QStringList& text);
  void setCaptionRows(const QVector<CaptionRowView>& rows);
  void setTranscriptPlayhead(qint64 position);
  void setTranscriptionState(TranscriptionState state, const QString& message = {},
                             int percent = 0);
  void setModelDownloadState(const ModelDownloadView& state);
  void setTranscriptionOptions(const TranscriptionOptionsView& options);
  void setCaptionStyle(const CaptionStyleView& style);
  void setReviewProposals(const QVector<CaptionProposalView>& proposals);
  [[nodiscard]] TranscriptionState transcriptionState() const noexcept {
    return transcription_state_;
  }

signals:
  void importCaptionsRequested();
  void extractEmbeddedCaptionsRequested();
  void exportCaptionsRequested();
  void addCaptionRequested();
  void removeCaptionRequested(int row);
  void captionTextEdited(int row, const QString& text);
  void captionActivated(int row);
  void findInTranscriptRequested(const QString& query);
  void captionIdActivated(const QString& captionId, qint64 start);
  void wordActivated(const QString& wordId, qint64 start);
  void transcriptionOptionsChanged(const TranscriptionOptionsView& options);
  void downloadModelRequested(const QString& modelId);
  void cancelTranscriptionRequested();
  void transcribeWithOptionsRequested(const TranscriptionOptionsView& options);
  void captionTimingEdited(const QString& captionId, qint64 start, qint64 end);
  void captionStyleEdited(const QString& captionId, const CaptionStyleView& style);
  void reviewProposalToggled(const QString& proposalId, bool selected);
  void applyReviewRequested();
  void discardReviewRequested();

private:
  void updateWordList(int row);
  void refreshTranscriptTable();
  void scrollToActiveRow();
  [[nodiscard]] static QString captionHtml(const CaptionRowView& row);
  [[nodiscard]] static QString uncertainWordLabel(const CaptionWordView& word);
  void updateStyleControls(const CaptionStyleView& style);
  CaptionStyleView styleFromControls() const;
  TranscriptionOptionsView optionsFromControls() const;
  void updateStateControls();

  QLineEdit* search_{nullptr};
  QStackedWidget* content_{nullptr};
  QTableWidget* table_{nullptr};
  QComboBox* language_{nullptr};
  QCheckBox* translate_{nullptr};
  QCheckBox* prefer_vulkan_{nullptr};
  QCheckBox* word_timestamps_{nullptr};
  QSpinBox* thread_count_{nullptr};
  QLabel* transcription_status_{nullptr};
  QProgressBar* transcription_progress_{nullptr};
  QPushButton* model_download_{nullptr};
  QPushButton* transcribe_{nullptr};
  QPushButton* transcribe_cancel_{nullptr};
  QListWidget* words_{nullptr};
  QLineEdit* font_family_{nullptr};
  QDoubleSpinBox* font_size_{nullptr};
  QComboBox* alignment_{nullptr};
  QDoubleSpinBox* vertical_position_{nullptr};
  QDoubleSpinBox* safe_margin_{nullptr};
  QDoubleSpinBox* outline_width_{nullptr};
  QPushButton* text_color_{nullptr};
  QPushButton* background_color_{nullptr};
  QPushButton* outline_color_{nullptr};
  QCheckBox* style_bold_{nullptr};
  QCheckBox* style_italic_{nullptr};
  QLabel* style_preview_{nullptr};
  QListWidget* review_{nullptr};
  QPushButton* apply_review_{nullptr};
  QPushButton* discard_review_{nullptr};
  QLabel* spelling_hint_{nullptr};
  QVector<CaptionRowView> rows_;
  QVector<CaptionProposalView> proposals_;
  qint64 transcript_playhead_{0};
  TranscriptionState transcription_state_{TranscriptionState::Idle};
  QString model_id_{QStringLiteral("base")};
  QColor text_color_value_{Qt::white};
  QColor background_color_value_{0, 0, 0, 178};
  QColor outline_color_value_{Qt::black};
};

class ProjectHealthPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit ProjectHealthPanelWidget(QWidget* parent = nullptr);

  void setIssues(const QVector<ProjectHealthIssueView>& issues);
  [[nodiscard]] QString selectedIssueId() const;

signals:
  void repairRequested(const QString& issueId);
  void refreshRequested();

private:
  QListWidget* list_{nullptr};
  QPushButton* repair_{nullptr};
};

class DeliverPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit DeliverPanelWidget(QWidget* parent = nullptr);

  [[nodiscard]] QString selectedPresetId() const;
  void setExportEnabled(bool enabled);
  [[nodiscard]] bool exportReady() const noexcept;
  [[nodiscard]] QString exportUnavailableReason() const;
  void setExportRunning(bool running, int percent = 0);
  void setExportJobs(const QVector<ExportJobView>& jobs);
  void loadPlatformPresets();
  void setEncoderCapabilities(const QString& summary);
  [[nodiscard]] QString deliveryOverviewText() const;
  [[nodiscard]] DeliveryRecipeSnapshot captureRecipeSnapshot() const;
  void applyRecipeSnapshot(const DeliveryRecipeSnapshot& snapshot);
  void setSavedRecipes(const QVector<QPair<QString, QString>>& recipes);
  [[nodiscard]] QString selectedRecipeId() const;
  void setDestinationPath(const QString& path);
  [[nodiscard]] QString destinationPath() const;
  [[nodiscard]] QString captionModeKey() const;
  [[nodiscard]] QString creatorVideoCodecKey() const;
  [[nodiscard]] QString sidecarFormatKey() const;
  [[nodiscard]] int overrideWidth() const;
  [[nodiscard]] int overrideHeight() const;
  [[nodiscard]] unsigned int overrideFrameRateNum() const;
  [[nodiscard]] unsigned int overrideFrameRateDen() const;
  [[nodiscard]] unsigned int overrideAudioBitrate() const;
  [[nodiscard]] std::uint64_t overrideVideoBitrate() const;
  [[nodiscard]] std::optional<int> overrideVideoQuality() const;
  [[nodiscard]] bool preferHardwareEncoder() const;
  [[nodiscard]] bool useExportRange() const;

signals:
  void presetChanged(const QString& presetId);
  void exportRequested(const QString& presetId);
  void destinationBrowseRequested();
  void cancelRequested();
  void cancelQueuedExportRequested(const QString& jobId);
  void saveDeliveryRecipeRequested(const QString& name);
  void queueDeliveryRecipeRequested(const QString& recipeId);

private:
  void refreshExportJobSelection();
  void refreshExportButtonAffordance();
  void refreshDeliveryOverview();
  QComboBox* preset_{nullptr};
  QToolButton* export_button_{nullptr};
  QProgressBar* export_progress_{nullptr};
  QListWidget* export_job_list_{nullptr};
  QPushButton* remove_queued_export_{nullptr};
  QLineEdit* destination_{nullptr};
  QToolButton* browse_button_{nullptr};
  QComboBox* resolution_{nullptr};
  QComboBox* frame_rate_{nullptr};
  QComboBox* video_bitrate_{nullptr};
  QComboBox* video_quality_{nullptr};
  QComboBox* video_codec_{nullptr};
  QCheckBox* hardware_encoder_{nullptr};
  QComboBox* audio_bitrate_{nullptr};
  QComboBox* caption_mode_{nullptr};
  QComboBox* sidecar_format_{nullptr};
  QCheckBox* use_export_range_{nullptr};
  QLabel* encoder_summary_{nullptr};
  QLabel* delivery_overview_{nullptr};
  QLabel* preset_notes_{nullptr};
  QComboBox* saved_recipes_{nullptr};
  QPushButton* save_recipe_{nullptr};
  QPushButton* queue_recipe_{nullptr};
  bool export_enabled_{false};
  bool selected_preset_available_{true};
  bool hardware_vp9_available_{false};
  bool hardware_av1_available_{false};
};

} // namespace video_editor::desktop_ui
