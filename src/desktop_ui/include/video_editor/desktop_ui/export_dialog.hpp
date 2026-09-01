// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace video_editor::desktop_ui {

class ExportDialog final : public QDialog {
  Q_OBJECT

public:
  explicit ExportDialog(QWidget* parent = nullptr);

  void loadPlatformPresets();
  void setSelectedPreset(const QString& presetId);
  void setDestinationPath(const QString& path);
  [[nodiscard]] QString selectedPresetId() const;
  [[nodiscard]] QString destinationPath() const;

protected:
  void accept() override;

private:
  void browseDestination();
  void updatePresetNotes();
  void updateAcceptEnabled();
  [[nodiscard]] QString fileExtensionForCurrentPreset() const;
  [[nodiscard]] QString saveFileFilterForCurrentPreset() const;

  QComboBox* preset_{nullptr};
  QLineEdit* destination_{nullptr};
  QPushButton* browse_{nullptr};
  QLabel* preset_notes_{nullptr};
  QPushButton* ok_button_{nullptr};
  bool selected_preset_available_{true};
};

} // namespace video_editor::desktop_ui
