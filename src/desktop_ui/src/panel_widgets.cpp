// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/panel_widgets.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace video_editor::desktop_ui {
namespace {

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
  table_->setColumnCount(4);
  table_->setHorizontalHeaderLabels({tr("Name"), tr("Duration"), tr("Format"), tr("Status")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
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
  connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& point) {
    const auto row = table_->rowAt(point.y());
    if (row < 0 || table_->item(row, 0) == nullptr) {
      return;
    }
    const auto id = table_->item(row, 0)->data(Qt::UserRole).toString();
    QMenu menu(this);
    auto* relink = menu.addAction(tr("Relink media…"));
    relink->setEnabled(table_->item(row, 3)->text() == tr("Offline"));
    const auto item =
        std::find_if(items_.cbegin(), items_.cend(),
                     [&id](const MediaItemView& candidate) { return candidate.id == id; });
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
    const auto* name = table_->item(row, 0);
    const auto* format = table_->item(row, 2);
    const auto matches = query.trimmed().isEmpty() ||
                         (name != nullptr && name->text().contains(query, Qt::CaseInsensitive)) ||
                         (format != nullptr && format->text().contains(query, Qt::CaseInsensitive));
    table_->setRowHidden(row, !matches);
  }
  emit searchChanged(query);
}

void MediaBinWidget::activateCurrent() {
  const auto row = table_->currentRow();
  if (row >= 0 && table_->item(row, 0) != nullptr) {
    emit mediaActivated(table_->item(row, 0)->data(Qt::UserRole).toString());
  }
}

void MediaBinWidget::rebuildTable() {
  table_->setSortingEnabled(false);
  table_->setRowCount(items_.size());
  for (int row = 0; row < items_.size(); ++row) {
    const auto& item = items_.at(row);
    auto* name = new QTableWidgetItem(item.displayName);
    name->setData(Qt::UserRole, item.id);
    name->setToolTip(item.filePath);
    table_->setItem(row, 0, name);
    table_->setItem(row, 1, new QTableWidgetItem(item.durationText));
    table_->setItem(row, 2, new QTableWidgetItem(item.formatText));
    const QString status_text = item.offline            ? tr("Offline")
                                : item.proxyGenerating  ? tr("Creating proxy…")
                                : item.proxyAvailable   ? tr("Proxy ready")
                                : item.proxyRecommended ? tr("Proxy recommended")
                                                        : tr("Original");
    auto* status = new QTableWidgetItem(status_text);
    status->setForeground(item.offline                                    ? QColor{235, 126, 126}
                          : item.proxyRecommended && !item.proxyAvailable ? QColor{225, 183, 98}
                                                                          : QColor{164, 193, 168});
    if (item.proxyRecommended && !item.proxyAvailable && !item.proxyGenerating) {
      status->setToolTip(tr("Right-click this item to create a smoother editing proxy"));
    }
    table_->setItem(row, 3, status);
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
  addField(tr("Scale"), QStringLiteral("scale"), QStringLiteral(" %"), 0, 2'000, 100, 1);
  addField(tr("Rotation"), QStringLiteral("rotation"), QStringLiteral("°"), -36000, 36000, 0, 0.1);
  addField(tr("Opacity"), QStringLiteral("opacity"), QStringLiteral(" %"), 0, 100, 100, 1);
  editorLayout->addWidget(essential);

  auto* advanced = new QGroupBox(tr("Advanced controls"), editor);
  advanced->setObjectName(QStringLiteral("inspectorAdvancedGroup"));
  advanced->setCheckable(true);
  advanced->setChecked(false);
  auto* advancedForm = new QFormLayout(advanced);
  auto* blend = new QComboBox(advanced);
  blend->setObjectName(QStringLiteral("inspector.blendMode"));
  blend->setAccessibleName(tr("Blend mode"));
  blend->addItems({tr("Normal"), tr("Multiply"), tr("Screen"), tr("Overlay"), tr("Soft Light")});
  advancedForm->addRow(tr("Blend mode"), blend);
  auto* interpolation = new QComboBox(advanced);
  interpolation->setObjectName(QStringLiteral("inspector.interpolation"));
  interpolation->setAccessibleName(tr("Keyframe interpolation"));
  interpolation->addItems({tr("Smooth"), tr("Linear"), tr("Hold")});
  advancedForm->addRow(tr("Interpolation"), interpolation);
  connect(blend, &QComboBox::currentTextChanged, this,
          [this](const QString& text) { emit parameterEdited(QStringLiteral("blendMode"), text); });
  connect(interpolation, &QComboBox::currentTextChanged, this, [this](const QString& text) {
    emit parameterEdited(QStringLiteral("interpolation"), text);
  });
  editorLayout->addWidget(advanced);
  editorLayout->addStretch();
  scroll->setWidget(editor);
  content_->addWidget(scroll);
  layout->addWidget(content_, 1);
}

void InspectorWidget::setSelectionName(const QString& name) {
  selection_name_->setText(name.isEmpty() ? tr("No selection") : name);
  content_->setCurrentIndex(name.isEmpty() ? 0 : 1);
}

void InspectorWidget::setParameter(const QString& parameterId, const QVariant& value) {
  if (auto* spin = findChild<QDoubleSpinBox*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(spin);
    spin->setValue(value.toDouble());
    return;
  }
  if (auto* combo = findChild<QComboBox*>(QStringLiteral("inspector.%1").arg(parameterId))) {
    const QSignalBlocker blocker(combo);
    combo->setCurrentText(value.toString());
  }
}

void InspectorWidget::clearSelection() {
  setSelectionName({});
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
      {QStringLiteral("video.crop"), tr("Crop"), tr("Video"), true},
      {QStringLiteral("video.gaussian_blur"), tr("Gaussian Blur"), tr("Video"), true},
      {QStringLiteral("audio.eq"), tr("Parametric Equalizer"), tr("Audio"), false},
      {QStringLiteral("audio.compressor"), tr("Compressor"), tr("Audio"), false},
      {QStringLiteral("audio.dialogue_denoise"), tr("Dialogue Noise Reduction"), tr("Audio"),
       false},
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

  setTrackNames({tr("A1 Dialogue"), tr("A2 Music"), tr("A3 Effects"), tr("Master")});
}

void AudioMixerWidget::setTrackNames(const QStringList& names) {
  delete strips_->layout();
  const auto children = strips_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
  qDeleteAll(children);

  auto* layout = new QHBoxLayout(strips_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  for (int index = 0; index < names.size(); ++index) {
    auto* strip = new QGroupBox(names.at(index), strips_);
    strip->setObjectName(QStringLiteral("mixerStrip.%1").arg(index));
    strip->setAccessibleName(tr("Mixer strip %1").arg(names.at(index)));
    strip->setMinimumWidth(112);
    auto* stripLayout = new QVBoxLayout(strip);

    auto* meter = new QProgressBar(strip);
    meter->setObjectName(QStringLiteral("audioMeter.%1").arg(index));
    meter->setAccessibleName(tr("Level meter for %1").arg(names.at(index)));
    meter->setOrientation(Qt::Vertical);
    meter->setRange(0, 60);
    meter->setValue(0);
    meter->setTextVisible(false);
    stripLayout->addWidget(meter, 1, Qt::AlignHCenter);

    auto* fader = new QSlider(Qt::Vertical, strip);
    fader->setObjectName(QStringLiteral("audioFader.%1").arg(index));
    fader->setAccessibleName(tr("Gain for %1").arg(names.at(index)));
    fader->setRange(-60, 12);
    fader->setValue(0);
    fader->setTickPosition(QSlider::TicksBothSides);
    fader->setTickInterval(12);
    stripLayout->addWidget(fader, 2, Qt::AlignHCenter);

    auto* value = new QLabel(QStringLiteral("0.0 dB"), strip);
    value->setAlignment(Qt::AlignCenter);
    stripLayout->addWidget(value);

    auto* buttons = new QHBoxLayout;
    auto* mute = new QToolButton(strip);
    mute->setText(tr("M"));
    mute->setCheckable(true);
    mute->setAccessibleName(tr("Mute %1").arg(names.at(index)));
    auto* solo = new QToolButton(strip);
    solo->setText(tr("S"));
    solo->setCheckable(true);
    solo->setAccessibleName(tr("Solo %1").arg(names.at(index)));
    buttons->addWidget(mute);
    buttons->addWidget(solo);
    stripLayout->addLayout(buttons);

    connect(fader, &QSlider::valueChanged, this, [this, index, value](int gain) {
      value->setText(QStringLiteral("%1 dB").arg(static_cast<double>(gain), 0, 'f', 1));
      emit gainEdited(index, static_cast<double>(gain));
    });
    connect(mute, &QToolButton::toggled, this,
            [this, index](bool muted) { emit muteToggled(index, muted); });
    connect(solo, &QToolButton::toggled, this,
            [this, index](bool soloed) { emit soloToggled(index, soloed); });
    layout->addWidget(strip);
  }
  layout->addStretch();
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
  auto* transcribe = new QPushButton(tr("Transcribe sequence"), empty);
  transcribe->setObjectName(QStringLiteral("transcribeButton"));
  transcribe->setAccessibleName(tr("Transcribe sequence locally"));
  auto* import = new QPushButton(tr("Import captions…"), empty);
  import->setObjectName(QStringLiteral("importCaptionsButton"));
  import->setAccessibleName(tr("Import SRT or WebVTT captions"));
  emptyLayout->addWidget(transcribe, 0, Qt::AlignHCenter);
  emptyLayout->addWidget(import, 0, Qt::AlignHCenter);
  emptyLayout->addStretch();
  content_->addWidget(empty);

  auto* tablePage = new QWidget(content_);
  auto* tableLayout = new QVBoxLayout(tablePage);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  table_ = new QTableWidget(tablePage);
  table_->setObjectName(QStringLiteral("captionsTable"));
  table_->setAccessibleName(tr("Caption segments"));
  table_->setColumnCount(2);
  table_->setHorizontalHeaderLabels({tr("Time"), tr("Caption")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setShowGrid(false);
  tableLayout->addWidget(table_, 1);
  auto* exportCaptions = new QPushButton(tr("Export SRT or WebVTT…"), tablePage);
  exportCaptions->setObjectName(QStringLiteral("exportCaptionsButton"));
  exportCaptions->setAccessibleName(tr("Export sequence captions"));
  tableLayout->addWidget(exportCaptions, 0, Qt::AlignRight);
  content_->addWidget(tablePage);
  layout->addWidget(content_, 1);

  connect(transcribe, &QPushButton::clicked, this, &CaptionsPanelWidget::transcribeRequested);
  connect(import, &QPushButton::clicked, this, &CaptionsPanelWidget::importCaptionsRequested);
  connect(exportCaptions, &QPushButton::clicked, this,
          &CaptionsPanelWidget::exportCaptionsRequested);
  connect(search_, &QLineEdit::textChanged, this, &CaptionsPanelWidget::findInTranscriptRequested);
  connect(table_, &QTableWidget::cellDoubleClicked, this,
          [this](int row, int) { emit captionActivated(row); });
}

void CaptionsPanelWidget::setCaptionRows(const QStringList& timecodes, const QStringList& text) {
  const auto count = std::min(timecodes.size(), text.size());
  table_->setRowCount(count);
  for (int row = 0; row < count; ++row) {
    table_->setItem(row, 0, new QTableWidgetItem(timecodes.at(row)));
    table_->setItem(row, 1, new QTableWidgetItem(text.at(row)));
  }
  content_->setCurrentIndex(count == 0 ? 0 : 1);
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
  layout->addWidget(makeMutedLabel(
      tr("CPU reference masters use the sequence resolution and original media."), this));

  auto* form = new QFormLayout;
  preset_ = new QComboBox(this);
  preset_->setObjectName(QStringLiteral("exportPreset"));
  preset_->setAccessibleName(tr("Export preset"));
  preset_->addItem(tr("Lossless master · FFV1 / Matroska"), QStringLiteral("master.ffv1"));
  preset_->addItem(tr("Editing master · ProRes 422 HQ / MOV"), QStringLiteral("master.prores"));
  form->addRow(tr("Preset"), preset_);

  auto* destination = new QLineEdit(this);
  destination->setObjectName(QStringLiteral("exportDestination"));
  destination->setAccessibleName(tr("Export destination"));
  destination->setPlaceholderText(tr("Choose when exporting…"));
  form->addRow(tr("Destination"), destination);
  layout->addLayout(form);

  auto* summary = new QGroupBox(tr("Summary"), this);
  auto* summaryLayout = new QFormLayout(summary);
  summaryLayout->addRow(tr("Video"), new QLabel(tr("Sequence resolution · Rec.709 SDR"), summary));
  summaryLayout->addRow(tr("Audio"), new QLabel(tr("Not included in this beta exporter"), summary));
  summaryLayout->addRow(tr("Source"), new QLabel(tr("Original media"), summary));
  layout->addWidget(summary);

  auto* advanced = new QGroupBox(tr("Advanced settings"), this);
  advanced->setCheckable(true);
  advanced->setChecked(false);
  auto* advancedForm = new QFormLayout(advanced);
  auto* encoder = new QComboBox(advanced);
  encoder->addItems({tr("Deterministic software reference")});
  encoder->setAccessibleName(tr("Encoder preference"));
  advancedForm->addRow(tr("Encoder"), encoder);
  auto* captions = new QComboBox(advanced);
  captions->addItems({tr("None · use caption panel for SRT/WebVTT")});
  captions->setAccessibleName(tr("Caption export mode"));
  advancedForm->addRow(tr("Captions"), captions);
  layout->addWidget(advanced);
  layout->addStretch();

  export_progress_ = new QProgressBar(this);
  export_progress_->setObjectName(QStringLiteral("exportProgress"));
  export_progress_->setAccessibleName(tr("Export progress"));
  export_progress_->setRange(0, 100);
  export_progress_->setValue(0);
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

  connect(preset_, &QComboBox::currentIndexChanged, this,
          [this] { emit presetChanged(selectedPresetId()); });
  connect(export_button_, &QToolButton::clicked, this,
          [this] { emit exportRequested(selectedPresetId()); });
}

QString DeliverPanelWidget::selectedPresetId() const {
  return preset_->currentData().toString();
}

void DeliverPanelWidget::setExportEnabled(bool enabled) {
  export_button_->setEnabled(enabled);
}

void DeliverPanelWidget::setExportRunning(const bool running, const int percent) {
  export_progress_->setVisible(running);
  export_progress_->setValue(std::clamp(percent, 0, 100));
  export_button_->setText(running ? tr("Cancel export") : tr("Export master"));
  export_button_->setAccessibleName(running ? tr("Cancel current export")
                                            : tr("Export video master"));
}

} // namespace video_editor::desktop_ui
