// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/scope_widget.hpp"

#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace video_editor::desktop_ui {
namespace {

constexpr int kPadding = 12;
constexpr QColor kBackground{18, 20, 24};
constexpr QColor kGrid{58, 64, 74};
constexpr QColor kTrace{120, 210, 140};
constexpr QColor kTraceDim{70, 120, 82};

class ScopeView : public QWidget {
public:
  enum class Kind { Waveform, Vectorscope, Histogram };

  ScopeView(const Kind kind, QWidget* parent = nullptr) : QWidget(parent), kind_(kind) {
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    switch (kind_) {
    case Kind::Waveform:
      setAccessibleName(QStringLiteral("Waveform"));
      break;
    case Kind::Vectorscope:
      setAccessibleName(QStringLiteral("Vectorscope"));
      break;
    case Kind::Histogram:
      setAccessibleName(QStringLiteral("Histogram"));
      break;
    }
  }

  void setAnalysis(const std::optional<render::ScopeAnalysis>& analysis) {
    analysis_ = analysis;
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);
    const QRect plot = plotRect();
    painter.setPen(kGrid);
    painter.drawRect(plot);

    if (!analysis_.has_value()) {
      painter.setPen(QColor(150, 156, 168));
      painter.drawText(plot, Qt::AlignCenter, tr("No frame"));
      return;
    }

    switch (kind_) {
    case Kind::Waveform:
      paintWaveform(painter, plot, *analysis_);
      break;
    case Kind::Vectorscope:
      paintVectorscope(painter, plot, *analysis_);
      break;
    case Kind::Histogram:
      paintHistogram(painter, plot, *analysis_);
      break;
    }
  }

private:
  [[nodiscard]] QRect plotRect() const {
    return QRect(kPadding, kPadding, std::max(1, width() - 2 * kPadding),
                 std::max(1, height() - 2 * kPadding));
  }

  static void paintWaveform(QPainter& painter, const QRect& plot,
                            const render::ScopeAnalysis& analysis) {
    for (int level : {0, 50, 100}) {
      const int y = plot.bottom() - (level * plot.height() / 100);
      painter.setPen(level == 50 ? QColor(90, 96, 108) : kGrid);
      painter.drawLine(plot.left(), y, plot.right(), y);
      painter.drawText(plot.left() + 2, y - 2, QStringLiteral("%1").arg(level));
    }

    const int columns = std::max(1, analysis.waveform_column_count);
    const float column_width = static_cast<float>(plot.width()) / static_cast<float>(columns);
    std::uint32_t global_peak = 1;
    for (int column = 0; column < columns; ++column) {
      global_peak = std::max(global_peak,
                             *std::max_element(analysis.waveform[column].begin(),
                                               analysis.waveform[column].end()));
    }

    for (int column = 0; column < columns; ++column) {
      const int x = plot.left() + static_cast<int>(std::lround(column * column_width));
      const int next_x =
          plot.left() + static_cast<int>(std::lround((column + 1) * column_width));
      const int column_width_px = std::max(1, next_x - x);
      for (int bin = 0; bin < render::ScopeAnalysis::kWaveformBins; ++bin) {
        const std::uint32_t count = analysis.waveform[column][bin];
        if (count == 0) {
          continue;
        }
        const int y = plot.bottom() -
                        (bin * plot.height() / (render::ScopeAnalysis::kWaveformBins - 1));
        const int intensity = static_cast<int>(255.0 * std::sqrt(static_cast<double>(count) /
                                                                 static_cast<double>(global_peak)));
        painter.fillRect(x, y, column_width_px, 1,
                         QColor(kTrace.red(), kTrace.green(), kTrace.blue(), intensity));
      }
    }
  }

  static void paintVectorscope(QPainter& painter, const QRect& plot,
                                const render::ScopeAnalysis& analysis) {
    const QPoint center(plot.center());
    const int radius = std::min(plot.width(), plot.height()) / 2 - 2;
    painter.setPen(kGrid);
    painter.drawEllipse(center, radius, radius);
    const QRect box(center.x() - radius / 2, center.y() - radius / 2, radius, radius);
    painter.drawRect(box);

    std::uint32_t peak = 1;
    for (const auto& row : analysis.vectorscope) {
      peak = std::max(peak, *std::max_element(row.begin(), row.end()));
    }

    const float scale_x = static_cast<float>(plot.width()) /
                          static_cast<float>(render::ScopeAnalysis::kVectorscopeSize);
    const float scale_y = static_cast<float>(plot.height()) /
                          static_cast<float>(render::ScopeAnalysis::kVectorscopeSize);
    for (int y = 0; y < render::ScopeAnalysis::kVectorscopeSize; ++y) {
      for (int x = 0; x < render::ScopeAnalysis::kVectorscopeSize; ++x) {
        const std::uint32_t count = analysis.vectorscope[y][x];
        if (count == 0) {
          continue;
        }
        const int intensity = static_cast<int>(255.0 * std::sqrt(static_cast<double>(count) /
                                                                 static_cast<double>(peak)));
        const QRect pixel(plot.left() + static_cast<int>(x * scale_x),
                          plot.top() + static_cast<int>(y * scale_y),
                          std::max(1, static_cast<int>(std::ceil(scale_x))),
                          std::max(1, static_cast<int>(std::ceil(scale_y))));
        painter.fillRect(pixel, QColor(kTrace.red(), kTrace.green(), kTrace.blue(), intensity));
      }
    }
  }

  static void paintHistogram(QPainter& painter, const QRect& plot,
                              const render::ScopeAnalysis& analysis) {
    painter.setPen(kGrid);
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.bottomLeft(), plot.topLeft());

    const auto draw_channel = [&](const std::array<std::uint32_t, render::ScopeAnalysis::kHistogramBins>& bins,
                                  const QColor& color) {
      const std::uint32_t peak =
          std::max<std::uint32_t>(1, *std::max_element(bins.begin(), bins.end()));
      QPainterPath path;
      path.moveTo(plot.bottomLeft());
      for (int bin = 0; bin < render::ScopeAnalysis::kHistogramBins; ++bin) {
        const int x = plot.left() + (bin * plot.width() / (render::ScopeAnalysis::kHistogramBins - 1));
        const int height =
            static_cast<int>((static_cast<double>(bins[bin]) / static_cast<double>(peak)) *
                             plot.height());
        path.lineTo(x, plot.bottom() - height);
      }
      path.lineTo(plot.bottomRight());
      path.closeSubpath();
      QColor fill = color;
      fill.setAlpha(70);
      painter.fillPath(path, fill);
      painter.setPen(color);
      painter.drawPath(path);
    };

    draw_channel(analysis.histogram_r, QColor(220, 90, 90));
    draw_channel(analysis.histogram_g, QColor(90, 200, 110));
    draw_channel(analysis.histogram_b, QColor(90, 140, 230));
    draw_channel(analysis.histogram_luma, QColor(210, 210, 210));
  }

  Kind kind_;
  std::optional<render::ScopeAnalysis> analysis_;
};

} // namespace

ScopeWidget::ScopeWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("videoScopes"));
  setAccessibleName(QStringLiteral("Video scopes"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  tabs_ = new QTabWidget(this);
  tabs_->setObjectName(QStringLiteral("videoScopesTabs"));
  tabs_->setAccessibleName(QStringLiteral("Video scope views"));
  layout->addWidget(tabs_);

  rebuildTabs();
}

void ScopeWidget::setAnalysis(const render::ScopeAnalysis& analysis) {
  if (!isVisible()) {
    return;
  }
  analysis_ = analysis;
  for (int index = 0; index < tabs_->count(); ++index) {
    if (auto* view = static_cast<ScopeView*>(tabs_->widget(index))) {
      view->setAnalysis(analysis_);
    }
  }
}

void ScopeWidget::clear() {
  analysis_.reset();
  for (int index = 0; index < tabs_->count(); ++index) {
    if (auto* view = static_cast<ScopeView*>(tabs_->widget(index))) {
      view->setAnalysis(analysis_);
    }
  }
}

void ScopeWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (analysis_.has_value()) {
    setAnalysis(*analysis_);
  }
}

void ScopeWidget::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
}

void ScopeWidget::rebuildTabs() {
  tabs_->clear();
  tabs_->addTab(new ScopeView(ScopeView::Kind::Waveform, tabs_), tr("Waveform"));
  tabs_->addTab(new ScopeView(ScopeView::Kind::Vectorscope, tabs_), tr("Vectorscope"));
  tabs_->addTab(new ScopeView(ScopeView::Kind::Histogram, tabs_), tr("Histogram"));
}

} // namespace video_editor::desktop_ui
