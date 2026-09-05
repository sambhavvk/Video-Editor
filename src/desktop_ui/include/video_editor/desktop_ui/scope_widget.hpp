// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/render_engine/scope_analyzer.h"

#include <QTabWidget>
#include <QWidget>

#include <optional>

namespace video_editor::desktop_ui {

class ScopeWidget final : public QWidget {
  Q_OBJECT

public:
  explicit ScopeWidget(QWidget* parent = nullptr);

  void setAnalysis(const render::ScopeAnalysis& analysis);
  void clear();

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void rebuildTabs();

  QTabWidget* tabs_{nullptr};
  std::optional<render::ScopeAnalysis> analysis_;
};

} // namespace video_editor::desktop_ui
