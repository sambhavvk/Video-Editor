// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/keyframe_curve_widget.hpp"

#include "video_editor/export_service/presets.h"
#include "video_editor/media_codec/encoder_capabilities.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace video_editor::desktop_ui {
namespace {

constexpr int kMediaIconColumn = 0;
constexpr int kMediaNameColumn = 1;
constexpr int kMediaFormatColumn = 3;
constexpr int kMediaStatusColumn = 4;

QStringList splitCommaSeparatedTags(const QString& text) {
  const auto parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
  QStringList tags;
  tags.reserve(parts.size());
  for (const auto& part : parts) {
    const auto trimmed = part.trimmed();
    if (!trimmed.isEmpty()) {
      tags.push_back(trimmed);
    }
  }
  return tags;
}

QLabel* makeMutedLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setAlignment(Qt::AlignCenter);
  label->setProperty("muted", true);
  return label;
}

QWidget* makeInspectorField(const QString& id, const QString& suffix, double minimum,
                            double maximum, double value, double step, QWidget* parent,
                            const std::function<void(double)>& changed) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);

  auto* spin = new QDoubleSpinBox(row);
  spin->setObjectName(QStringLiteral("inspector.%1").arg(id));
  spin->setAccessibleName(id);
  spin->setRange(minimum, maximum);
  spin->setValue(value);
  spin->setSingleStep(step);
  spin->setDecimals(2);
  spin->setSuffix(suffix);
  spin->setKeyboardTracking(false);
  layout->addWidget(spin, 1);

  auto* keyframe = new QToolButton(row);
  keyframe->setObjectName(QStringLiteral("keyframe.%1").arg(id));
  keyframe->setAccessibleName(QObject::tr("Toggle keyframe for %1").arg(id));
  keyframe->setText(QStringLiteral("◆"));
  keyframe->setToolTip(QObject::tr("Toggle keyframe"));
  keyframe->setCheckable(true);
  layout->addWidget(keyframe);

  QObject::connect(spin, &QDoubleSpinBox::valueChanged, row, changed);
  return row;
}

} // namespace

MediaBinWidget::MediaBinWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("mediaBinPanel"));
  setAccessibleName(tr("Media bin"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(8);

  auto* tools = new QHBoxLayout;
  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("mediaSearch"));
  search_->setAccessibleName(tr("Search media"));
  search_->setPlaceholderText(tr("Search media…"));
  search_->setClearButtonEnabled(true);
  search_->setFocusPolicy(Qt::StrongFocus);
  tools->addWidget(search_, 1);

  auto* import = new QToolButton(this);
  import->setObjectName(QStringLiteral("importMediaButton"));
  import->setAccessibleName(tr("Import media"));
  import->setToolTip(tr("Import media"));
  import->setText(tr("Import"));
  import->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  import->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tools->addWidget(import);
  layout->addLayout(tools);

  content_ = new QStackedWidget(this);
  content_->setObjectName(QStringLiteral("mediaBinContent"));

  auto* empty = new QWidget(content_);
  empty->setObjectName(QStringLiteral("mediaBinEmptyState"));
  auto* emptyLayout = new QVBoxLayout(empty);
  emptyLayout->setContentsMargins(18, 30, 18, 30);
  emptyLayout->addStretch();
  auto* heading = new QLabel(tr("Bring in your media"), empty);
  heading->setAlignment(Qt::AlignCenter);
  QFont headingFont = heading->font();
  headingFont.setWeight(QFont::DemiBold);
  headingFont.setPointSize(headingFont.pointSize() + 2);
  heading->setFont(headingFont);
  emptyLayout->addWidget(heading);
  emptyLayout->addWidget(
      makeMutedLabel(tr("Video, audio, and still images stay in their original location."), empty));
  auto* emptyImport = new QPushButton(tr("Import media"), empty);
  emptyImport->setObjectName(QStringLiteral("emptyStateImportButton"));
  emptyImport->setAccessibleName(tr("Import media from files"));
  emptyImport->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  emptyLayout->addWidget(emptyImport, 0, Qt::AlignHCenter);
  emptyLayout->addStretch();
  content_->addWidget(empty);

  table_ = new QTableWidget(content_);
  table_->setObjectName(QStringLiteral("mediaTable"));
  table_->setAccessibleName(tr("Imported media"));
  table_->setColumnCount(5);
  table_->setHorizontalHeaderLabels(
      {QString{}, tr("Name"), tr("Duration"), tr("Format"), tr("Status")});
  table_->setIconSize(QSize{40, 40});
  table_->verticalHeader()->setDefaultSectionSize(48);
  table_->horizontalHeader()->setSectionResizeMode(kMediaIconColumn, QHeaderView::Fixed);
  table_->setColumnWidth(kMediaIconColumn, 48);
  table_->horizontalHeader()->setSectionResizeMode(kMediaNameColumn, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(kMediaFormatColumn, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(kMediaStatusColumn, QHeaderView::ResizeToContents);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setShowGrid(false);
  table_->setSortingEnabled(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  content_->addWidget(table_);
  layout->addWidget(content_, 1);

  connect(import, &QToolButton::clicked, this, &MediaBinWidget::importRequested);
  connect(emptyImport, &QPushButton::clicked, this, &MediaBinWidget::importRequested);
  connect(search_, &QLineEdit::textChanged, this, &MediaBinWidget::applyFilter);
  connect(table_, &QTableWidget::itemDoubleClicked, this, [this] { activateCurrent(); });
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          &MediaBinWidget::emitCurrentMediaSelection);
  connect(table_, &QTableWidget::currentCellChanged, this,
          [this](int, int, int, int) { emitCurrentMediaSelection(); });
  connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& point) {
    const auto row = table_->rowAt(point.y());
    const auto id = mediaIdAtRow(row);
    if (id.isEmpty()) {
      return;
    }
    const auto item =
        std::find_if(items_.cbegin(), items_.cend(),
                     [&id](const MediaItemView& candidate) { return candidate.id == id; });
    QMenu menu(this);
    auto* relink = menu.addAction(tr("Relink media…"));
    relink->setEnabled(item != items_.cend() && (item->offline || item->contentChanged));
    QAction* proxy = nullptr;
    if (item != items_.cend() && !item->offline && !item->proxyAvailable) {
      proxy = menu.addAction(item->proxyGenerating ? tr("Cancel proxy generation")
                                                   : tr("Create editing proxy"));
    }
    const QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(point));
    if (chosen == relink) {
      emit relinkRequested(id);
    } else if (proxy != nullptr && chosen == proxy) {
      emit proxyRequested(id);
    }
  });
}

void MediaBinWidget::setItems(const QVector<MediaItemView>& items) {
  items_ = items;
  rebuildTable();
  applyFilter(search_->text());
}

void MediaBinWidget::applyFilter(const QString& query) {
  for (int row = 0; row < table_->rowCount(); ++row) {
    const auto* name = table_->item(row, kMediaNameColumn);
    const auto* format = table_->item(row, kMediaFormatColumn);
    const auto matches = query.trimmed().isEmpty() ||
                         (name != nullptr && name->text().contains(query, Qt::CaseInsensitive)) ||
                         (format != nullptr && format->text().contains(query, Qt::CaseInsensitive));
    table_->setRowHidden(row, !matches);
  }
  emit searchChanged(query);
}

void MediaBinWidget::activateCurrent() {
  const auto id = mediaIdAtRow(table_->currentRow());
  if (!id.isEmpty()) {
    emit mediaActivated(id);
  }
}

void MediaBinWidget::emitCurrentMediaSelection() {
  emit mediaSelectionChanged(mediaIdAtRow(table_->currentRow()));
}

QString MediaBinWidget::mediaIdAtRow(const int row) const {
  if (table_ == nullptr || row < 0) {
    return {};
  }
  if (const auto* name = table_->item(row, kMediaNameColumn)) {
    const auto id = name->data(Qt::UserRole).toString();
    if (!id.isEmpty()) {
      return id;
    }
  }
  if (const auto* icon = table_->item(row, kMediaIconColumn)) {
    return icon->data(Qt::UserRole).toString();
  }
  return {};
}

void MediaBinWidget::rebuildTable() {
  table_->setSortingEnabled(false);
  table_->setRowCount(items_.size());
  for (int row = 0; row < items_.size(); ++row) {
    const auto& item = items_.at(row);
    auto* icon = new QTableWidgetItem();
    if (!item.thumbnail.isNull()) {
      icon->setIcon(QIcon{QPixmap::fromImage(item.thumbnail)});
    }
    icon->setData(Qt::UserRole, item.id);
    icon->setTextAlignment(Qt::AlignCenter);
    table_->setItem(row, kMediaIconColumn, icon);

    const QString visible_name =
        item.metadataTitle.isEmpty() ? item.displayName : item.metadataTitle;
    auto* name = new QTableWidgetItem(visible_name);
    name->setData(Qt::UserRole, item.id);
    name->setToolTip(item.filePath);
    table_->setItem(row, kMediaNameColumn, name);
    table_->setItem(row, 2, new QTableWidgetItem(item.durationText));
    table_->setItem(row, kMediaFormatColumn, new QTableWidgetItem(item.formatText));

    QString status_text;
    QColor status_color{164, 193, 168};
    if (item.offline) {
      status_text = tr("Offline");
      status_color = QColor{235, 126, 126};
    } else if (item.contentChanged) {
      status_text = tr("Changed");
      status_color = QColor{225, 183, 98};
    } else if (item.proxyGenerating) {
      status_text = tr("Creating proxy…");
    } else if (item.proxyAvailable) {
      status_text = tr("Proxy ready");
    } else if (item.proxyRecommended) {
      status_text = tr("Proxy recommended");
      status_color = QColor{225, 183, 98};
    } else {
      status_text = tr("Original");
    }
    auto* status = new QTableWidgetItem(status_text);
    status->setForeground(status_color);
    if (item.proxyRecommended && !item.proxyAvailable && !item.proxyGenerating &&
        !item.contentChanged) {
      status->setToolTip(tr("Right-click this item to create a smoother editing proxy"));
    }
    table_->setItem(row, kMediaStatusColumn, status);
  }
  table_->setSortingEnabled(true);
  content_->setCurrentIndex(items_.isEmpty() ? 0 : 1);
}

InspectorWidget::InspectorWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("inspectorPanel"));
  setAccessibleName(tr("Inspector"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);

  selection_name_ = new QLabel(tr("No selection"), this);
  selection_name_->setObjectName(QStringLiteral("inspectorSelectionName"));
  selection_name_->setAccessibleName(tr("Inspector selection"));
  QFont titleFont = selection_name_->font();
  titleFont.setWeight(QFont::DemiBold);
  selection_name_->setFont(titleFont);
  layout->addWidget(selection_name_);

  asset_group_ = new QGroupBox(tr("Asset"), this);
  asset_group_->setObjectName(QStringLiteral("inspectorAssetGroup"));
  asset_group_->setAccessibleName(tr("Asset metadata"));
  auto* assetForm = new QFormLayout(asset_group_);
  assetForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  asset_title_ = new QLineEdit(asset_group_);
  asset_title_->setObjectName(QStringLiteral("inspector.assetTitle"));
  asset_title_->setAccessibleName(tr("Asset title"));
  assetForm->addRow(tr("Title"), asset_title_);
  asset_tags_ = new QLineEdit(asset_group_);
  asset_tags_->setObjectName(QStringLiteral("inspector.assetTags"));
  asset_tags_->setAccessibleName(tr("Asset tags"));
  asset_tags_->setPlaceholderText(tr("comma-separated"));
  assetForm->addRow(tr("Tags"), asset_tags_);
  asset_notes_ = new QPlainTextEdit(asset_group_);
  asset_notes_->setObjectName(QStringLiteral("inspector.assetNotes"));
  asset_notes_->setAccessibleName(tr("Asset notes"));
  asset_notes_->setTabChangesFocus(true);
  asset_notes_->setFixedHeight(asset_notes_->fontMetrics().lineSpacing() * 4 + 14);
  assetForm->addRow(tr("Notes"), asset_notes_);
  asset_rating_ = new QSpinBox(asset_group_);
  asset_rating_->setObjectName(QStringLiteral("inspector.assetRating"));
  asset_rating_->setAccessibleName(tr("Asset rating"));
  asset_rating_->setRange(0, 5);
  asset_rating_->setKeyboardTracking(false);
  assetForm->addRow(tr("Rating"), asset_rating_);
  setTabOrder(asset_title_, asset_tags_);
  setTabOrder(asset_tags_, asset_notes_);
  setTabOrder(asset_notes_, asset_rating_);
  connect(asset_title_, &QLineEdit::textEdited, this, &InspectorWidget::publishAssetMetadata);
  connect(asset_tags_, &QLineEdit::textEdited, this, &InspectorWidget::publishAssetMetadata);
  connect(asset_notes_, &QPlainTextEdit::textChanged, this, &InspectorWidget::publishAssetMetadata);
  connect(asset_rating_, &QSpinBox::valueChanged, this, &InspectorWidget::publishAssetMetadata);
  asset_group_->setVisible(false);
  asset_group_->setEnabled(false);
  layout->addWidget(asset_group_);

  content_ = new QStackedWidget(this);
  auto* empty =
      makeMutedLabel(tr("Select a clip, transition, title, or track to adjust it."), content_);
  empty->setObjectName(QStringLiteral("inspectorEmptyState"));
  content_->addWidget(empty);

  auto* scroll = new QScrollArea(content_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* editor = new QWidget(scroll);
  auto* editorLayout = new QVBoxLayout(editor);
  editorLayout->setContentsMargins(0, 0, 0, 0);

  auto* essential = new QGroupBox(tr("Essential"), editor);
  essential->setObjectName(QStringLiteral("inspectorEssentialGroup"));
  visual_controls_ = essential;
  transform_form_ = new QFormLayout(essential);
  transform_form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  const auto addField = [this, essential](const QString& label, const QString& id,
                                          const QString& suffix, double minimum, double maximum,
                                          double value, double step) {
    auto* field =
        makeInspectorField(id, suffix, minimum, maximum, value, step, essential,
                           [this, id](double updated) { emit parameterEdited(id, updated); });
    auto* keyframe = field->findChild<QToolButton*>(QStringLiteral("keyframe.%1").arg(id));
    connect(keyframe, &QToolButton::clicked, this,
            [this, id] { emit keyframeToggleRequested(id); });
    transform_form_->addRow(label, field);
  };
  addField(tr("Position X"), QStringLiteral("positionX"), QStringLiteral(" px"), -100'000, 100'000,
           0, 1);
  addField(tr("Position Y"), QStringLiteral("positionY"), QStringLiteral(" px"), -100'000, 100'000,
           0, 1);
  addField(tr("Scale"), QStringLiteral("scale"), QStringLiteral(" %"), -100'000, 2'000, 100, 1);
  addField(tr("Rotation"), QStringLiteral("rotation"), QStringLiteral("°"), -36000, 36000, 0, 0.1);
  addField(tr("Opacity"), QStringLiteral("opacity"), QStringLiteral(" %"), 0, 100, 100, 1);
  editorLayout->addWidget(essential);

  auto* audio = new QGroupBox(tr("Clip audio"), editor);
  audio->setObjectName(QStringLiteral("inspectorAudioGroup"));
  audio_controls_ = audio;
  auto* audioForm = new QFormLayout(audio);
  const auto addAudioField = [this, audio, audioForm](const QString& label, const QString& id,
                                                      const QString& suffix, double minimum,
                                                      double maximum, double value, double step) {
    auto* field =
        makeInspectorField(id, suffix, minimum, maximum, value, step, audio,
                           [this, id](double updated) { emit parameterEdited(id, updated); });
    audioForm->addRow(label, field);
  };
  addAudioField(tr("Gain"), QStringLiteral("audioGain"), QStringLiteral(" dB"), -96, 24, 0, 0.1);
  addAudioField(tr("Pan"), QStringLiteral("audioPan"), QStringLiteral(" %"), -100, 100, 0, 1);
  addAudioField(tr("Fade in"), QStringLiteral("fadeIn"), QStringLiteral(" s"), 0, 86'400, 0, 0.01);
  addAudioField(tr("Fade out"), QStringLiteral("fadeOut"), QStringLiteral(" s"), 0, 86'400, 0,
                0.01);
  editorLayout->addWidget(audio);

  auto* titleGroup = new QGroupBox(tr("Title"), editor);
  titleGroup->setObjectName(QStringLiteral("inspectorTitleGroup"));
  title_controls_ = titleGroup;
  auto* titleForm = new QFormLayout(titleGroup);
  titleForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  auto* titleText = new QLineEdit(titleGroup);
  titleText->setObjectName(QStringLiteral("inspector.titleText"));
  titleText->setAccessibleName(tr("Title text"));
  titleText->setPlaceholderText(tr("Title text"));
  connect(titleText, &QLineEdit::textEdited, this,
          [this](const QString& text) { emit parameterEdited(QStringLiteral("titleText"), text); });
  titleForm->addRow(tr("Text"), titleText);
  auto* titleFontEdit = new QLineEdit(titleGroup);
  titleFontEdit->setObjectName(QStringLiteral("inspector.titleFont"));
  titleFontEdit->setAccessibleName(tr("Title font family"));
  titleFontEdit->setText(QStringLiteral("sans-serif"));
  connect(titleFontEdit, &QLineEdit::textEdited, this,
          [this](const QString& text) { emit parameterEdited(QStringLiteral("titleFont"), text); });
  titleForm->addRow(tr("Font"), titleFontEdit);
  const auto addTitleField =
      [this, titleGroup, titleForm](const QString& label, const QString& id, const QString& suffix,
                                    double minimum, double maximum, double value, double step) {
        auto* field =
            makeInspectorField(id, suffix, minimum, maximum, value, step, titleGroup,
                               [this, id](double updated) { emit parameterEdited(id, updated); });
        titleForm->addRow(label, field);
      };
  addTitleField(tr("Size"), QStringLiteral("titleSize"), QStringLiteral(" pt"), 1.0, 4096.0, 96.0,
                1.0);
  auto* titleAlign = new QComboBox(titleGroup);
  titleAlign->setObjectName(QStringLiteral("inspector.titleAlign"));
  titleAlign->setAccessibleName(tr("Title alignment"));
  titleAlign->addItem(tr("Left"), QStringLiteral("left"));
  titleAlign->addItem(tr("Center"), QStringLiteral("center"));
  titleAlign->addItem(tr("Right"), QStringLiteral("right"));
  titleAlign->setCurrentIndex(1);
  connect(titleAlign, &QComboBox::currentIndexChanged, this, [this, titleAlign](const int index) {
    emit parameterEdited(QStringLiteral("titleAlign"), titleAlign->itemData(index));
  });
  titleForm->addRow(tr("Alignment"), titleAlign);
  auto* titleBold = new QToolButton(titleGroup);
  titleBold->setObjectName(QStringLiteral("inspector.titleBold"));
  titleBold->setAccessibleName(tr("Bold"));
  titleBold->setText(tr("B"));
  titleBold->setCheckable(true);
  QFont boldFont = titleBold->font();
  boldFont.setBold(true);
  titleBold->setFont(boldFont);
  connect(titleBold, &QToolButton::toggled, this, [this](const bool checked) {
    emit parameterEdited(QStringLiteral("titleBold"), checked);
  });
  auto* titleItalic = new QToolButton(titleGroup);
  titleItalic->setObjectName(QStringLiteral("inspector.titleItalic"));
  titleItalic->setAccessibleName(tr("Italic"));
  titleItalic->setText(tr("I"));
  titleItalic->setCheckable(true);
  QFont italicFont = titleItalic->font();
  italicFont.setItalic(true);
  titleItalic->setFont(italicFont);
  connect(titleItalic, &QToolButton::toggled, this, [this](const bool checked) {
    emit parameterEdited(QStringLiteral("titleItalic"), checked);
  });
  auto* titleStyleRow = new QWidget(titleGroup);
  auto* titleStyleLayout = new QHBoxLayout(titleStyleRow);
  titleStyleLayout->setContentsMargins(0, 0, 0, 0);
  titleStyleLayout->setSpacing(4);
  titleStyleLayout->addWidget(titleBold);
  titleStyleLayout->addWidget(titleItalic);
  titleStyleLayout->addStretch();
  titleForm->addRow(tr("Style"), titleStyleRow);
  titleGroup->setVisible(false);
  editorLayout->addWidget(titleGroup);

  auto* speedGroup = new QGroupBox(tr("Speed"), editor);
  speedGroup->setObjectName(QStringLiteral("inspectorSpeedGroup"));
  speed_controls_ = speedGroup;
  auto* speedForm = new QFormLayout(speedGroup);
  auto* speedField = makeInspectorField(
      QStringLiteral("speed"), QStringLiteral(" %"), 0.01, 20'000.0, 100.0, 1.0, speedGroup,
      [this](double updated) { emit parameterEdited(QStringLiteral("speed"), updated); });
  speedForm->addRow(tr("Rate"), speedField);
  auto* reverseCheck = new QCheckBox(tr("Reverse"), speedGroup);
  reverseCheck->setObjectName(QStringLiteral("inspector.reverse"));
  reverseCheck->setAccessibleName(tr("Reverse playback"));
  connect(reverseCheck, &QCheckBox::toggled, this,
          [this](const bool checked) { emit parameterEdited(QStringLiteral("reverse"), checked); });
  speedForm->addRow(QString{}, reverseCheck);
  speedGroup->setVisible(false);
  editorLayout->addWidget(speedGroup);

  auto* advanced = new QGroupBox(tr("Advanced controls"), editor);
  advanced->setObjectName(QStringLiteral("inspectorAdvancedGroup"));
  advanced_controls_ = advanced;
  advanced->setCheckable(true);
  advanced->setChecked(false);
  auto* advancedForm = new QFormLayout(advanced);
  const auto addAdvancedField =
      [this, advanced, advancedForm](const QString& label, const QString& id, const QString& suffix,
                                     const double minimum, const double maximum, const double value,
                                     const double step) {
        auto* field =
            makeInspectorField(id, suffix, minimum, maximum, value, step, advanced,
                               [this, id](double updated) { emit parameterEdited(id, updated); });
        auto* keyframe = field->findChild<QToolButton*>(QStringLiteral("keyframe.%1").arg(id));
        connect(keyframe, &QToolButton::clicked, this,
                [this, id] { emit keyframeToggleRequested(id); });
        advancedForm->addRow(label, field);
      };
  addAdvancedField(tr("Scale X"), QStringLiteral("scaleX"), QStringLiteral(" %"), -100'000, 100'000,
                   100, 0.1);
  addAdvancedField(tr("Scale Y"), QStringLiteral("scaleY"), QStringLiteral(" %"), -100'000, 100'000,
                   100, 0.1);
  addAdvancedField(tr("Anchor X"), QStringLiteral("anchorX"), QStringLiteral(" %"), 0, 100, 50,
                   0.1);
  addAdvancedField(tr("Anchor Y"), QStringLiteral("anchorY"), QStringLiteral(" %"), 0, 100, 50,
                   0.1);
  addAdvancedField(tr("Crop left"), QStringLiteral("cropLeft"), QStringLiteral(" %"), 0, 99.99, 0,
                   0.1);
  addAdvancedField(tr("Crop top"), QStringLiteral("cropTop"), QStringLiteral(" %"), 0, 99.99, 0,
                   0.1);
  addAdvancedField(tr("Crop right"), QStringLiteral("cropRight"), QStringLiteral(" %"), 0, 99.99, 0,
                   0.1);
  addAdvancedField(tr("Crop bottom"), QStringLiteral("cropBottom"), QStringLiteral(" %"), 0, 99.99,
                   0, 0.1);
  auto* blend = new QComboBox(advanced);
  blend->setObjectName(QStringLiteral("inspector.blendMode"));
  blend->setAccessibleName(tr("Blend mode"));
  blend->addItem(tr("Normal"), QStringLiteral("normal"));
  blend->addItem(tr("Add"), QStringLiteral("add"));
  blend->addItem(tr("Multiply"), QStringLiteral("multiply"));
  blend->addItem(tr("Screen"), QStringLiteral("screen"));
  blend->addItem(tr("Overlay"), QStringLiteral("overlay"));
  advancedForm->addRow(tr("Blend mode"), blend);
  auto* interpolation = new QComboBox(advanced);
  interpolation->setObjectName(QStringLiteral("inspector.interpolation"));
  interpolation->setAccessibleName(tr("Keyframe interpolation"));
  interpolation->addItems({tr("Smooth"), tr("Linear"), tr("Hold")});
  advancedForm->addRow(tr("Interpolation"), interpolation);
  connect(blend, &QComboBox::currentIndexChanged, this, [this, blend](const int index) {
    emit parameterEdited(QStringLiteral("blendMode"), blend->itemData(index));
  });
  connect(interpolation, &QComboBox::currentTextChanged, this, [this](const QString& text) {
    emit parameterEdited(QStringLiteral("interpolation"), text);
  });
  editorLayout->addWidget(advanced);

  auto* effects = new QGroupBox(tr("Effects & animation"), editor);
  effects->setObjectName(QStringLiteral("inspectorEffectsGroup"));
  effects->setAccessibleDescription(
      tr("Expandable controls for effect parameters, keyframe timing, and interpolation."));
  effects->setCheckable(true);
  effects->setChecked(false);
  effects_controls_ = effects;
  auto* effectsLayout = new QVBoxLayout(effects);
  effectsLayout->setContentsMargins(8, 8, 8, 8);
  pick_white_balance_ = new QPushButton(tr("Pick white balance"), effects);
  pick_white_balance_->setObjectName(QStringLiteral("pickWhiteBalance"));
  pick_white_balance_->setAccessibleName(tr("Pick white balance from program viewer"));
  pick_white_balance_->setToolTip(tr("Click the program viewer to sample neutral gray"));
  pick_white_balance_->hide();
  effectsLayout->addWidget(pick_white_balance_);
  connect(pick_white_balance_, &QPushButton::clicked, this,
          &InspectorWidget::pickWhiteBalanceRequested);
  auto* effectsBody = new QWidget(effects);
  effectsBody->setObjectName(QStringLiteral("effectParameterEditor"));
  effect_parameter_editor_ = effectsBody;
  auto* effectsBodyLayout = new QVBoxLayout(effectsBody);
  effectsBodyLayout->setContentsMargins(0, 0, 0, 0);
  effect_parameter_selector_ = new QComboBox(effectsBody);
  effect_parameter_selector_->setObjectName(QStringLiteral("effectParameterSelector"));
  effect_parameter_selector_->setAccessibleName(tr("Effect parameter"));
  effectsBodyLayout->addWidget(effect_parameter_selector_);
  auto* parameterEditor = new QWidget(effectsBody);
  effect_parameter_editor_ = parameterEditor;
  effect_parameter_form_ = new QFormLayout(parameterEditor);
  effect_parameter_form_->setContentsMargins(0, 0, 0, 0);
  effectsBodyLayout->addWidget(parameterEditor);

  auto* keyframesLabel = new QLabel(tr("Keyframes"), effectsBody);
  keyframesLabel->setObjectName(QStringLiteral("keyframeListLabel"));
  effectsBodyLayout->addWidget(keyframesLabel);
  keyframe_list_ = new QListWidget(effectsBody);
  keyframe_list_->setObjectName(QStringLiteral("keyframeList"));
  keyframe_list_->setAccessibleName(tr("Keyframes for effect parameter"));
  keyframe_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  keyframe_list_->setMaximumHeight(110);
  effectsBodyLayout->addWidget(keyframe_list_);

  auto* keyframeForm = new QFormLayout;
  keyframe_time_ = new QDoubleSpinBox(effectsBody);
  keyframe_time_->setObjectName(QStringLiteral("keyframeTime"));
  keyframe_time_->setAccessibleName(tr("Keyframe time"));
  keyframe_time_->setRange(0.0, 86'400.0);
  keyframe_time_->setDecimals(3);
  keyframe_time_->setSuffix(tr(" s"));
  keyframe_time_->setKeyboardTracking(false);
  keyframeForm->addRow(tr("Time"), keyframe_time_);
  keyframe_value_ = new QDoubleSpinBox(effectsBody);
  keyframe_value_->setObjectName(QStringLiteral("keyframeValue"));
  keyframe_value_->setAccessibleName(tr("Keyframe value"));
  keyframe_value_->setRange(-1.0e9, 1.0e9);
  keyframe_value_->setDecimals(4);
  keyframe_value_->setKeyboardTracking(false);
  keyframeForm->addRow(tr("Value"), keyframe_value_);
  keyframe_interpolation_ = new QComboBox(effectsBody);
  keyframe_interpolation_->setObjectName(QStringLiteral("keyframeInterpolation"));
  keyframe_interpolation_->setAccessibleName(tr("Keyframe interpolation"));
  keyframe_interpolation_->addItem(tr("Hold"),
                                   QVariant::fromValue(KeyframeInterpolationView::Hold));
  keyframe_interpolation_->addItem(tr("Linear"),
                                   QVariant::fromValue(KeyframeInterpolationView::Linear));
  keyframe_interpolation_->addItem(tr("Bezier"),
                                   QVariant::fromValue(KeyframeInterpolationView::Bezier));
  keyframeForm->addRow(tr("Interpolation"), keyframe_interpolation_);
  auto* keyframeActions = new QHBoxLayout;
  keyframe_delete_ = new QToolButton(effectsBody);
  keyframe_delete_->setObjectName(QStringLiteral("deleteKeyframe"));
  keyframe_delete_->setAccessibleName(tr("Delete selected keyframe"));
  keyframe_delete_->setText(tr("Delete keyframe"));
  keyframe_delete_->setEnabled(false);
  keyframeActions->addWidget(keyframe_delete_);
  keyframeActions->addStretch();
  keyframeForm->addRow(QString{}, keyframeActions);
  effectsBodyLayout->addLayout(keyframeForm);

  keyframe_curve_ = new KeyframeCurveWidget(effectsBody);
  effectsBodyLayout->addWidget(keyframe_curve_);
  effectsLayout->addWidget(effectsBody);
  effectsBody->setVisible(false);
  connect(effects, &QGroupBox::toggled, effectsBody, &QWidget::setVisible);
  connect(effect_parameter_selector_, &QComboBox::currentIndexChanged, this,
          &InspectorWidget::selectEffectParameter);
  connect(keyframe_list_, &QListWidget::currentRowChanged, this, &InspectorWidget::selectKeyframe);
  connect(keyframe_time_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (active_effect_parameter_ < 0 || active_keyframe_ < 0 ||
        active_effect_parameter_ >= effect_parameters_.size() ||
        active_keyframe_ >= effect_parameters_.at(active_effect_parameter_).keyframes.size()) {
      return;
    }
    auto& keyframe = effect_parameters_[active_effect_parameter_].keyframes[active_keyframe_];
    keyframe.time = static_cast<qint64>(std::llround(keyframe_time_->value() * 48'000.0));
    emit effectKeyframeValueEdited(effect_parameters_.at(active_effect_parameter_).effectId,
                                   effect_parameters_.at(active_effect_parameter_).parameterId,
                                   keyframe.id, keyframe.time, keyframe.value);
    refreshKeyframeEditor();
  });
  connect(keyframe_value_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (active_effect_parameter_ < 0 || active_keyframe_ < 0 ||
        active_effect_parameter_ >= effect_parameters_.size() ||
        active_keyframe_ >= effect_parameters_.at(active_effect_parameter_).keyframes.size()) {
      return;
    }
    auto& keyframe = effect_parameters_[active_effect_parameter_].keyframes[active_keyframe_];
    keyframe.value = keyframe_value_->value();
    emit effectKeyframeValueEdited(effect_parameters_.at(active_effect_parameter_).effectId,
                                   effect_parameters_.at(active_effect_parameter_).parameterId,
                                   keyframe.id, keyframe.time, keyframe.value);
    refreshKeyframeEditor();
  });
  connect(keyframe_interpolation_, &QComboBox::currentIndexChanged, this, [this](const int index) {
    if (active_effect_parameter_ < 0 || active_keyframe_ < 0 || index < 0 ||
        active_effect_parameter_ >= effect_parameters_.size() ||
        active_keyframe_ >= effect_parameters_.at(active_effect_parameter_).keyframes.size()) {
      return;
    }
    const auto interpolation =
        keyframe_interpolation_->itemData(index).value<KeyframeInterpolationView>();
    auto& keyframe = effect_parameters_[active_effect_parameter_].keyframes[active_keyframe_];
    keyframe.interpolation = interpolation;
    emit effectKeyframeInterpolationEdited(
        effect_parameters_.at(active_effect_parameter_).effectId,
        effect_parameters_.at(active_effect_parameter_).parameterId, keyframe.id, interpolation);
    keyframe_curve_->setKeyframes(effect_parameters_.at(active_effect_parameter_).keyframes, 1,
                                  keyframe_value_->minimum(), keyframe_value_->maximum());
  });
  connect(keyframe_delete_, &QToolButton::clicked, this, [this] {
    if (active_effect_parameter_ < 0 || active_keyframe_ < 0 ||
        active_effect_parameter_ >= effect_parameters_.size() ||
        active_keyframe_ >= effect_parameters_.at(active_effect_parameter_).keyframes.size()) {
      return;
    }
    const auto& parameter = effect_parameters_.at(active_effect_parameter_);
    const auto& keyframe = parameter.keyframes.at(active_keyframe_);
    emit effectKeyframeRemoved(parameter.effectId, parameter.parameterId, keyframe.id);
  });
  connect(keyframe_curve_, &KeyframeCurveWidget::keyframeSelected, this, [this](const QString& id) {
    if (active_effect_parameter_ < 0) {
      return;
    }
    const auto& keyframes = effect_parameters_.at(active_effect_parameter_).keyframes;
    const auto it = std::find_if(keyframes.cbegin(), keyframes.cend(),
                                 [&id](const auto& keyframe) { return keyframe.id == id; });
    if (it != keyframes.cend()) {
      selectKeyframe(static_cast<int>(std::distance(keyframes.cbegin(), it)));
    }
  });
  connect(keyframe_curve_, &KeyframeCurveWidget::keyframeValueCommitted, this,
          [this](const QString& id, const qint64 time, const double value) {
            if (active_effect_parameter_ < 0) {
              return;
            }
            const auto& parameter = effect_parameters_.at(active_effect_parameter_);
            emit effectKeyframeValueEdited(parameter.effectId, parameter.parameterId, id, time,
                                           value);
          });
  connect(keyframe_curve_, &KeyframeCurveWidget::keyframeControlPointsCommitted, this,
          [this](const QString& id, const QPointF& incoming, const QPointF& outgoing) {
            if (active_effect_parameter_ < 0) {
              return;
            }
            const auto& parameter = effect_parameters_.at(active_effect_parameter_);
            emit effectKeyframeControlPointsEdited(parameter.effectId, parameter.parameterId, id,
                                                   incoming, outgoing);
          });
  editorLayout->addWidget(effects);
  editorLayout->addStretch();
  scroll->setWidget(editor);
  content_->addWidget(scroll);
  layout->addWidget(content_, 1);
}

void InspectorWidget::setAssetMetadata(const AssetMetadataView& metadata) {
  asset_metadata_ = metadata;
  const QSignalBlocker titleBlocker(asset_title_);
  const QSignalBlocker tagsBlocker(asset_tags_);
  const QSignalBlocker notesBlocker(asset_notes_);
  const QSignalBlocker ratingBlocker(asset_rating_);
  asset_title_->setText(metadata.title);
  asset_tags_->setText(metadata.tags.join(QStringLiteral(", ")));
  asset_notes_->setPlainText(metadata.notes);
  asset_rating_->setValue(std::clamp(metadata.rating, 0, 5));
  const bool has_asset = !metadata.assetId.isEmpty();
  asset_group_->setVisible(has_asset);
  asset_group_->setEnabled(has_asset);
}

void InspectorWidget::clearAssetMetadata() {
  setAssetMetadata({});
}

void InspectorWidget::publishAssetMetadata() {
  if (asset_metadata_.assetId.isEmpty()) {
    return;
  }
  AssetMetadataView view = asset_metadata_;
  view.title = asset_title_->text();
  view.tags = splitCommaSeparatedTags(asset_tags_->text());
  view.notes = asset_notes_->toPlainText();
  view.rating = asset_rating_->value();
  asset_metadata_ = view;
  emit assetMetadataEdited(view);
}

void InspectorWidget::setSelectionName(const QString& name) {
  selection_name_->setText(name.isEmpty() ? tr("No selection") : name);
  content_->setCurrentIndex(name.isEmpty() ? 0 : 1);
}

void InspectorWidget::setClipCapabilities(const bool visual, const bool audio) {
  visual_controls_->setVisible(visual);
  advanced_controls_->setVisible(visual);
  audio_controls_->setVisible(audio);
}

void InspectorWidget::setTitleControlsVisible(const bool visible) {
  title_controls_->setVisible(visible);
}

void InspectorWidget::setSpeedControlsVisible(const bool visible) {
  speed_controls_->setVisible(visible);
}

void InspectorWidget::setParameter(const QString& parameterId, const QVariant& value) {
  if (auto* spin = findChild<QDoubleSpinBox*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(spin);
    spin->setValue(value.toDouble());
    return;
  }
  if (auto* combo = findChild<QComboBox*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(combo);
    const int data_index = combo->findData(value);
    if (data_index >= 0) {
      combo->setCurrentIndex(data_index);
    } else {
      combo->setCurrentText(value.toString());
    }
    return;
  }
  if (auto* line = findChild<QLineEdit*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(line);
    line->setText(value.toString());
    return;
  }
  if (auto* check = findChild<QCheckBox*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(check);
    check->setChecked(value.toBool());
    return;
  }
  if (auto* tool = findChild<QToolButton*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(tool);
    tool->setChecked(value.toBool());
  }
}

void InspectorWidget::setEffectParameters(const QVector<EffectParameterView>& parameters) {
  effect_parameters_ = parameters;
  active_effect_parameter_ = -1;
  active_keyframe_ = -1;
  {
    const QSignalBlocker blocker(effect_parameter_selector_);
    effect_parameter_selector_->clear();
    for (const auto& parameter : effect_parameters_) {
      effect_parameter_selector_->addItem(
          parameter.effectName.isEmpty()
              ? parameter.displayName
              : tr("%1 · %2").arg(parameter.effectName, parameter.displayName),
          parameter.parameterId);
    }
  }
  rebuildEffectParameterFields();
  if (!effect_parameters_.isEmpty()) {
    effect_parameter_selector_->setCurrentIndex(0);
    selectEffectParameter(0);
  } else {
    refreshKeyframeEditor();
  }
  if (effects_controls_ != nullptr) {
    effects_controls_->setVisible(!effect_parameters_.isEmpty());
    effects_controls_->setChecked(!effect_parameters_.isEmpty());
  }
  if (pick_white_balance_ != nullptr) {
    const bool has_color_effect = std::any_of(
        effect_parameters_.cbegin(), effect_parameters_.cend(), [](const EffectParameterView& view) {
          return view.effectType == QStringLiteral("video.color");
        });
    pick_white_balance_->setVisible(has_color_effect);
  }
}

void InspectorWidget::rebuildEffectParameterFields() {
  if (effect_parameter_form_ == nullptr) {
    return;
  }
  while (effect_parameter_form_->rowCount() > 0) {
    effect_parameter_form_->removeRow(0);
  }
  for (int index = 0; index < effect_parameters_.size(); ++index) {
    const auto& parameter = effect_parameters_.at(index);
    auto* row = new QWidget(effect_parameter_editor_);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    const QString objectName = QStringLiteral("effectParameter.%1").arg(index);
    if (parameter.value.metaType().id() == QMetaType::Bool) {
      auto* check = new QCheckBox(row);
      check->setObjectName(objectName);
      check->setAccessibleName(parameter.displayName);
      check->setChecked(parameter.value.toBool());
      connect(check, &QCheckBox::toggled, this, [this, index](const bool value) {
        if (index < effect_parameters_.size()) {
          emit effectParameterEdited(effect_parameters_.at(index).effectId,
                                     effect_parameters_.at(index).parameterId, value);
        }
      });
      rowLayout->addWidget(check);
    } else if (parameter.value.canConvert<double>() &&
               parameter.value.metaType().id() != QMetaType::QString) {
      auto* spin = new QDoubleSpinBox(row);
      spin->setObjectName(objectName);
      spin->setAccessibleName(parameter.displayName);
      spin->setRange(-1.0e9, 1.0e9);
      spin->setDecimals(4);
      spin->setKeyboardTracking(false);
      spin->setValue(parameter.value.toDouble());
      connect(spin, &QDoubleSpinBox::editingFinished, this, [this, index, spin] {
        if (index < effect_parameters_.size()) {
          emit effectParameterEdited(effect_parameters_.at(index).effectId,
                                     effect_parameters_.at(index).parameterId, spin->value());
        }
      });
      rowLayout->addWidget(spin, 1);
    } else {
      auto* line = new QLineEdit(row);
      line->setObjectName(objectName);
      line->setAccessibleName(parameter.displayName);
      line->setText(parameter.value.toString());
      connect(line, &QLineEdit::editingFinished, this, [this, index, line] {
        if (index < effect_parameters_.size()) {
          emit effectParameterEdited(effect_parameters_.at(index).effectId,
                                     effect_parameters_.at(index).parameterId, line->text());
        }
      });
      rowLayout->addWidget(line, 1);
      if (parameter.effectType == QStringLiteral("video.lut") &&
          parameter.parameterId == QStringLiteral("path")) {
        auto* browse = new QPushButton(tr("Browse LUT…"), row);
        browse->setAccessibleName(tr("Browse for LUT file"));
        connect(browse, &QPushButton::clicked, this, [this, index] {
          if (index < effect_parameters_.size()) {
            const auto& parameter = effect_parameters_.at(index);
            emit effectLutBrowseRequested(parameter.effectId, parameter.parameterId);
          }
        });
        rowLayout->addWidget(browse);
      }
      if (parameter.effectType == QStringLiteral("video.curves")) {
        auto* note = new QLabel(tr("Format: x,y;x,y (0–1)"), row);
        note->setObjectName(QStringLiteral("curvesFormatNote"));
        note->setWordWrap(true);
        rowLayout->addWidget(note);
      }
    }
    auto* keyframe = new QToolButton(row);
    keyframe->setObjectName(QStringLiteral("effectKeyframe.%1").arg(index));
    keyframe->setAccessibleName(tr("Toggle keyframe for %1").arg(parameter.displayName));
    keyframe->setToolTip(tr("Add or remove a keyframe at the current playhead"));
    keyframe->setText(QStringLiteral("◆"));
    keyframe->setCheckable(false);
    connect(keyframe, &QToolButton::clicked, this, [this, index] {
      if (index < effect_parameters_.size()) {
        emit effectKeyframeToggleRequested(effect_parameters_.at(index).effectId,
                                           effect_parameters_.at(index).parameterId);
      }
    });
    rowLayout->addWidget(keyframe);
    effect_parameter_form_->addRow(parameter.displayName, row);
  }
}

void InspectorWidget::selectEffectParameter(const int index) {
  if (index < 0 || index >= effect_parameters_.size()) {
    active_effect_parameter_ = -1;
    active_keyframe_ = -1;
    refreshKeyframeEditor();
    return;
  }
  active_effect_parameter_ = index;
  active_keyframe_ = -1;
  const auto& parameter = effect_parameters_.at(index);
  {
    const QSignalBlocker blocker(keyframe_list_);
    keyframe_list_->clear();
    for (const auto& keyframe : parameter.keyframes) {
      keyframe_list_->addItem(tr("%1 · %2")
                                  .arg(static_cast<double>(keyframe.time) / 48'000.0, 0, 'f', 3)
                                  .arg(keyframe.value, 0, 'f', 3));
    }
  }
  if (!parameter.keyframes.isEmpty()) {
    keyframe_list_->setCurrentRow(0);
    // The list is populated while its signals are blocked.  Select explicitly
    // so the editor state cannot diverge from QListWidget's visual selection.
    selectKeyframe(0);
  } else {
    refreshKeyframeEditor();
  }
}

void InspectorWidget::selectKeyframe(const int index) {
  if (active_effect_parameter_ < 0 || active_effect_parameter_ >= effect_parameters_.size() ||
      index < 0 || index >= effect_parameters_.at(active_effect_parameter_).keyframes.size()) {
    active_keyframe_ = -1;
    refreshKeyframeEditor();
    return;
  }
  active_keyframe_ = index;
  const auto& parameter = effect_parameters_.at(active_effect_parameter_);
  const auto& keyframe = parameter.keyframes.at(index);
  emit effectKeyframeSelected(parameter.effectId, parameter.parameterId, keyframe.time);
  refreshKeyframeEditor();
}

void InspectorWidget::refreshKeyframeEditor() {
  const bool enabled =
      active_effect_parameter_ >= 0 && active_effect_parameter_ < effect_parameters_.size() &&
      active_keyframe_ >= 0 &&
      active_keyframe_ < effect_parameters_.at(active_effect_parameter_).keyframes.size();
  keyframe_time_->setEnabled(enabled);
  keyframe_value_->setEnabled(enabled);
  keyframe_interpolation_->setEnabled(enabled);
  keyframe_delete_->setEnabled(enabled);
  if (!enabled) {
    keyframe_curve_->setKeyframes({}, 1, 0.0, 1.0);
    return;
  }
  const auto& parameter = effect_parameters_.at(active_effect_parameter_);
  const auto& keyframe = parameter.keyframes.at(active_keyframe_);
  double curve_minimum = keyframe.value;
  double curve_maximum = keyframe.value;
  for (const auto& candidate : parameter.keyframes) {
    curve_minimum = std::min(curve_minimum, candidate.value);
    curve_maximum = std::max(curve_maximum, candidate.value);
  }
  if (std::abs(curve_maximum - curve_minimum) < 1.0e-9) {
    const double padding = std::max(1.0, std::abs(curve_minimum) * 0.1);
    curve_minimum -= padding;
    curve_maximum += padding;
  } else {
    const double padding = (curve_maximum - curve_minimum) * 0.1;
    curve_minimum -= padding;
    curve_maximum += padding;
  }
  {
    const QSignalBlocker timeBlocker(keyframe_time_);
    const QSignalBlocker valueBlocker(keyframe_value_);
    const QSignalBlocker interpolationBlocker(keyframe_interpolation_);
    keyframe_time_->setValue(static_cast<double>(keyframe.time) / 48'000.0);
    keyframe_value_->setValue(keyframe.value);
    keyframe_interpolation_->setCurrentIndex(
        keyframe_interpolation_->findData(QVariant::fromValue(keyframe.interpolation)));
  }
  keyframe_curve_->setKeyframes(parameter.keyframes, parameter.duration, curve_minimum,
                                curve_maximum);
  keyframe_curve_->setSelectedKeyframe(keyframe.id);
}

void InspectorWidget::clearSelection() {
  setEffectParameters({});
  setSelectionName({});
  clearAssetMetadata();
}

EffectsPanelWidget::EffectsPanelWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("effectsPanel"));
  setAccessibleName(tr("Effects"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(8);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("effectsSearch"));
  search_->setAccessibleName(tr("Search effects"));
  search_->setPlaceholderText(tr("Search effects and transitions…"));
  search_->setClearButtonEnabled(true);
  layout->addWidget(search_);

  list_ = new QListWidget(this);
  list_->setObjectName(QStringLiteral("effectsList"));
  list_->setAccessibleName(tr("Available effects and transitions"));
  list_->setUniformItemSizes(true);
  layout->addWidget(list_, 1);

  setEffects({
      {QStringLiteral("transition.cross_dissolve"), tr("Cross Dissolve"), tr("Transitions"), true},
      {QStringLiteral("transition.dip_to_black"), tr("Dip to Black"), tr("Transitions"), true},
      {QStringLiteral("video.color"), tr("Color Adjustments"), tr("Video"), true},
      {QStringLiteral("video.curves"), tr("Color Curves"), tr("Video"), true},
      {QStringLiteral("video.lut"), tr("LUT"), tr("Video"), true},
      {QStringLiteral("video.crop"), tr("Crop"), tr("Video"), true},
      {QStringLiteral("video.gaussian_blur"), tr("Gaussian Blur"), tr("Video"), true},
  });

  connect(search_, &QLineEdit::textChanged, this, &EffectsPanelWidget::applyFilter);
  connect(list_, &QListWidget::itemDoubleClicked, this, [this] { activateCurrent(); });
  connect(list_, &QListWidget::itemActivated, this, [this] { activateCurrent(); });
}

void EffectsPanelWidget::setEffects(const QVector<EffectView>& effects) {
  effects_ = effects;
  rebuild();
}

void EffectsPanelWidget::applyFilter(const QString& query) {
  Q_UNUSED(query)
  rebuild();
}

void EffectsPanelWidget::activateCurrent() {
  auto* item = list_->currentItem();
  if (item == nullptr || item->data(Qt::UserRole).toString().isEmpty()) {
    return;
  }
  const auto id = item->data(Qt::UserRole).toString();
  emit effectActivated(id);
  emit effectAddRequested(id);
}

void EffectsPanelWidget::rebuild() {
  list_->clear();
  const auto query = search_->text().trimmed();
  QString previousCategory;
  for (const auto& effect : effects_) {
    if (!query.isEmpty() && !effect.displayName.contains(query, Qt::CaseInsensitive) &&
        !effect.category.contains(query, Qt::CaseInsensitive)) {
      continue;
    }
    if (effect.category != previousCategory) {
      auto* category = new QListWidgetItem(effect.category, list_);
      QFont font = category->font();
      font.setWeight(QFont::DemiBold);
      category->setFont(font);
      category->setForeground(QColor{146, 153, 166});
      category->setFlags(Qt::NoItemFlags);
      previousCategory = effect.category;
    }
    auto* item = new QListWidgetItem(effect.displayName, list_);
    item->setData(Qt::UserRole, effect.id);
    item->setToolTip(effect.accelerated ? tr("GPU accelerated · Double-click to add")
                                        : tr("Double-click to add"));
  }
}

AudioMixerWidget::AudioMixerWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("audioMixerPanel"));
  setAccessibleName(tr("Audio mixer"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);

  auto* master = new QGroupBox(tr("Master"), this);
  master->setObjectName(QStringLiteral("masterAudioStrip"));
  master->setAccessibleName(tr("Master audio meter"));
  auto* masterLayout = new QHBoxLayout(master);
  master_peak_ = new QLabel(tr("Peak: —"), master);
  master_peak_->setObjectName(QStringLiteral("masterPeakMeter"));
  master_rms_ = new QLabel(tr("RMS: —"), master);
  master_rms_->setObjectName(QStringLiteral("masterRmsMeter"));
  master_lufs_ = new QLabel(tr("LUFS-I: —"), master);
  master_lufs_->setObjectName(QStringLiteral("masterLufsMeter"));
  masterLayout->addWidget(master_peak_);
  masterLayout->addWidget(master_rms_);
  masterLayout->addWidget(master_lufs_);
  masterLayout->addStretch();
  layout->addWidget(master);

  auto* deviceRow = new QHBoxLayout;
  deviceRow->addWidget(new QLabel(tr("Output device"), this));
  device_selector_ = new QComboBox(this);
  device_selector_->setObjectName(QStringLiteral("audioOutputDevice"));
  device_selector_->setAccessibleName(tr("Audio output device"));
  deviceRow->addWidget(device_selector_, 1);
  device_selector_->addItem(tr("System default"), QString{});
  device_status_ = new QLabel(tr("Detecting devices…"), this);
  device_status_->setObjectName(QStringLiteral("audioDeviceStatus"));
  device_status_->setProperty("muted", true);
  deviceRow->addWidget(device_status_);
  calibrate_latency_ = new QPushButton(tr("Calibrate"), this);
  calibrate_latency_->setObjectName(QStringLiteral("audioOutputCalibrate"));
  calibrate_latency_->setAccessibleName(tr("Calibrate output latency"));
  deviceRow->addWidget(calibrate_latency_);
  calibrated_latency_label_ = new QLabel(this);
  calibrated_latency_label_->setObjectName(QStringLiteral("audioCalibratedLatency"));
  calibrated_latency_label_->setProperty("muted", true);
  calibrated_latency_label_->hide();
  deviceRow->addWidget(calibrated_latency_label_);
  layout->addLayout(deviceRow);
  connect(device_selector_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (index >= 0) {
      emit outputDeviceSelected(device_selector_->itemData(index).toString());
    }
  });
  connect(calibrate_latency_, &QPushButton::clicked, this,
          &AudioMixerWidget::calibrateOutputLatencyRequested);

  auto* normalize = new QGroupBox(tr("Loudness normalization"), this);
  normalize->setObjectName(QStringLiteral("loudnessNormalization"));
  auto* normalizeLayout = new QHBoxLayout(normalize);
  normalization_status_ =
      new QLabel(tr("Analyze the sequence to review integrated LUFS."), normalize);
  normalization_status_->setObjectName(QStringLiteral("normalizationStatus"));
  normalization_status_->setWordWrap(true);
  normalize->setAccessibleName(tr("Loudness normalization"));
  normalization_analyze_ = new QPushButton(tr("Analyze"), normalize);
  normalization_analyze_->setObjectName(QStringLiteral("normalizationAnalyze"));
  normalization_analyze_->setAccessibleName(tr("Analyze loudness"));
  normalization_apply_ = new QPushButton(tr("Apply"), normalize);
  normalization_apply_->setObjectName(QStringLiteral("normalizationApply"));
  normalization_apply_->setAccessibleName(tr("Apply loudness normalization"));
  normalization_apply_->setEnabled(false);
  normalization_target_ = new QDoubleSpinBox(normalize);
  normalization_target_->setObjectName(QStringLiteral("normalizationTargetLufs"));
  normalization_target_->setAccessibleName(tr("Normalization target LUFS"));
  normalization_target_->setRange(-24.0, -9.0);
  normalization_target_->setValue(-14.0);
  normalization_target_->setDecimals(1);
  normalization_target_->setSuffix(tr(" LUFS"));
  normalization_target_->setKeyboardTracking(false);
  normalizeLayout->addWidget(normalization_status_, 1);
  normalizeLayout->addWidget(new QLabel(tr("Target"), normalize));
  normalizeLayout->addWidget(normalization_target_);
  normalizeLayout->addWidget(normalization_analyze_);
  normalizeLayout->addWidget(normalization_apply_);
  layout->addWidget(normalize);
  connect(normalization_analyze_, &QPushButton::clicked, this,
          &AudioMixerWidget::normalizationAnalyzeRequested);
  connect(normalization_apply_, &QPushButton::clicked, this,
          &AudioMixerWidget::normalizationApplyRequested);
  connect(normalization_target_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          &AudioMixerWidget::normalizationTargetChanged);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("mixerScrollArea"));
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  strips_ = new QWidget(scroll);
  strips_->setObjectName(QStringLiteral("mixerStrips"));
  scroll->setWidget(strips_);
  layout->addWidget(scroll, 1);

  setTracks({});
}

namespace {

bool effectsMatchStructure(const QVector<AudioTrackView::Effect>& left,
                           const QVector<AudioTrackView::Effect>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (int index = 0; index < left.size(); ++index) {
    if (left.at(index).id != right.at(index).id) {
      return false;
    }
    const auto& leftParameters = left.at(index).parameters;
    const auto& rightParameters = right.at(index).parameters;
    if (leftParameters.size() != rightParameters.size()) {
      return false;
    }
    for (int parameterIndex = 0; parameterIndex < leftParameters.size(); ++parameterIndex) {
      if (leftParameters.at(parameterIndex).id != rightParameters.at(parameterIndex).id) {
        return false;
      }
    }
  }
  return true;
}

void updatePanLabel(QLabel* panLabel, const int panValue) {
  if (panValue == 0) {
    panLabel->setText(QObject::tr("C"));
  } else if (panValue < 0) {
    panLabel->setText(QStringLiteral("L%1").arg(std::abs(panValue)));
  } else {
    panLabel->setText(QStringLiteral("R%1").arg(panValue));
  }
}

void discardMixerStripWidget(QWidget* widget) {
  widget->hide();
  widget->setObjectName(QString{});
  if (widget->property("trackId").isValid()) {
    widget->setProperty("trackId", QVariant{});
  }
  for (QObject* child : widget->children()) {
    if (auto* childWidget = qobject_cast<QWidget*>(child)) {
      discardMixerStripWidget(childWidget);
    }
  }
  widget->deleteLater();
}

}  // namespace

bool AudioMixerWidget::canUpdateStripsInPlace(const QVector<AudioTrackView>& tracks) const {
  if (tracks.size() != tracks_.size()) {
    return false;
  }
  if (tracks.isEmpty()) {
    return strips_->findChild<QLabel*>(QStringLiteral("mixerEmptyState"),
                                       Qt::FindDirectChildrenOnly) != nullptr;
  }
  if (strips_->findChild<QLabel*>(QStringLiteral("mixerEmptyState"),
                                   Qt::FindDirectChildrenOnly) != nullptr) {
    return false;
  }
  for (int index = 0; index < tracks.size(); ++index) {
    if (tracks.at(index).id != tracks_.at(index).id) {
      return false;
    }
    if (!effectsMatchStructure(tracks.at(index).effects, tracks_.at(index).effects)) {
      return false;
    }
    auto* strip =
        strips_->findChild<QGroupBox*>(QStringLiteral("mixerStrip.%1").arg(index),
                                       Qt::FindDirectChildrenOnly);
    if (strip == nullptr) {
      return false;
    }
    auto* meter = strip->findChild<QProgressBar*>(QStringLiteral("audioMeter.%1").arg(index));
    if (meter == nullptr || meter->property("trackId").toString() != tracks.at(index).id) {
      return false;
    }
  }
  return strips_->findChild<QGroupBox*>(QStringLiteral("mixerStrip.%1").arg(tracks.size()),
                                        Qt::FindDirectChildrenOnly) == nullptr;
}

void AudioMixerWidget::updateStripsInPlace(const QVector<AudioTrackView>& tracks) {
  for (int index = 0; index < tracks.size(); ++index) {
    const AudioTrackView& track = tracks.at(index);
    auto* strip = strips_->findChild<QGroupBox*>(QStringLiteral("mixerStrip.%1").arg(index));
    strip->setTitle(track.displayName);

    auto* fader = strip->findChild<QSlider*>(QStringLiteral("audioFader.%1").arg(index));
    auto* gainLabel = strip->findChild<QLabel*>(QStringLiteral("mixerGainValue.%1").arg(index));
    auto* pan = strip->findChild<QSlider*>(QStringLiteral("audioPan.%1").arg(index));
    auto* panLabel = strip->findChild<QLabel*>(QStringLiteral("mixerPanValue.%1").arg(index));
    auto* mute = strip->findChild<QToolButton*>(QStringLiteral("mixerMute.%1").arg(index));
    auto* solo = strip->findChild<QToolButton*>(QStringLiteral("mixerSolo.%1").arg(index));

    const int gainValue = static_cast<int>(std::round(track.gain_db));
    {
      const QSignalBlocker blocker(fader);
      fader->setValue(gainValue);
    }
    gainLabel->setText(QStringLiteral("%1 dB").arg(track.gain_db, 0, 'f', 1));

    const int panValue = static_cast<int>(std::round(track.pan * 100.0));
    {
      const QSignalBlocker blocker(pan);
      pan->setValue(panValue);
    }
    updatePanLabel(panLabel, panValue);

    {
      const QSignalBlocker muteBlocker(mute);
      mute->setChecked(track.muted);
    }
    {
      const QSignalBlocker soloBlocker(solo);
      solo->setChecked(track.soloed);
    }

    for (const auto& effect : track.effects) {
      for (const auto& parameter : effect.parameters) {
        auto* spin = strip->findChild<QDoubleSpinBox*>(QStringLiteral("trackEffectParameter.%1.%2.%3")
                                                           .arg(index)
                                                           .arg(effect.id)
                                                           .arg(parameter.id));
        if (spin == nullptr) {
          continue;
        }
        const QSignalBlocker blocker(spin);
        spin->setValue(parameter.value.toDouble());
      }
    }
  }
}

void AudioMixerWidget::discardStripWidgets() {
  if (QLayout* layout = strips_->layout()) {
    while (QLayoutItem* item = layout->takeAt(0)) {
      if (QWidget* widget = item->widget()) {
        discardMixerStripWidget(widget);
      }
      delete item;
    }
    delete layout;
    return;
  }
  const auto children = strips_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
  for (QWidget* child : children) {
    discardMixerStripWidget(child);
  }
}

void AudioMixerWidget::buildStrips(const QVector<AudioTrackView>& tracks) {
  auto* layout = new QHBoxLayout(strips_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  if (tracks.isEmpty()) {
    auto* empty = makeMutedLabel(tr("No audio tracks in this sequence."), strips_);
    empty->setObjectName(QStringLiteral("mixerEmptyState"));
    layout->addWidget(empty);
  }
  for (int index = 0; index < tracks.size(); ++index) {
    const AudioTrackView& track = tracks.at(index);
    auto* strip = new QGroupBox(track.displayName, strips_);
    strip->setObjectName(QStringLiteral("mixerStrip.%1").arg(index));
    strip->setAccessibleName(tr("Mixer strip %1").arg(track.displayName));
    strip->setMinimumWidth(112);
    auto* stripLayout = new QVBoxLayout(strip);

    auto* meter = new QProgressBar(strip);
    meter->setObjectName(QStringLiteral("audioMeter.%1").arg(index));
    meter->setProperty("trackId", track.id);
    meter->setAccessibleName(tr("Level meter for %1").arg(track.displayName));
    meter->setOrientation(Qt::Vertical);
    meter->setRange(0, 60);
    float initial_loudest = -60.0F;
    for (const float value : track.peak_dbfs) {
      initial_loudest = std::max(initial_loudest, value);
    }
    meter->setValue(track.meter_active && !track.meter_stale
                        ? static_cast<int>(std::clamp(initial_loudest + 60.0F, 0.0F, 60.0F))
                        : 0);
    meter->setTextVisible(false);
    meter->setEnabled(track.meter_active && !track.meter_stale);
    meter->setToolTip(track.meter_active && !track.meter_stale
                          ? tr("Peak/RMS telemetry is live")
                          : tr("Per-track telemetry is inactive"));
    stripLayout->addWidget(meter, 1, Qt::AlignHCenter);

    auto* fader = new QSlider(Qt::Vertical, strip);
    fader->setObjectName(QStringLiteral("audioFader.%1").arg(index));
    fader->setAccessibleName(tr("Gain for %1").arg(track.displayName));
    fader->setRange(-96, 24);
    fader->setValue(static_cast<int>(std::round(track.gain_db)));
    fader->setTickPosition(QSlider::TicksBothSides);
    fader->setTickInterval(12);
    fader->setEnabled(true);
    fader->setToolTip(tr("Track gain in decibels."));
    stripLayout->addWidget(fader, 2, Qt::AlignHCenter);

    auto* value = new QLabel(QStringLiteral("%1 dB").arg(track.gain_db, 0, 'f', 1), strip);
    value->setObjectName(QStringLiteral("mixerGainValue.%1").arg(index));
    value->setAlignment(Qt::AlignCenter);
    stripLayout->addWidget(value);

    auto* pan = new QSlider(Qt::Horizontal, strip);
    pan->setObjectName(QStringLiteral("audioPan.%1").arg(index));
    pan->setAccessibleName(tr("Pan for %1").arg(track.displayName));
    pan->setRange(-100, 100);
    pan->setValue(static_cast<int>(std::round(track.pan * 100.0)));
    pan->setTickPosition(QSlider::TicksBothSides);
    pan->setTickInterval(50);
    auto* pan_label = new QLabel(QStringLiteral("C"), strip);
    pan_label->setObjectName(QStringLiteral("mixerPanValue.%1").arg(index));
    pan_label->setAlignment(Qt::AlignCenter);
    auto* pan_row = new QHBoxLayout;
    pan_row->addWidget(new QLabel(QStringLiteral("L"), strip));
    pan_row->addWidget(pan, 1);
    pan_row->addWidget(new QLabel(QStringLiteral("R"), strip));
    stripLayout->addLayout(pan_row);
    stripLayout->addWidget(pan_label);

    auto* buttons = new QHBoxLayout;
    auto* mute = new QToolButton(strip);
    mute->setText(tr("M"));
    mute->setCheckable(true);
    mute->setObjectName(QStringLiteral("mixerMute.%1").arg(index));
    mute->setAccessibleName(tr("Mute %1").arg(track.displayName));
    mute->setChecked(track.muted);
    auto* solo = new QToolButton(strip);
    solo->setText(tr("S"));
    solo->setCheckable(true);
    solo->setObjectName(QStringLiteral("mixerSolo.%1").arg(index));
    solo->setAccessibleName(tr("Solo %1").arg(track.displayName));
    solo->setChecked(track.soloed);
    buttons->addWidget(mute);
    buttons->addWidget(solo);
    stripLayout->addLayout(buttons);

    if (!track.effects.isEmpty()) {
      auto* effects = new QGroupBox(tr("Track effects"), strip);
      effects->setCheckable(true);
      effects->setChecked(false);
      effects->setAccessibleDescription(tr("Expandable EQ, dynamics, and noise controls"));
      auto* effectsLayout = new QVBoxLayout(effects);
      for (const auto& effect : track.effects) {
        auto* effectBox = new QGroupBox(effect.displayName, effects);
        auto* effectLayout = new QFormLayout(effectBox);
        auto* remove = new QToolButton(effectBox);
        remove->setText(tr("Remove"));
        remove->setAccessibleName(tr("Remove %1").arg(effect.displayName));
        effectLayout->addRow(remove);
        connect(remove, &QToolButton::clicked, this, [this, index, effectId = effect.id] {
          emit trackEffectRemoveRequested(index, effectId);
        });
        for (const auto& parameter : effect.parameters) {
          const QString& parameterId = parameter.id;
          auto* spin = new QDoubleSpinBox(effectBox);
          spin->setObjectName(QStringLiteral("trackEffectParameter.%1.%2.%3")
                                  .arg(index)
                                  .arg(effect.id)
                                  .arg(parameterId));
          spin->setAccessibleName(tr("%1 %2").arg(effect.displayName, parameterId));
          spin->setDecimals(2);
          spin->setKeyboardTracking(false);
          if (parameterId == QStringLiteral("ratio")) {
            spin->setRange(1.0, 20.0);
          } else if (parameterId == QStringLiteral("quality")) {
            spin->setRange(0.1, 20.0);
          } else if (parameterId.contains(QStringLiteral("frequency"))) {
            spin->setRange(20.0, 20'000.0);
          } else if (parameterId.contains(QStringLiteral("attack"))) {
            spin->setRange(0.1, 200.0);
          } else if (parameterId.contains(QStringLiteral("release"))) {
            spin->setRange(1.0, 2'000.0);
          } else if (parameterId.contains(QStringLiteral("threshold"))) {
            spin->setRange(-96.0, 0.0);
          } else if (parameterId.contains(QStringLiteral("makeup")) ||
                     parameterId.contains(QStringLiteral("gain"))) {
            spin->setRange(-96.0, 24.0);
          } else if (parameterId.contains(QStringLiteral("strength"))) {
            spin->setRange(0.0, 1.0);
          } else if (parameterId.contains(QStringLiteral("ceiling"))) {
            spin->setRange(-24.0, 0.0);
          } else {
            spin->setRange(-96.0, 24.0);
          }
          {
            const QSignalBlocker blocker(spin);
            spin->setValue(parameter.value.toDouble());
          }
          effectLayout->addRow(parameterId, spin);
          connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                  [this, index, effectId = effect.id, parameterId](double value) {
                    emit trackEffectParameterEdited(index, effectId, parameterId, value);
                  });
        }
        effectsLayout->addWidget(effectBox);
      }
      effectsLayout->addStretch();
      stripLayout->addWidget(effects);
    }
    auto* addEffect = new QComboBox(strip);
    addEffect->setObjectName(QStringLiteral("trackEffectAdd.%1").arg(index));
    addEffect->setAccessibleName(tr("Add effect to %1").arg(track.displayName));
    addEffect->addItem(tr("Add effect…"), QString{});
    addEffect->addItem(tr("Parametric EQ"), QStringLiteral("audio.eq"));
    addEffect->addItem(tr("Compressor"), QStringLiteral("audio.compressor"));
    addEffect->addItem(tr("Dialogue noise reduction"), QStringLiteral("audio.dialogue_denoise"));
    addEffect->addItem(tr("Limiter"), QStringLiteral("audio.limiter"));
    stripLayout->addWidget(addEffect);
    connect(addEffect, &QComboBox::currentIndexChanged, this, [this, index, addEffect](int item) {
      const QString id = addEffect->itemData(item).toString();
      if (!id.isEmpty()) {
        emit trackEffectAddRequested(index, id);
        addEffect->setCurrentIndex(0);
      }
    });

    connect(fader, &QSlider::valueChanged, this, [this, index, value](int gain) {
      value->setText(QStringLiteral("%1 dB").arg(static_cast<double>(gain), 0, 'f', 1));
      emit gainEdited(index, static_cast<double>(gain));
    });
    connect(pan, &QSlider::valueChanged, this, [this, index, pan_label](int pan_value) {
      const double pan_normalized = static_cast<double>(pan_value) / 100.0;
      if (pan_value == 0) {
        pan_label->setText(tr("C"));
      } else if (pan_value < 0) {
        pan_label->setText(QStringLiteral("L%1").arg(std::abs(pan_value)));
      } else {
        pan_label->setText(QStringLiteral("R%1").arg(pan_value));
      }
      emit panEdited(index, pan_normalized);
    });
    connect(mute, &QToolButton::toggled, this,
            [this, index](bool muted) { emit muteToggled(index, muted); });
    connect(solo, &QToolButton::toggled, this,
            [this, index](bool soloed) { emit soloToggled(index, soloed); });
    layout->addWidget(strip);
  }
  layout->addStretch();
}

void AudioMixerWidget::rebuildStrips(const QVector<AudioTrackView>& tracks) {
  discardStripWidgets();
  buildStrips(tracks);
}

void AudioMixerWidget::setTrackNames(const QStringList& names) {
  QVector<AudioTrackView> tracks;
  tracks.reserve(names.size());
  for (const QString& name : names) {
    tracks.push_back({.id = {}, .displayName = name, .effects = {}});
  }
  setTracks(tracks);
}

void AudioMixerWidget::setTracks(const QVector<AudioTrackView>& tracks) {
  if (canUpdateStripsInPlace(tracks)) {
    if (tracks.isEmpty()) {
      tracks_ = tracks;
      return;
    }
    updateStripsInPlace(tracks);
    tracks_ = tracks;
    return;
  }
  rebuildStrips(tracks);
  tracks_ = tracks;
}

void AudioMixerWidget::setMeterLevels(const int trackIndex, const QVector<float>& peakDbfs) {
  if (trackIndex < 0) {
    return;
  }
  // The meter widget range is 0..60 where 0 = -60 dBFS and 60 = 0 dBFS.
  // Values below -60 clamp to 0 (silent); values above 0 clamp to 60.
  auto* meter = findChild<QProgressBar*>(QStringLiteral("audioMeter.%1").arg(trackIndex));
  if (meter == nullptr) {
    return;
  }
  if (peakDbfs.isEmpty()) {
    meter->setValue(0);
    return;
  }
  // Show the louder of the channels for a single-bar meter.
  float loudest = peakDbfs.first();
  for (float v : peakDbfs) {
    loudest = std::max(loudest, v);
  }
  const int scaled = static_cast<int>(std::clamp(loudest + 60.0F, 0.0F, 60.0F));
  meter->setValue(scaled);
}

void AudioMixerWidget::setTrackMeters(const QVector<AudioTrackMeterView>& meters) {
  for (auto* candidate : findChildren<QProgressBar*>()) {
    if (!candidate->property("trackId").isValid()) {
      continue;
    }
    candidate->setValue(0);
    candidate->setEnabled(false);
    candidate->setToolTip(tr("Per-track telemetry is inactive"));
  }
  for (const auto& meter_view : meters) {
    QProgressBar* meter = nullptr;
    for (auto* candidate : findChildren<QProgressBar*>()) {
      if (candidate->property("trackId").toString() == meter_view.id) {
        meter = candidate;
        break;
      }
    }
    if (meter == nullptr) {
      continue;
    }
    const bool active = meter_view.active && !meter_view.stale;
    meter->setEnabled(active);
    float loudest = -60.0F;
    for (const float value : meter_view.peak_dbfs) {
      loudest = std::max(loudest, value);
    }
    meter->setValue(active ? static_cast<int>(std::clamp(loudest + 60.0F, 0.0F, 60.0F)) : 0);
    const float peak = std::max(meter_view.peak_dbfs[0], meter_view.peak_dbfs[1]);
    const float rms = std::max(meter_view.rms_dbfs[0], meter_view.rms_dbfs[1]);
    meter->setToolTip(
        active ? tr("Peak: %1 dBFS · RMS: %2 dBFS").arg(peak, 0, 'f', 1).arg(rms, 0, 'f', 1)
               : (meter_view.stale ? tr("Per-track telemetry is stale") : tr("Track inactive")));
  }
}

void AudioMixerWidget::setMasterMeter(const float peakDbfs, const float rmsDbfs, const double lufs,
                                      const bool active, const bool lufsValid,
                                      const bool lufsStale) {
  const QString unavailable = active ? QStringLiteral("—") : tr("inactive");
  master_peak_->setText(active ? tr("Peak: %1 dBFS").arg(peakDbfs, 0, 'f', 1)
                               : tr("Peak: %1").arg(unavailable));
  master_rms_->setText(active ? tr("RMS: %1 dBFS").arg(rmsDbfs, 0, 'f', 1)
                              : tr("RMS: %1").arg(unavailable));
  const QString lufsText =
      !active ? tr("LUFS-I: inactive")
              : (lufsValid && !lufsStale && std::isfinite(lufs)
                     ? tr("LUFS-I: %1").arg(lufs, 0, 'f', 1)
                     : (lufsStale ? tr("LUFS-I: stale") : tr("LUFS-I: analyzing")));
  master_lufs_->setText(lufsText);
}

void AudioMixerWidget::setOutputDevices(const QStringList& ids, const QStringList& names,
                                        const QString& selectedId, const bool available,
                                        const QString& status) {
  const QSignalBlocker blocker(device_selector_);
  device_selector_->clear();
  device_selector_->addItem(tr("System default"), QString{});
  for (int index = 0; index < ids.size() && index < names.size(); ++index) {
    device_selector_->addItem(names.at(index), ids.at(index));
  }
  const int selected = device_selector_->findData(selectedId);
  if (selected >= 0) {
    device_selector_->setCurrentIndex(selected);
  }
  device_selector_->setEnabled(available && device_selector_->count() > 0);
  device_status_->setText(status.isEmpty() ? (available ? tr("Ready") : tr("Unavailable"))
                                           : status);
  if (calibrate_latency_ != nullptr) {
    calibrate_latency_->setEnabled(available && device_selector_->count() > 0);
  }
}

void AudioMixerWidget::setCalibratedLatencyFrames(const std::optional<std::uint64_t> frames) {
  if (calibrated_latency_label_ == nullptr) {
    return;
  }
  if (!frames.has_value()) {
    calibrated_latency_label_->hide();
    calibrated_latency_label_->clear();
    return;
  }
  calibrated_latency_label_->setText(
      tr("Calibrated: %1 frames").arg(QString::number(*frames)));
  calibrated_latency_label_->show();
}

void AudioMixerWidget::setCalibrationBusy(const bool busy) {
  if (calibrate_latency_ != nullptr) {
    calibrate_latency_->setEnabled(!busy && device_selector_ != nullptr &&
                                   device_selector_->isEnabled());
  }
}

void AudioMixerWidget::setNormalizationReview(const double measuredLufs, const double gainDb,
                                              const double targetLufs) {
  normalization_status_->setText(tr("Measured %1 LUFS · target %2 LUFS · proposed gain %3 dB")
                                     .arg(measuredLufs, 0, 'f', 1)
                                     .arg(targetLufs, 0, 'f', 1)
                                     .arg(gainDb, 0, 'f', 1));
  normalization_apply_->setEnabled(true);
}

void AudioMixerWidget::setNormalizationBusy(const bool busy) {
  normalization_analyze_->setEnabled(!busy);
  if (busy) {
    normalization_apply_->setEnabled(false);
    normalization_status_->setText(tr("Analyzing sequence loudness…"));
  }
}

void AudioMixerWidget::setNormalizationStatus(const QString& status) {
  normalization_status_->setText(status);
  normalization_apply_->setEnabled(false);
}

double AudioMixerWidget::normalizationTargetLufs() const {
  return normalization_target_ != nullptr ? normalization_target_->value() : -14.0;
}

void AudioMixerWidget::setNormalizationTargetLufs(const double targetLufs) {
  if (normalization_target_ == nullptr) {
    return;
  }
  const QSignalBlocker blocker(normalization_target_);
  normalization_target_->setValue(std::clamp(targetLufs, -24.0, -9.0));
}

CaptionsPanelWidget::CaptionsPanelWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("captionsPanel"));
  setAccessibleName(tr("Captions and transcript"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(8);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("transcriptSearch"));
  search_->setAccessibleName(tr("Search transcript"));
  search_->setPlaceholderText(tr("Search transcript…"));
  search_->setClearButtonEnabled(true);
  layout->addWidget(search_);

  auto* transcription = new QGroupBox(tr("Local transcription"), this);
  transcription->setObjectName(QStringLiteral("transcriptionOptionsGroup"));
  transcription->setCheckable(true);
  transcription->setChecked(false);
  auto* transcriptionLayout = new QVBoxLayout(transcription);
  auto* options = new QFormLayout;
  language_ = new QComboBox(transcription);
  language_->setObjectName(QStringLiteral("transcriptionLanguage"));
  language_->setAccessibleName(tr("Transcription language"));
  language_->addItem(tr("Detect language"), QString{});
  language_->addItem(tr("English"), QStringLiteral("en"));
  language_->addItem(tr("French"), QStringLiteral("fr"));
  language_->addItem(tr("German"), QStringLiteral("de"));
  language_->addItem(tr("Spanish"), QStringLiteral("es"));
  language_->addItem(tr("Japanese"), QStringLiteral("ja"));
  options->addRow(tr("Language"), language_);
  translate_ = new QCheckBox(tr("Translate to English"), transcription);
  translate_->setObjectName(QStringLiteral("transcriptionTranslate"));
  translate_->setAccessibleName(tr("Translate transcript to English"));
  options->addRow(QString{}, translate_);
  prefer_vulkan_ = new QCheckBox(tr("Prefer Vulkan acceleration"), transcription);
  prefer_vulkan_->setObjectName(QStringLiteral("transcriptionVulkan"));
  prefer_vulkan_->setAccessibleName(tr("Prefer Vulkan acceleration when available"));
  options->addRow(QString{}, prefer_vulkan_);
  word_timestamps_ = new QCheckBox(tr("Keep word timings"), transcription);
  word_timestamps_->setObjectName(QStringLiteral("transcriptionWordTimings"));
  word_timestamps_->setAccessibleName(tr("Keep word-level timings"));
  word_timestamps_->setChecked(true);
  word_timestamps_->setEnabled(false);
  options->addRow(QString{}, word_timestamps_);
  thread_count_ = new QSpinBox(transcription);
  thread_count_->setObjectName(QStringLiteral("transcriptionThreadCount"));
  thread_count_->setAccessibleName(tr("Transcription worker threads"));
  thread_count_->setRange(0, 64);
  thread_count_->setSpecialValueText(tr("Automatic"));
  options->addRow(tr("Threads"), thread_count_);
  transcriptionLayout->addLayout(options);

  auto* modelRow = new QHBoxLayout;
  transcription_status_ =
      new QLabel(tr("Model not installed; download is optional."), transcription);
  transcription_status_->setObjectName(QStringLiteral("transcriptionStatus"));
  transcription_status_->setAccessibleName(tr("Transcription model status"));
  transcription_status_->setWordWrap(true);
  modelRow->addWidget(transcription_status_, 1);
  model_download_ = new QPushButton(tr("Download model…"), transcription);
  model_download_->setObjectName(QStringLiteral("downloadTranscriptionModelButton"));
  model_download_->setAccessibleName(tr("Download the optional transcription model"));
  modelRow->addWidget(model_download_);
  transcriptionLayout->addLayout(modelRow);
  transcription_progress_ = new QProgressBar(transcription);
  transcription_progress_->setObjectName(QStringLiteral("transcriptionProgress"));
  transcription_progress_->setAccessibleName(tr("Transcription progress"));
  transcription_progress_->setRange(0, 100);
  transcription_progress_->setTextVisible(true);
  transcription_progress_->hide();
  transcriptionLayout->addWidget(transcription_progress_);
  auto* transcriptionButtons = new QHBoxLayout;
  transcribe_ = new QPushButton(tr("Transcribe selected audio"), transcription);
  transcribe_->setObjectName(QStringLiteral("transcribeButton"));
  transcribe_->setAccessibleName(tr("Transcribe the selected audio clip locally"));
  transcribe_cancel_ = new QPushButton(tr("Cancel"), transcription);
  transcribe_cancel_->setObjectName(QStringLiteral("cancelTranscriptionButton"));
  transcribe_cancel_->setAccessibleName(tr("Cancel transcription or model download"));
  transcribe_cancel_->hide();
  transcriptionButtons->addWidget(transcribe_);
  transcriptionButtons->addWidget(transcribe_cancel_);
  transcriptionLayout->addLayout(transcriptionButtons);
  layout->addWidget(transcription);

  content_ = new QStackedWidget(this);
  auto* empty = new QWidget(content_);
  auto* emptyLayout = new QVBoxLayout(empty);
  emptyLayout->addStretch();
  auto* heading = new QLabel(tr("Create captions in a few clicks"), empty);
  heading->setAlignment(Qt::AlignCenter);
  QFont font = heading->font();
  font.setWeight(QFont::DemiBold);
  heading->setFont(font);
  emptyLayout->addWidget(heading);
  emptyLayout->addWidget(
      makeMutedLabel(tr("Transcribe locally, or import an SRT or WebVTT file."), empty));
  auto* transcribe = new QPushButton(tr("Transcribe selected audio"), empty);
  transcribe->setObjectName(QStringLiteral("emptyTranscribeButton"));
  transcribe->setAccessibleName(tr("Transcribe the selected audio clip locally"));
  auto* import = new QPushButton(tr("Import captions…"), empty);
  import->setObjectName(QStringLiteral("importCaptionsButton"));
  import->setAccessibleName(tr("Import SRT or WebVTT captions"));
  auto* emptyAdd = new QPushButton(tr("Add caption at playhead"), empty);
  emptyAdd->setObjectName(QStringLiteral("emptyAddCaptionButton"));
  emptyAdd->setAccessibleName(tr("Add a caption at the playhead"));
  emptyLayout->addWidget(transcribe, 0, Qt::AlignHCenter);
  emptyLayout->addWidget(import, 0, Qt::AlignHCenter);
  emptyLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
  emptyLayout->addStretch();
  content_->addWidget(empty);

  auto* tablePage = new QWidget(content_);
  auto* tableLayout = new QVBoxLayout(tablePage);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  table_ = new QTableWidget(tablePage);
  table_->setObjectName(QStringLiteral("captionsTable"));
  table_->setAccessibleName(tr("Caption segments"));
  table_->setColumnCount(3);
  table_->setHorizontalHeaderLabels({tr("Time"), tr("Caption"), tr("Confidence")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setShowGrid(false);
  tableLayout->addWidget(table_, 1);

  auto* wordsGroup = new QGroupBox(tr("Words"), tablePage);
  wordsGroup->setObjectName(QStringLiteral("captionWordsGroup"));
  auto* wordsLayout = new QVBoxLayout(wordsGroup);
  words_ = new QListWidget(wordsGroup);
  words_->setObjectName(QStringLiteral("captionWordsList"));
  words_->setAccessibleName(tr("Word-level transcript navigation"));
  words_->setMaximumHeight(110);
  wordsLayout->addWidget(words_);
  tableLayout->addWidget(wordsGroup);

  auto* captionActions = new QHBoxLayout;
  auto* addCaption = new QPushButton(tr("Add at playhead"), tablePage);
  addCaption->setObjectName(QStringLiteral("addCaptionButton"));
  addCaption->setAccessibleName(tr("Add a caption at the playhead"));
  auto* removeCaption = new QPushButton(tr("Delete caption"), tablePage);
  removeCaption->setObjectName(QStringLiteral("removeCaptionButton"));
  removeCaption->setAccessibleName(tr("Delete the selected caption"));
  auto* exportCaptions = new QPushButton(tr("Export SRT or WebVTT…"), tablePage);
  exportCaptions->setObjectName(QStringLiteral("exportCaptionsButton"));
  exportCaptions->setAccessibleName(tr("Export sequence captions"));
  captionActions->addWidget(addCaption);
  captionActions->addWidget(removeCaption);
  captionActions->addStretch();
  captionActions->addWidget(exportCaptions);
  tableLayout->addLayout(captionActions);
  content_->addWidget(tablePage);
  layout->addWidget(content_, 1);

  auto* style = new QGroupBox(tr("Caption style"), this);
  style->setObjectName(QStringLiteral("captionStyleGroup"));
  style->setCheckable(true);
  style->setChecked(false);
  auto* styleForm = new QFormLayout(style);
  font_family_ = new QLineEdit(style);
  font_family_->setObjectName(QStringLiteral("captionFontFamily"));
  font_family_->setAccessibleName(tr("Caption font family"));
  font_family_->setText(QStringLiteral("sans-serif"));
  styleForm->addRow(tr("Font"), font_family_);
  font_size_ = new QDoubleSpinBox(style);
  font_size_->setObjectName(QStringLiteral("captionFontSize"));
  font_size_->setAccessibleName(tr("Caption font size"));
  font_size_->setRange(1.0, 4096.0);
  font_size_->setValue(48.0);
  font_size_->setSuffix(tr(" pt"));
  styleForm->addRow(tr("Size"), font_size_);
  alignment_ = new QComboBox(style);
  alignment_->setObjectName(QStringLiteral("captionAlignment"));
  alignment_->setAccessibleName(tr("Caption alignment"));
  alignment_->addItem(tr("Left"), QStringLiteral("left"));
  alignment_->addItem(tr("Center"), QStringLiteral("center"));
  alignment_->addItem(tr("Right"), QStringLiteral("right"));
  alignment_->setCurrentIndex(1);
  styleForm->addRow(tr("Alignment"), alignment_);
  vertical_position_ = new QDoubleSpinBox(style);
  vertical_position_->setObjectName(QStringLiteral("captionVerticalPosition"));
  vertical_position_->setAccessibleName(tr("Caption vertical position"));
  vertical_position_->setRange(0.0, 1.0);
  vertical_position_->setSingleStep(0.01);
  vertical_position_->setValue(0.9);
  styleForm->addRow(tr("Vertical position"), vertical_position_);
  safe_margin_ = new QDoubleSpinBox(style);
  safe_margin_->setObjectName(QStringLiteral("captionSafeMargin"));
  safe_margin_->setAccessibleName(tr("Caption safe margin"));
  safe_margin_->setRange(0.0, 0.5);
  safe_margin_->setSingleStep(0.01);
  safe_margin_->setValue(0.05);
  styleForm->addRow(tr("Safe margin"), safe_margin_);
  outline_width_ = new QDoubleSpinBox(style);
  outline_width_->setObjectName(QStringLiteral("captionOutlineWidth"));
  outline_width_->setAccessibleName(tr("Caption outline width"));
  outline_width_->setRange(0.0, 32.0);
  outline_width_->setValue(0.0);
  styleForm->addRow(tr("Outline"), outline_width_);
  const auto addColorButton = [this, styleForm, style](const QString& label, QPushButton*& button,
                                                       QColor& color, const QString& name) {
    button = new QPushButton(style);
    button->setObjectName(name);
    button->setAccessibleName(label);
    button->setText(label);
    button->setToolTip(tr("Choose a color"));
    styleForm->addRow(label, button);
    connect(button, &QPushButton::clicked, this, [this, button, &color, label] {
      const QColor selected =
          QColorDialog::getColor(color, this, label, QColorDialog::ShowAlphaChannel);
      if (!selected.isValid())
        return;
      color = selected;
      button->setStyleSheet(
          QStringLiteral("background-color: %1").arg(color.name(QColor::HexArgb)));
      const int row = table_->currentRow();
      if (row >= 0 && row < rows_.size())
        emit captionStyleEdited(rows_.at(row).id, styleFromControls());
    });
  };
  addColorButton(tr("Text color"), text_color_, text_color_value_,
                 QStringLiteral("captionTextColor"));
  addColorButton(tr("Background color"), background_color_, background_color_value_,
                 QStringLiteral("captionBackgroundColor"));
  addColorButton(tr("Outline color"), outline_color_, outline_color_value_,
                 QStringLiteral("captionOutlineColor"));
  auto* styleButtons = new QWidget(style);
  auto* styleButtonsLayout = new QHBoxLayout(styleButtons);
  styleButtonsLayout->setContentsMargins(0, 0, 0, 0);
  style_bold_ = new QCheckBox(tr("Bold"), styleButtons);
  style_bold_->setObjectName(QStringLiteral("captionStyleBold"));
  style_bold_->setAccessibleName(tr("Bold captions"));
  style_italic_ = new QCheckBox(tr("Italic"), styleButtons);
  style_italic_->setObjectName(QStringLiteral("captionStyleItalic"));
  style_italic_->setAccessibleName(tr("Italic captions"));
  styleButtonsLayout->addWidget(style_bold_);
  styleButtonsLayout->addWidget(style_italic_);
  styleButtonsLayout->addStretch();
  styleForm->addRow(tr("Emphasis"), styleButtons);
  style_preview_ = new QLabel(tr("Preview uses these settings for burn-in and export."), style);
  style_preview_->setObjectName(QStringLiteral("captionStylePreview"));
  style_preview_->setAccessibleName(tr("Caption style preview"));
  style_preview_->setWordWrap(true);
  styleForm->addRow(QString{}, style_preview_);
  layout->addWidget(style);

  auto* reviewGroup = new QGroupBox(tr("Review suggestions"), this);
  reviewGroup->setObjectName(QStringLiteral("captionReviewGroup"));
  auto* reviewLayout = new QVBoxLayout(reviewGroup);
  review_ = new QListWidget(reviewGroup);
  review_->setObjectName(QStringLiteral("captionReviewList"));
  review_->setAccessibleName(tr("Caption and silence proposals"));
  reviewLayout->addWidget(review_);
  auto* reviewButtons = new QHBoxLayout;
  apply_review_ = new QPushButton(tr("Apply selected"), reviewGroup);
  apply_review_->setObjectName(QStringLiteral("applyCaptionReviewButton"));
  apply_review_->setAccessibleName(tr("Apply selected caption review suggestions"));
  discard_review_ = new QPushButton(tr("Discard"), reviewGroup);
  discard_review_->setObjectName(QStringLiteral("discardCaptionReviewButton"));
  discard_review_->setAccessibleName(tr("Discard caption review suggestions"));
  reviewButtons->addWidget(apply_review_);
  reviewButtons->addWidget(discard_review_);
  reviewLayout->addLayout(reviewButtons);
  reviewGroup->setVisible(false);
  layout->addWidget(reviewGroup);

  connect(transcribe, &QPushButton::clicked, this,
          [this] { emit transcribeWithOptionsRequested(optionsFromControls()); });
  connect(import, &QPushButton::clicked, this, &CaptionsPanelWidget::importCaptionsRequested);
  connect(emptyAdd, &QPushButton::clicked, this, &CaptionsPanelWidget::addCaptionRequested);
  connect(exportCaptions, &QPushButton::clicked, this,
          &CaptionsPanelWidget::exportCaptionsRequested);
  connect(addCaption, &QPushButton::clicked, this, &CaptionsPanelWidget::addCaptionRequested);
  connect(removeCaption, &QPushButton::clicked, this, [this] {
    if (table_->currentRow() >= 0) {
      emit removeCaptionRequested(table_->currentRow());
    }
  });
  connect(search_, &QLineEdit::textChanged, this, &CaptionsPanelWidget::findInTranscriptRequested);
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    emit captionActivated(row);
    if (row >= 0 && row < rows_.size()) {
      emit captionIdActivated(rows_.at(row).id, rows_.at(row).start);
    }
  });
  connect(table_, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
    updateWordList(currentRow);
    if (currentRow >= 0 && currentRow < rows_.size()) {
      updateStyleControls(rows_.at(currentRow).style);
    }
  });
  connect(words_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    if (item != nullptr)
      emit wordActivated(item->data(Qt::UserRole).toString(),
                         item->data(Qt::UserRole + 1).toLongLong());
  });
  connect(words_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    if (item != nullptr)
      emit wordActivated(item->data(Qt::UserRole).toString(),
                         item->data(Qt::UserRole + 1).toLongLong());
  });
  connect(table_, &QTableWidget::cellChanged, this, [this](const int row, const int column) {
    if (column == 1 && table_->item(row, column) != nullptr) {
      emit captionTextEdited(row, table_->item(row, column)->text());
    }
  });
  const auto emitOptions = [this] { emit transcriptionOptionsChanged(optionsFromControls()); };
  connect(language_, &QComboBox::currentTextChanged, this,
          [emitOptions](const QString&) { emitOptions(); });
  connect(translate_, &QCheckBox::toggled, this, [emitOptions](bool) { emitOptions(); });
  connect(prefer_vulkan_, &QCheckBox::toggled, this, [emitOptions](bool) { emitOptions(); });
  connect(word_timestamps_, &QCheckBox::toggled, this, [emitOptions](bool) { emitOptions(); });
  connect(thread_count_, &QSpinBox::valueChanged, this, [emitOptions](int) { emitOptions(); });
  connect(model_download_, &QPushButton::clicked, this,
          [this] { emit downloadModelRequested(model_id_); });
  connect(transcribe_, &QPushButton::clicked, this,
          [this] { emit transcribeWithOptionsRequested(optionsFromControls()); });
  connect(transcribe_cancel_, &QPushButton::clicked, this,
          &CaptionsPanelWidget::cancelTranscriptionRequested);
  const auto emitStyle = [this] {
    const int row = table_->currentRow();
    if (row >= 0 && row < rows_.size()) {
      emit captionStyleEdited(rows_.at(row).id, styleFromControls());
    }
  };
  connect(font_family_, &QLineEdit::editingFinished, this, emitStyle);
  connect(font_size_, &QDoubleSpinBox::editingFinished, this, emitStyle);
  connect(alignment_, &QComboBox::currentTextChanged, this,
          [emitStyle](const QString&) { emitStyle(); });
  connect(vertical_position_, &QDoubleSpinBox::editingFinished, this, emitStyle);
  connect(safe_margin_, &QDoubleSpinBox::editingFinished, this, emitStyle);
  connect(outline_width_, &QDoubleSpinBox::editingFinished, this, emitStyle);
  connect(style_bold_, &QCheckBox::toggled, this, [emitStyle](bool) { emitStyle(); });
  connect(style_italic_, &QCheckBox::toggled, this, [emitStyle](bool) { emitStyle(); });
  connect(review_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
    if (item == nullptr)
      return;
    const QString id = item->data(Qt::UserRole).toString();
    for (auto& proposal : proposals_) {
      if (proposal.id == id)
        proposal.selected = item->checkState() == Qt::Checked;
    }
    emit reviewProposalToggled(id, item->checkState() == Qt::Checked);
  });
  connect(apply_review_, &QPushButton::clicked, this, &CaptionsPanelWidget::applyReviewRequested);
  connect(discard_review_, &QPushButton::clicked, this,
          &CaptionsPanelWidget::discardReviewRequested);
  updateStateControls();
}

void CaptionsPanelWidget::setCaptionRows(const QStringList& timecodes, const QStringList& text) {
  QVector<CaptionRowView> rows;
  const auto count = std::min(timecodes.size(), text.size());
  rows.reserve(count);
  for (int row = 0; row < count; ++row) {
    rows.push_back({.id = QStringLiteral("row-%1").arg(row),
                    .timecode = timecodes.at(row),
                    .text = text.at(row),
                    .language = {},
                    .start = 0,
                    .end = 0,
                    .words = {},
                    .style = {},
                    .confidence = 1.0,
                    .suggested = false});
  }
  setCaptionRows(rows);
}

void CaptionsPanelWidget::setCaptionRows(const QVector<CaptionRowView>& rows) {
  QString selectedId;
  const int currentRow = table_->currentRow();
  if (const auto* item = currentRow >= 0 ? table_->item(currentRow, 0) : nullptr; item != nullptr) {
    selectedId = item->data(Qt::UserRole).toString();
  }
  rows_ = rows;
  const QSignalBlocker blocker(table_);
  table_->setRowCount(rows_.size());
  for (int row = 0; row < rows_.size(); ++row) {
    const auto& view = rows_.at(row);
    auto* time = new QTableWidgetItem(view.timecode);
    time->setFlags(time->flags() & ~Qt::ItemIsEditable);
    time->setData(Qt::UserRole, view.id);
    table_->setItem(row, 0, time);
    table_->setItem(row, 1, new QTableWidgetItem(view.text));
    auto* confidence = new QTableWidgetItem(
        view.suggested ? tr("Suggested · %1%").arg(view.confidence * 100.0, 0, 'f', 0)
                       : tr("Edited"));
    confidence->setFlags(confidence->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, 2, confidence);
  }
  content_->setCurrentIndex(rows_.isEmpty() ? 0 : 1);
  int selectedRow = -1;
  for (int row = 0; row < rows_.size(); ++row) {
    if (!selectedId.isEmpty() && rows_.at(row).id == selectedId) {
      selectedRow = row;
      break;
    }
  }
  if (selectedRow < 0 && !rows_.isEmpty())
    selectedRow = 0;
  if (selectedRow >= 0)
    table_->setCurrentCell(selectedRow, 0);
  updateWordList(selectedRow);
  if (selectedRow >= 0) {
    updateStyleControls(rows_.at(selectedRow).style);
  }
}

void CaptionsPanelWidget::setTranscriptionState(const TranscriptionState state,
                                                const QString& message, const int percent) {
  transcription_state_ = state;
  if (!message.isEmpty())
    transcription_status_->setText(message);
  transcription_progress_->setValue(std::clamp(percent, 0, 100));
  transcription_progress_->setVisible(state == TranscriptionState::Downloading ||
                                      state == TranscriptionState::Running ||
                                      state == TranscriptionState::Cancelling);
  updateStateControls();
}

void CaptionsPanelWidget::setModelDownloadState(const ModelDownloadView& state) {
  model_id_ = state.modelId.isEmpty() ? model_id_ : state.modelId;
  if (!state.status.isEmpty())
    transcription_status_->setText(state.status);
  const int percent =
      state.totalBytes > 0 ? static_cast<int>((100LL * state.receivedBytes) / state.totalBytes) : 0;
  setTranscriptionState(state.state, {}, percent);
}

void CaptionsPanelWidget::setTranscriptionOptions(const TranscriptionOptionsView& options) {
  model_id_ = options.modelId.isEmpty() ? model_id_ : options.modelId;
  const QSignalBlocker languageBlocker(language_);
  const QSignalBlocker translateBlocker(translate_);
  const QSignalBlocker vulkanBlocker(prefer_vulkan_);
  const QSignalBlocker wordsBlocker(word_timestamps_);
  const QSignalBlocker threadBlocker(thread_count_);
  language_->setCurrentIndex(language_->findData(options.language));
  if (language_->currentIndex() < 0)
    language_->setCurrentIndex(0);
  translate_->setChecked(options.translate);
  prefer_vulkan_->setChecked(options.preferVulkan);
  word_timestamps_->setChecked(options.wordTimestamps);
  thread_count_->setValue(std::clamp(options.threadCount, 0, 64));
}

void CaptionsPanelWidget::setCaptionStyle(const CaptionStyleView& style) {
  updateStyleControls(style);
}

void CaptionsPanelWidget::setReviewProposals(const QVector<CaptionProposalView>& proposals) {
  proposals_ = proposals;
  auto* group = findChild<QGroupBox*>(QStringLiteral("captionReviewGroup"));
  if (group != nullptr)
    group->setVisible(!proposals_.isEmpty());
  const QSignalBlocker blocker(review_);
  review_->clear();
  for (const auto& proposal : proposals_) {
    auto* item = new QListWidgetItem(
        tr("%1 · %2 · %3").arg(proposal.kind, proposal.summary, proposal.previewRange), review_);
    item->setData(Qt::UserRole, proposal.id);
    item->setToolTip(proposal.confidence);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(proposal.selected ? Qt::Checked : Qt::Unchecked);
  }
  apply_review_->setEnabled(!proposals_.isEmpty());
}

void CaptionsPanelWidget::updateWordList(const int row) {
  if (words_ == nullptr)
    return;
  const QSignalBlocker blocker(words_);
  words_->clear();
  if (row < 0 || row >= rows_.size())
    return;
  for (const auto& word : rows_.at(row).words) {
    auto* item = new QListWidgetItem(tr("%1  ·  %2–%3 s  ·  %4%")
                                         .arg(word.text)
                                         .arg(static_cast<double>(word.start) / 48'000.0, 0, 'f', 3)
                                         .arg(static_cast<double>(word.end) / 48'000.0, 0, 'f', 3)
                                         .arg(word.probability * 100.0, 0, 'f', 0),
                                     words_);
    item->setData(Qt::UserRole, word.id);
    item->setData(Qt::UserRole + 1, word.start);
  }
}

TranscriptionOptionsView CaptionsPanelWidget::optionsFromControls() const {
  return {.modelId = model_id_,
          .language = language_->currentData().toString(),
          .translate = translate_->isChecked(),
          .preferVulkan = prefer_vulkan_->isChecked(),
          .wordTimestamps = word_timestamps_->isChecked(),
          .threadCount = thread_count_->value()};
}

CaptionStyleView CaptionsPanelWidget::styleFromControls() const {
  return {.fontFamily = font_family_->text(),
          .fontSize = font_size_->value(),
          .textColor = text_color_value_,
          .backgroundColor = background_color_value_,
          .bold = style_bold_->isChecked(),
          .italic = style_italic_->isChecked(),
          .alignment = alignment_->currentData().toString(),
          .verticalPosition = vertical_position_->value(),
          .safeMargin = safe_margin_->value(),
          .outlineWidth = outline_width_->value(),
          .outlineColor = outline_color_value_};
}

void CaptionsPanelWidget::updateStyleControls(const CaptionStyleView& style) {
  const QSignalBlocker fontBlocker(font_family_);
  const QSignalBlocker sizeBlocker(font_size_);
  const QSignalBlocker alignBlocker(alignment_);
  const QSignalBlocker positionBlocker(vertical_position_);
  const QSignalBlocker marginBlocker(safe_margin_);
  const QSignalBlocker outlineBlocker(outline_width_);
  const QSignalBlocker boldBlocker(style_bold_);
  const QSignalBlocker italicBlocker(style_italic_);
  font_family_->setText(style.fontFamily);
  text_color_value_ = style.textColor;
  background_color_value_ = style.backgroundColor;
  outline_color_value_ = style.outlineColor;
  font_size_->setValue(style.fontSize);
  alignment_->setCurrentIndex(std::max(0, alignment_->findData(style.alignment)));
  vertical_position_->setValue(style.verticalPosition);
  safe_margin_->setValue(style.safeMargin);
  outline_width_->setValue(style.outlineWidth);
  style_bold_->setChecked(style.bold);
  style_italic_->setChecked(style.italic);
  const QString css = QStringLiteral("font-family:'%1'; font-size:%2pt; color:%3; background:%4; "
                                     "border: %5px solid %6; padding: 8px;")
                          .arg(style.fontFamily)
                          .arg(style.fontSize, 0, 'f', 1)
                          .arg(style.textColor.name(QColor::HexArgb))
                          .arg(style.backgroundColor.name(QColor::HexArgb))
                          .arg(style.outlineWidth, 0, 'f', 1)
                          .arg(style.outlineColor.name(QColor::HexArgb));
  style_preview_->setText(tr("Preview (approximate; final export uses the same persisted style)"));
  style_preview_->setStyleSheet(css);
  text_color_->setStyleSheet(
      QStringLiteral("background-color: %1").arg(text_color_value_.name(QColor::HexArgb)));
  background_color_->setStyleSheet(
      QStringLiteral("background-color: %1").arg(background_color_value_.name(QColor::HexArgb)));
  outline_color_->setStyleSheet(
      QStringLiteral("background-color: %1").arg(outline_color_value_.name(QColor::HexArgb)));
}

void CaptionsPanelWidget::updateStateControls() {
  const bool active = transcription_state_ == TranscriptionState::Downloading ||
                      transcription_state_ == TranscriptionState::Running ||
                      transcription_state_ == TranscriptionState::Cancelling;
  const bool ready = transcription_state_ == TranscriptionState::Ready;
  model_download_->setEnabled(!active && !ready);
  transcribe_->setEnabled(ready);
  transcribe_cancel_->setVisible(active);
  transcribe_cancel_->setEnabled(transcription_state_ != TranscriptionState::Cancelling);
}

DeliverPanelWidget::DeliverPanelWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("deliverPanel"));
  setAccessibleName(tr("Deliver and export"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  auto* heading = new QLabel(tr("Export your video"), this);
  QFont font = heading->font();
  font.setPointSize(font.pointSize() + 2);
  font.setWeight(QFont::DemiBold);
  heading->setFont(font);
  layout->addWidget(heading);

  auto* presetForm = new QFormLayout;
  preset_ = new QComboBox(this);
  preset_->setObjectName(QStringLiteral("exportPreset"));
  preset_->setAccessibleName(tr("Export preset"));
  presetForm->addRow(tr("Preset"), preset_);
  layout->addLayout(presetForm);

  auto* destinationRow = new QHBoxLayout;
  destination_ = new QLineEdit(this);
  destination_->setObjectName(QStringLiteral("destinationField"));
  destination_->setAccessibleName(tr("Export destination"));
  destination_->setReadOnly(true);
  destination_->setPlaceholderText(tr("Choose a destination…"));
  browse_button_ = new QToolButton(this);
  browse_button_->setObjectName(QStringLiteral("browseButton"));
  browse_button_->setAccessibleName(tr("Browse for export destination"));
  browse_button_->setText(tr("Browse…"));
  destinationRow->addWidget(destination_);
  destinationRow->addWidget(browse_button_);
  auto* destinationWidget = new QWidget(this);
  destinationWidget->setLayout(destinationRow);
  auto* destinationForm = new QFormLayout;
  destinationForm->addRow(tr("Destination"), destinationWidget);
  layout->addLayout(destinationForm);

  auto* summary = new QGroupBox(tr("Summary"), this);
  auto* summaryLayout = new QFormLayout(summary);
  auto* summaryVideo = new QLabel(summary);
  auto* summaryAudio = new QLabel(summary);
  auto* summarySource = new QLabel(tr("Original media"), summary);
  summaryLayout->addRow(tr("Video"), summaryVideo);
  summaryLayout->addRow(tr("Audio"), summaryAudio);
  summaryLayout->addRow(tr("Source"), summarySource);
  layout->addWidget(summary);

  auto* advanced = new QGroupBox(tr("Advanced settings"), this);
  advanced->setCheckable(true);
  advanced->setChecked(false);
  auto* advancedForm = new QFormLayout(advanced);

  resolution_ = new QComboBox(advanced);
  resolution_->setObjectName(QStringLiteral("resolutionCombo"));
  resolution_->setAccessibleName(tr("Export resolution"));
  resolution_->addItem(tr("Use sequence"), QVariantList{0, 0});
  resolution_->addItem(tr("1920×1080"), QVariantList{1920, 1080});
  resolution_->addItem(tr("2560×1440"), QVariantList{2560, 1440});
  resolution_->addItem(tr("3840×2160"), QVariantList{3840, 2160});
  resolution_->addItem(tr("1080×1920 (vertical)"), QVariantList{1080, 1920});
  resolution_->addItem(tr("720×1280 (vertical)"), QVariantList{720, 1280});
  advancedForm->addRow(tr("Resolution"), resolution_);

  frame_rate_ = new QComboBox(advanced);
  frame_rate_->setObjectName(QStringLiteral("frameRateCombo"));
  frame_rate_->setAccessibleName(tr("Export frame rate"));
  frame_rate_->addItem(tr("Use sequence"), QVariantList{0, 0});
  for (const auto& rate : {24, 25, 30, 50, 60}) {
    frame_rate_->addItem(tr("%1 fps").arg(rate), QVariantList{rate, 1});
  }
  advancedForm->addRow(tr("Frame rate"), frame_rate_);

  video_bitrate_ = new QComboBox(advanced);
  video_bitrate_->setObjectName(QStringLiteral("videoBitrateCombo"));
  video_bitrate_->setAccessibleName(tr("Export video bitrate"));
  video_bitrate_->addItem(tr("Use preset default"), QVariant::fromValue<qulonglong>(0));
  for (const auto bitrate : {2ULL, 4ULL, 6ULL, 8ULL, 12ULL, 16ULL, 25ULL, 40ULL}) {
    video_bitrate_->addItem(tr("%1 Mbps").arg(bitrate),
                            QVariant::fromValue<qulonglong>(bitrate * 1'000'000ULL));
  }
  advancedForm->addRow(tr("Video bitrate"), video_bitrate_);

  video_quality_ = new QComboBox(advanced);
  video_quality_->setObjectName(QStringLiteral("videoQualityCombo"));
  video_quality_->setAccessibleName(tr("VP9 constant quality"));
  video_quality_->addItem(tr("Use video bitrate"));
  for (const int quality : {20, 24, 28, 32, 36, 40}) {
    video_quality_->addItem(tr("Constant quality %1").arg(quality), quality);
  }
  advancedForm->addRow(tr("VP9 quality"), video_quality_);

  hardware_encoder_ = new QCheckBox(tr("Use hardware VP9 when available"), advanced);
  hardware_encoder_->setObjectName(QStringLiteral("hardwareEncoderCheck"));
  hardware_encoder_->setAccessibleName(tr("Use hardware VP9 encoder when available"));
  hardware_encoder_->setToolTip(tr("The export retries with deterministic libvpx-vp9 software "
                                   "encoding if hardware setup or encoding fails."));
  hardware_encoder_->setChecked(true);
  advancedForm->addRow(QString{}, hardware_encoder_);

  audio_bitrate_ = new QComboBox(advanced);
  audio_bitrate_->setObjectName(QStringLiteral("audioBitrateCombo"));
  audio_bitrate_->setAccessibleName(tr("Export audio bitrate"));
  audio_bitrate_->addItem(tr("Use preset default"), 0u);
  for (const auto bitrate : {64u, 96u, 128u, 192u, 256u}) {
    audio_bitrate_->addItem(tr("%1 kbps").arg(bitrate), bitrate * 1000u);
  }
  advancedForm->addRow(tr("Audio bitrate"), audio_bitrate_);

  caption_mode_ = new QComboBox(advanced);
  caption_mode_->setObjectName(QStringLiteral("captionModeCombo"));
  caption_mode_->setAccessibleName(tr("Caption export mode"));
  caption_mode_->addItem(tr("None"), QStringLiteral("none"));
  caption_mode_->addItem(tr("Burn in"), QStringLiteral("burn_in"));
  caption_mode_->addItem(tr("Sidecar file"), QStringLiteral("sidecar"));
  caption_mode_->addItem(tr("Burn in + sidecar"), QStringLiteral("burn_in_and_sidecar"));
  advancedForm->addRow(tr("Captions"), caption_mode_);

  sidecar_format_ = new QComboBox(advanced);
  sidecar_format_->setObjectName(QStringLiteral("sidecarFormatCombo"));
  sidecar_format_->setAccessibleName(tr("Caption sidecar format"));
  sidecar_format_->addItem(tr("SRT"), QStringLiteral("srt"));
  sidecar_format_->addItem(tr("WebVTT"), QStringLiteral("vtt"));
  advancedForm->addRow(tr("Sidecar format"), sidecar_format_);

  encoder_summary_ = makeMutedLabel(QString(), advanced);
  encoder_summary_->setObjectName(QStringLiteral("encoderSummary"));
  encoder_summary_->setAccessibleName(tr("Encoder capabilities"));
  encoder_summary_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  advancedForm->addRow(tr("Encoder"), encoder_summary_);
  layout->addWidget(advanced);

  preset_notes_ = makeMutedLabel(QString(), this);
  preset_notes_->setObjectName(QStringLiteral("presetNotes"));
  preset_notes_->setAccessibleName(tr("Preset notes"));
  preset_notes_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  layout->addWidget(preset_notes_);
  layout->addStretch();

  export_progress_ = new QProgressBar(this);
  export_progress_->setObjectName(QStringLiteral("exportProgress"));
  export_progress_->setAccessibleName(tr("Export progress"));
  export_progress_->setRange(0, 100);
  export_progress_->setTextVisible(true);
  export_progress_->hide();
  layout->addWidget(export_progress_);

  export_button_ = new QToolButton(this);
  export_button_->setObjectName(QStringLiteral("exportButton"));
  export_button_->setAccessibleName(tr("Export video master"));
  export_button_->setText(tr("Export master"));
  export_button_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
  export_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  export_button_->setMinimumHeight(38);
  layout->addWidget(export_button_);

  connect(browse_button_, &QToolButton::clicked, this,
          &DeliverPanelWidget::destinationBrowseRequested);
  connect(caption_mode_, &QComboBox::currentIndexChanged, this, [this] {
    const auto mode = captionModeKey();
    sidecar_format_->setEnabled(mode == QStringLiteral("sidecar") ||
                                mode == QStringLiteral("burn_in_and_sidecar"));
  });
  connect(preset_, &QComboBox::currentIndexChanged, this, [this, summaryVideo, summaryAudio] {
    const auto info = export_service::platform_preset_info(
        static_cast<export_service::PlatformPreset>(preset_->currentData().toInt()));
    const bool audio_only = info.audio_only;
    summaryVideo->setText(info.audio_only ? tr("Audio only")
                                          : QString::fromStdString(info.intended_video_codec));
    summaryAudio->setText(QString::fromStdString(info.intended_audio_codec));
    const bool available = preset_->itemData(preset_->currentIndex(), Qt::UserRole - 1).toBool();
    selected_preset_available_ = available;
    preset_notes_->setText(
        available ? QString::fromStdString(info.notes)
                  : tr("Unavailable: the required FOSS encoder is not present in this build."));
    resolution_->setEnabled(!audio_only);
    frame_rate_->setEnabled(!audio_only);
    video_bitrate_->setEnabled(!audio_only);
    video_quality_->setEnabled(!audio_only);
    hardware_encoder_->setEnabled(!audio_only && hardware_vp9_available_);
    hardware_encoder_->setToolTip(
        hardware_vp9_available_
            ? tr("Use hardware VP9 when available; failed hardware export retries in software.")
            : tr("No usable VP9 hardware encoder was detected; software libvpx-vp9 will be used."));
    caption_mode_->setEnabled(!audio_only);
    if (audio_only) {
      caption_mode_->setCurrentIndex(0);
    }
    export_button_->setEnabled(export_enabled_ && selected_preset_available_);
    emit presetChanged(selectedPresetId());
  });
  connect(export_button_, &QToolButton::clicked, this, [this] {
    if (export_progress_->isVisible()) {
      emit cancelRequested();
    } else {
      emit exportRequested(selectedPresetId());
    }
  });

  loadPlatformPresets();
  sidecar_format_->setEnabled(false);
}

void DeliverPanelWidget::loadPlatformPresets() {
  preset_->clear();
  const auto presets = export_service::available_platform_presets();
  const auto software_matrix = media::probe_encoder_capabilities(false);
  hardware_vp9_available_ = std::any_of(
      software_matrix.encoders.cbegin(), software_matrix.encoders.cend(), [](const auto& encoder) {
        return encoder.codec == media::DeliveryCodec::Vp9 && encoder.hardware && encoder.available;
      });
  hardware_encoder_->setChecked(hardware_vp9_available_);
  setEncoderCapabilities(
      QString::fromStdString(media::format_capability_summary(software_matrix)) +
      tr("\nHardware device readiness is validated on the export worker when delivery starts."));
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
}

void DeliverPanelWidget::setEncoderCapabilities(const QString& summary) {
  encoder_summary_->setText(summary);
}

void DeliverPanelWidget::setDestinationPath(const QString& path) {
  destination_->setText(path);
}

QString DeliverPanelWidget::destinationPath() const {
  return destination_->text();
}

QString DeliverPanelWidget::selectedPresetId() const {
  return preset_->currentData().toString();
}

void DeliverPanelWidget::setExportEnabled(bool enabled) {
  export_enabled_ = enabled;
  export_button_->setEnabled(enabled && selected_preset_available_);
}

void DeliverPanelWidget::setExportRunning(const bool running, const int percent) {
  export_progress_->setVisible(running);
  export_progress_->setValue(std::clamp(percent, 0, 100));
  export_button_->setText(running ? tr("Cancel export") : tr("Export master"));
  export_button_->setEnabled(running || (export_enabled_ && selected_preset_available_));
  export_button_->setAccessibleName(running ? tr("Cancel current export")
                                            : tr("Export video master"));
}

QString DeliverPanelWidget::captionModeKey() const {
  return caption_mode_->currentData().toString();
}

QString DeliverPanelWidget::sidecarFormatKey() const {
  return sidecar_format_->currentData().toString();
}

int DeliverPanelWidget::overrideWidth() const {
  return resolution_->currentData().toList().value(0).toInt();
}

int DeliverPanelWidget::overrideHeight() const {
  return resolution_->currentData().toList().value(1).toInt();
}

unsigned int DeliverPanelWidget::overrideFrameRateNum() const {
  return frame_rate_->currentData().toList().value(0).toUInt();
}

unsigned int DeliverPanelWidget::overrideFrameRateDen() const {
  return frame_rate_->currentData().toList().value(1).toUInt();
}

unsigned int DeliverPanelWidget::overrideAudioBitrate() const {
  return audio_bitrate_->currentData().toUInt();
}

std::uint64_t DeliverPanelWidget::overrideVideoBitrate() const {
  return video_bitrate_->currentData().toULongLong();
}

std::optional<int> DeliverPanelWidget::overrideVideoQuality() const {
  const QVariant value = video_quality_->currentData();
  if (!value.isValid()) {
    return std::nullopt;
  }
  return value.toInt() == 0 ? std::nullopt : std::optional<int>{value.toInt()};
}

bool DeliverPanelWidget::preferHardwareEncoder() const {
  return hardware_encoder_ != nullptr && hardware_encoder_->isChecked();
}

} // namespace video_editor::desktop_ui
