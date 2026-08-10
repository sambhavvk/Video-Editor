// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QVariant>
#include <QWidget>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QSlider;
class QStackedWidget;
class QTableWidget;
class QToolButton;

namespace video_editor::desktop_ui {

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
  void clearSelection();

signals:
  void parameterEdited(const QString& parameterId, const QVariant& value);
  void keyframeToggleRequested(const QString& parameterId);

private:
  QLabel* selection_name_{nullptr};
  QStackedWidget* content_{nullptr};
  QFormLayout* transform_form_{nullptr};
  QWidget* visual_controls_{nullptr};
  QWidget* audio_controls_{nullptr};
  QWidget* advanced_controls_{nullptr};
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

signals:
  void gainEdited(int trackIndex, double decibels);
  void muteToggled(int trackIndex, bool muted);
  void soloToggled(int trackIndex, bool soloed);

private:
  QWidget* strips_{nullptr};
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

signals:
  void presetChanged(const QString& presetId);
  void exportRequested(const QString& presetId);

private:
  QComboBox* preset_{nullptr};
  QToolButton* export_button_{nullptr};
  QProgressBar* export_progress_{nullptr};
};

} // namespace video_editor::desktop_ui
