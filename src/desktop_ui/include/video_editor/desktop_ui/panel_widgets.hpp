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
class QProgressBar;
class QPushButton;
class QSlider;
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
  void searchChanged(const QString& query);

private slots:
  void applyFilter(const QString& query);
  void activateCurrent();

private:
  void rebuildTable();

  QLineEdit* search_{nullptr};
  QStackedWidget* content_{nullptr};
  QTableWidget* table_{nullptr};
  QVector<MediaItemView> items_;
};

class InspectorWidget final : public QWidget {
  Q_OBJECT

public:
  explicit InspectorWidget(QWidget* parent = nullptr);

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

  QLabel* selection_name_{nullptr};
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
};

class CaptionsPanelWidget final : public QWidget {
  Q_OBJECT

public:
  explicit CaptionsPanelWidget(QWidget* parent = nullptr);

  void setCaptionRows(const QStringList& timecodes, const QStringList& text);

signals:
  void transcribeRequested();
  void importCaptionsRequested();
  void exportCaptionsRequested();
  void addCaptionRequested();
  void removeCaptionRequested(int row);
  void captionTextEdited(int row, const QString& text);
  void captionActivated(int row);
  void findInTranscriptRequested(const QString& query);

private:
  QLineEdit* search_{nullptr};
  QStackedWidget* content_{nullptr};
  QTableWidget* table_{nullptr};
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
