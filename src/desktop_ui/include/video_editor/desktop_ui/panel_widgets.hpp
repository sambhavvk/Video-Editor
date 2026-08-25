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

namespace video_editor::desktop_ui {

class KeyframeCurveWidget;

class MediaBinWidget final : public QWidget {
  Q_OBJECT

public:
  explicit MediaBinWidget(QWidget* parent = nullptr);

  void setItems(const QVector<MediaItemView>& items);
  [[nodiscard]] const QVector<MediaItemView>& items() const noexcept {
    return items_;
  }

signals:
  void importRequested();
  void relinkRequested(const QString& mediaId);
  void proxyRequested(const QString& mediaId);
  void mediaActivated(const QString& mediaId);
  void mediaSelectionChanged(const QString& mediaId);
  void searchChanged(const QString& query);

private slots:
  void applyFilter(const QString& query);
  void activateCurrent();

private:
  void rebuildTable();
  void emitCurrentMediaSelection();
  [[nodiscard]] QString mediaIdAtRow(int row) const;

  QLineEdit* search_{nullptr};
  QStackedWidget* content_{nullptr};
  QTableWidget* table_{nullptr};
  QVector<MediaItemView> items_;
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
  void addTitleRequested();

private:
  void rebuildEffectParameterFields();
  void selectEffectParameter(int index);
  void selectKeyframe(int index);
  void refreshKeyframeEditor();
  void publishAssetMetadata();

  QLabel* selection_name_{nullptr};
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
  void setTranscriptionState(TranscriptionState state, const QString& message = {},
                             int percent = 0);
  void setModelDownloadState(const ModelDownloadView& state);
  void setTranscriptionOptions(const TranscriptionOptionsView& options);
  void setCaptionStyle(const CaptionStyleView& style);
  void setReviewProposals(const QVector<CaptionProposalView>& proposals);

signals:
  void importCaptionsRequested();
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
  QVector<CaptionRowView> rows_;
  QVector<CaptionProposalView> proposals_;
  TranscriptionState transcription_state_{TranscriptionState::Idle};
  QString model_id_{QStringLiteral("base")};
  QColor text_color_value_{Qt::white};
  QColor background_color_value_{0, 0, 0, 178};
  QColor outline_color_value_{Qt::black};
};

class DeliverPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit DeliverPanelWidget(QWidget* parent = nullptr);

  [[nodiscard]] QString selectedPresetId() const;
  void setExportEnabled(bool enabled);
  void setExportRunning(bool running, int percent = 0);
  void loadPlatformPresets();
  void setEncoderCapabilities(const QString& summary);
  void setDestinationPath(const QString& path);
  [[nodiscard]] QString destinationPath() const;
  [[nodiscard]] QString captionModeKey() const;
  [[nodiscard]] QString sidecarFormatKey() const;
  [[nodiscard]] int overrideWidth() const;
  [[nodiscard]] int overrideHeight() const;
  [[nodiscard]] unsigned int overrideFrameRateNum() const;
  [[nodiscard]] unsigned int overrideFrameRateDen() const;
  [[nodiscard]] unsigned int overrideAudioBitrate() const;
  [[nodiscard]] std::uint64_t overrideVideoBitrate() const;
  [[nodiscard]] std::optional<int> overrideVideoQuality() const;
  [[nodiscard]] bool preferHardwareEncoder() const;

signals:
  void presetChanged(const QString& presetId);
  void exportRequested(const QString& presetId);
  void destinationBrowseRequested();
  void cancelRequested();

private:
  QComboBox* preset_{nullptr};
  QToolButton* export_button_{nullptr};
  QProgressBar* export_progress_{nullptr};
  QLineEdit* destination_{nullptr};
  QToolButton* browse_button_{nullptr};
  QComboBox* resolution_{nullptr};
  QComboBox* frame_rate_{nullptr};
  QComboBox* video_bitrate_{nullptr};
  QComboBox* video_quality_{nullptr};
  QCheckBox* hardware_encoder_{nullptr};
  QComboBox* audio_bitrate_{nullptr};
  QComboBox* caption_mode_{nullptr};
  QComboBox* sidecar_format_{nullptr};
  QLabel* encoder_summary_{nullptr};
  QLabel* preset_notes_{nullptr};
  bool export_enabled_{false};
  bool selected_preset_available_{true};
  bool hardware_vp9_available_{false};
};

} // namespace video_editor::desktop_ui
