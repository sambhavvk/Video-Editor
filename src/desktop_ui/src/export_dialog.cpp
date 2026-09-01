// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/export_dialog.hpp"

#include "video_editor/export_service/presets.h"
#include "video_editor/media_codec/encoder_capabilities.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace video_editor::desktop_ui {
namespace {

[[nodiscard]] QString extensionForPlatform(const export_service::PlatformPreset platform) {
  const auto video = export_service::reference_video_preset_for(platform).value_or(
      export_service::VideoPreset::Ffv1Matroska);
  if (platform == export_service::PlatformPreset::PodcastAudioOnly) {
    return QStringLiteral("webm");
  }
  if (video == export_service::VideoPreset::Vp9OpusWebm) {
    return QStringLiteral("webm");
  }
  if (video == export_service::VideoPreset::ProRes422HqMov) {
    return QStringLiteral("mov");
  }
  return QStringLiteral("mkv");
}

[[nodiscard]] QString saveFilterForExtension(const QString& extension) {
  if (extension == QStringLiteral("webm")) {
    return QObject::tr("WebM video (*.webm)");
  }
  if (extension == QStringLiteral("mov")) {
    return QObject::tr("QuickTime movie (*.mov)");
  }
  return QObject::tr("Matroska video (*.mkv)");
}

} // namespace

ExportDialog::ExportDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("exportDialog"));
  setAccessibleName(tr("Export video"));
  setWindowTitle(tr("Export Video"));
  setModal(true);
  resize(520, 220);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  auto* form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  preset_ = new QComboBox(this);
  preset_->setObjectName(QStringLiteral("exportPreset"));
  preset_->setAccessibleName(tr("Export preset"));
  form->addRow(tr("Format"), preset_);

  auto* destination_row = new QWidget(this);
  auto* destination_layout = new QHBoxLayout(destination_row);
  destination_layout->setContentsMargins(0, 0, 0, 0);
  destination_layout->setSpacing(6);
  destination_ = new QLineEdit(destination_row);
  destination_->setObjectName(QStringLiteral("destinationField"));
  destination_->setAccessibleName(tr("Export destination"));
  destination_->setPlaceholderText(tr("Choose a destination file"));
  browse_ = new QPushButton(tr("Browse…"), destination_row);
  browse_->setObjectName(QStringLiteral("browseButton"));
  browse_->setAccessibleName(tr("Browse export destination"));
  destination_layout->addWidget(destination_, 1);
  destination_layout->addWidget(browse_);
  form->addRow(tr("Destination"), destination_row);
  layout->addLayout(form);

  preset_notes_ = new QLabel(this);
  preset_notes_->setObjectName(QStringLiteral("presetNotes"));
  preset_notes_->setAccessibleName(tr("Preset notes"));
  preset_notes_->setWordWrap(true);
  preset_notes_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  layout->addWidget(preset_notes_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->setObjectName(QStringLiteral("exportDialogButtons"));
  ok_button_ = buttons->button(QDialogButtonBox::Ok);
  ok_button_->setObjectName(QStringLiteral("exportOkButton"));
  buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("exportCancelButton"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(browse_, &QPushButton::clicked, this, &ExportDialog::browseDestination);
  connect(preset_, &QComboBox::currentIndexChanged, this, &ExportDialog::updatePresetNotes);
  connect(destination_, &QLineEdit::textChanged, this, &ExportDialog::updateAcceptEnabled);

  loadPlatformPresets();
}

void ExportDialog::loadPlatformPresets() {
  preset_->clear();
  const auto presets = export_service::available_platform_presets();
  const auto software_matrix = media::probe_encoder_capabilities(false);
  for (const auto& info : presets) {
    const auto required_video =
        info.intended_video_codec == "vp9"
            ? media::DeliveryCodec::Vp9
            : (info.intended_video_codec == "ffv1" ? media::DeliveryCodec::Ffv1
                                                   : media::DeliveryCodec::ProRes);
    const bool video_available =
        info.audio_only || media::best_encoder_for(software_matrix, required_video).has_value();
    const bool audio_available =
        info.intended_audio_codec != "opus" ||
        media::best_encoder_for(software_matrix, media::DeliveryCodec::Opus).has_value();
    const bool available = video_available && audio_available;
    const int index = preset_->count();
    preset_->addItem(QString::fromStdString(info.display_name),
                     QString::number(static_cast<int>(info.preset)));
    preset_->setItemData(index, available, Qt::UserRole - 1);
  }
  if (preset_->count() > 0) {
    preset_->setCurrentIndex(0);
  }
  updatePresetNotes();
}

void ExportDialog::setSelectedPreset(const QString& presetId) {
  if (presetId.isEmpty()) {
    return;
  }
  const int index = preset_->findData(presetId);
  if (index >= 0) {
    preset_->setCurrentIndex(index);
  }
}

void ExportDialog::setDestinationPath(const QString& path) {
  destination_->setText(path);
}

QString ExportDialog::selectedPresetId() const {
  return preset_->currentData().toString();
}

QString ExportDialog::destinationPath() const {
  return destination_->text().trimmed();
}

void ExportDialog::browseDestination() {
  const QString extension = fileExtensionForCurrentPreset();
  const QString filter = saveFileFilterForCurrentPreset();
  const QString default_name = extension == QStringLiteral("webm") &&
                                       selectedPresetId().toInt() ==
                                           static_cast<int>(
                                               export_service::PlatformPreset::PodcastAudioOnly)
                                   ? QStringLiteral("podcast.webm")
                                   : QStringLiteral("export.%1").arg(extension);
  QString initial = destination_->text().trimmed();
  if (initial.isEmpty()) {
    initial = default_name;
  }
  const QString chosen =
      QFileDialog::getSaveFileName(this, tr("Choose export destination"), initial, filter);
  if (!chosen.isEmpty()) {
    destination_->setText(chosen);
  }
}

void ExportDialog::updatePresetNotes() {
  bool numeric = false;
  const int value = preset_->currentData().toInt(&numeric);
  if (!numeric) {
    preset_notes_->clear();
    selected_preset_available_ = false;
    updateAcceptEnabled();
    return;
  }
  const auto info = export_service::platform_preset_info(
      static_cast<export_service::PlatformPreset>(value));
  const bool available = preset_->itemData(preset_->currentIndex(), Qt::UserRole - 1).toBool();
  selected_preset_available_ = available;
  preset_notes_->setText(
      available ? QString::fromStdString(info.notes)
                : tr("Unavailable: the required FOSS encoder is not present in this build."));
  updateAcceptEnabled();
}

void ExportDialog::updateAcceptEnabled() {
  if (ok_button_ != nullptr) {
    ok_button_->setEnabled(selected_preset_available_ && !destinationPath().isEmpty());
  }
}

QString ExportDialog::fileExtensionForCurrentPreset() const {
  bool numeric = false;
  const int value = preset_->currentData().toInt(&numeric);
  if (!numeric) {
    return QStringLiteral("mkv");
  }
  return extensionForPlatform(static_cast<export_service::PlatformPreset>(value));
}

QString ExportDialog::saveFileFilterForCurrentPreset() const {
  return saveFilterForExtension(fileExtensionForCurrentPreset());
}

void ExportDialog::accept() {
  if (!selected_preset_available_ || destinationPath().isEmpty()) {
    return;
  }
  QDialog::accept();
}

} // namespace video_editor::desktop_ui
