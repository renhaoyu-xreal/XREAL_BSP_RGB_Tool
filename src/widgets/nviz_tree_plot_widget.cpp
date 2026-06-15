#include "recordlab/widgets/nviz_tree_plot_widget.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace recordlab::widgets {

class NvizTreePlotCanvas : public QWidget {
public:
  explicit NvizTreePlotCanvas(NvizTreePlotWidget *owner, QWidget *parent = nullptr)
      : QWidget(parent), owner_(owner) {
    setAutoFillBackground(false);
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    Q_UNUSED(event);
    if (!owner_) {
      return;
    }
    QPainter painter(this);
    owner_->renderPlot(painter, rect());
  }

private:
  NvizTreePlotWidget *owner_ = nullptr;
};

QList<QColor> NvizTreePlotWidget::fieldColors() {
  return {QColor(0, 102, 204),      // 蓝色
          QColor(220, 53, 69),      // 红色
          QColor(40, 167, 69),      // 绿色
          QColor(255, 193, 7),      // 黄色
          QColor(108, 117, 125),    // 灰色
          QColor(23, 162, 184),     // 青色
          QColor(111, 66, 193),     // 紫色
          QColor(253, 126, 20)};    // 橙色
}

NvizTreePlotWidget::NvizTreePlotWidget(QWidget *parent)
    : QWidget(parent) {
  setMinimumHeight(280);
  setStyleSheet(QStringLiteral("background-color: white;"));

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(6);

  plotCanvas_ = new NvizTreePlotCanvas(this, this);
  plotCanvas_->setMinimumHeight(220);
  mainLayout->addWidget(plotCanvas_, 1);

  auto *toolbarWidget = new QWidget(this);
  toolbarWidget->setAutoFillBackground(true);
  toolbarWidget->setMinimumHeight(48);
  toolbarWidget->setStyleSheet(QStringLiteral(
      "QWidget { background-color: #fbfaf7; border-top: 1px solid #d8cfbf; }"
      "QPushButton {"
      "  background: #efe7d8;"
      "  border: 1px solid #b9ab8e;"
      "  border-radius: 6px;"
      "  padding: 5px 12px;"
      "  min-height: 30px;"
      "}"
      "QPushButton:hover { background: #f5edde; }"
      "QPushButton:pressed {"
      "  background: #e8dfce;"
      "  padding-top: 6px;"
      "  padding-bottom: 4px;"
      "}"
      "QPushButton:checked {"
      "  background: #d8e7fb;"
      "  border-color: #7ea2d6;"
      "  color: #243b5a;"
      "}"
      "QCheckBox { border: none; padding: 4px 2px; }"
      "QCheckBox::indicator { width: 14px; height: 14px; }"));
  auto *toolbar = new QHBoxLayout(toolbarWidget);
  toolbar->setContentsMargins(8, 7, 8, 7);
  toolbar->setSpacing(8);
  pauseButton_ = new QPushButton(QStringLiteral("暂停"), toolbarWidget);
  pauseButton_->setCheckable(true);
  pauseButton_->setMinimumHeight(30);
  clearButton_ = new QPushButton(QStringLiteral("清除"), toolbarWidget);
  clearButton_->setMinimumHeight(30);
  resetButton_ = new QPushButton(QStringLiteral("重置坐标"), toolbarWidget);
  resetButton_->setMinimumHeight(30);
  autoScaleCheck_ = new QCheckBox(QStringLiteral("自动缩放"), this);
  autoScaleCheck_->setChecked(true);
  autoScrollCheck_ = new QCheckBox(QStringLiteral("自动滚动"), this);
  autoScrollCheck_->setChecked(true);

  toolbar->addWidget(pauseButton_);
  toolbar->addWidget(clearButton_);
  toolbar->addWidget(resetButton_);
  toolbar->addWidget(autoScaleCheck_);
  toolbar->addWidget(autoScrollCheck_);
  toolbar->addStretch();

  mainLayout->addWidget(toolbarWidget, 0);

  connect(pauseButton_, &QPushButton::clicked, this, &NvizTreePlotWidget::onPauseClicked);
  connect(clearButton_, &QPushButton::clicked, this, &NvizTreePlotWidget::onClearClicked);
  connect(resetButton_, &QPushButton::clicked, this, &NvizTreePlotWidget::onResetClicked);
  connect(autoScaleCheck_, &QCheckBox::toggled, this, &NvizTreePlotWidget::onAutoScaleToggled);
  connect(autoScrollCheck_, &QCheckBox::toggled, this, &NvizTreePlotWidget::onAutoScrollToggled);
}

QColor NvizTreePlotWidget::assignColorForField(const QString &fieldKey) {
  if (colorByFieldKey_.contains(fieldKey)) {
    return colorByFieldKey_[fieldKey];
  }
  const auto colors = fieldColors();
  const QColor color = colors[nextColorIndex_ % colors.size()];
  ++nextColorIndex_;
  colorByFieldKey_[fieldKey] = color;
  return color;
}

void NvizTreePlotWidget::appendSample(const QString &fieldKey,
                                      const QString &displayName,
                                      double timestamp, double value) {
  if (isPaused_) return;

  assignColorForField(fieldKey);
  displayNameByFieldKey_[fieldKey] = displayName;

  auto &series = seriesByFieldKey_[fieldKey];
  series.append({timestamp, value});

  viewEndTime_ = hasViewRange_ ? std::max(viewEndTime_, timestamp) : timestamp;
  if (autoScroll_) {
    viewEndTime_ = hasViewRange_ ? std::max(viewEndTime_, timestamp) : timestamp;
    viewStartTime_ = viewEndTime_ - timeWindow_;
  } else if (!hasViewRange_) {
    viewStartTime_ = timestamp;
  }
  hasViewRange_ = true;

  for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end(); ++it) {
    auto &samples = it.value();
    while (!samples.isEmpty() && samples.front().timestamp < viewStartTime_) {
      samples.removeFirst();
    }
  }

  requestRepaint();
}

void NvizTreePlotWidget::clearData() {
  seriesByFieldKey_.clear();
  colorByFieldKey_.clear();
  displayNameByFieldKey_.clear();
  nextColorIndex_ = 0;
  viewStartTime_ = 0.0;
  viewEndTime_ = 0.0;
  hasViewRange_ = false;
  requestRepaint();
}

void NvizTreePlotWidget::setPaused(bool paused) {
  isPaused_ = paused;
  if (pauseButton_) {
    pauseButton_->setChecked(paused);
    pauseButton_->setText(paused ? QStringLiteral("恢复")
                                 : QStringLiteral("暂停"));
  }
}

bool NvizTreePlotWidget::isPaused() const {
  return isPaused_;
}

void NvizTreePlotWidget::requestRepaint() {
  if (plotCanvas_) {
    plotCanvas_->update();
    return;
  }
  update();
}

void NvizTreePlotWidget::renderPlot(QPainter &painter, const QRect &canvasRect) {
  painter.fillRect(canvasRect, Qt::white);

  if (seriesByFieldKey_.isEmpty()) {
    painter.setPen(Qt::gray);
    painter.drawText(canvasRect, Qt::AlignCenter,
                     QStringLiteral("No data to display"));
    return;
  }

  double dataMinTime = 1e100;
  double dataMaxTime = -1e100;

  for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end();
       ++it) {
    for (const auto &sample : it.value()) {
      dataMinTime = std::min(dataMinTime, sample.timestamp);
      dataMaxTime = std::max(dataMaxTime, sample.timestamp);
    }
  }

  if (dataMinTime > dataMaxTime) {
    painter.setPen(Qt::gray);
    painter.drawText(canvasRect, Qt::AlignCenter,
                     QStringLiteral("No data to display"));
    return;
  }

  double minTime = dataMinTime;
  double maxTime = dataMaxTime;
  if (hasViewRange_) {
    minTime = viewStartTime_;
    maxTime = autoScroll_ ? viewEndTime_ : std::max(viewEndTime_, dataMaxTime);
  }

  double minValue = 1e100;
  double maxValue = -1e100;
  for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end();
       ++it) {
    for (const auto &sample : it.value()) {
      if (sample.timestamp < minTime || sample.timestamp > maxTime) {
        continue;
      }
      minValue = std::min(minValue, sample.value);
      maxValue = std::max(maxValue, sample.value);
    }
  }

  if (!autoScale_ && fixedMaxValue_ > fixedMinValue_) {
    minValue = fixedMinValue_;
    maxValue = fixedMaxValue_;
  }

  if (minTime >= maxTime || minValue >= maxValue) {
    painter.setPen(Qt::gray);
    painter.drawText(canvasRect, Qt::AlignCenter,
                     QStringLiteral("Insufficient data"));
    return;
  }

  painter.setFont(QFont(QStringLiteral("Arial"), 9));
  QFontMetrics fm(painter.font());

  // 图例在折线图上方单独占位，避免挤出右边界或覆盖曲线。
  const int leftMargin = 50;
  const int rightMargin = 10;
  const int legendLineHeight = 18;
  const int legendTop = canvasRect.top() + 8;
  const int legendLeft = canvasRect.left() + leftMargin;
  const int legendRight = canvasRect.right() - rightMargin;
  const int legendAvailableWidth = std::max(80, legendRight - legendLeft);
  int legendRows = 1;
  int currentLegendX = legendLeft;
  for (auto it = displayNameByFieldKey_.begin();
       it != displayNameByFieldKey_.end(); ++it) {
    const int itemWidth =
        std::clamp(fm.horizontalAdvance(it.value()) + 32, 90,
                   std::max(90, legendAvailableWidth));
    if (currentLegendX > legendLeft &&
        currentLegendX + itemWidth > legendRight) {
      currentLegendX = legendLeft;
      ++legendRows;
    }
    currentLegendX += itemWidth + 12;
  }

  // 留边距用于绘制图例、坐标轴和标签。
  const int topMargin = 16 + legendRows * legendLineHeight;
  const int bottomMargin = 40;

  const QRect plotRect(canvasRect.left() + leftMargin, canvasRect.top() + topMargin,
                       canvasRect.width() - leftMargin - rightMargin,
                       canvasRect.height() - topMargin - bottomMargin);

  int legendX = legendLeft;
  int legendY = legendTop;
  for (auto it = displayNameByFieldKey_.begin();
       it != displayNameByFieldKey_.end(); ++it) {
    const QString &fieldKey = it.key();
    const QString &displayName = it.value();
    const QColor color = colorByFieldKey_[fieldKey];
    const int itemWidth =
        std::clamp(fm.horizontalAdvance(displayName) + 32, 90,
                   std::max(90, legendAvailableWidth));
    if (legendX > legendLeft && legendX + itemWidth > legendRight) {
      legendX = legendLeft;
      legendY += legendLineHeight;
    }

    painter.fillRect(legendX, legendY + 3, 10, 10, color);
    painter.setPen(QPen(QColor(QStringLiteral("#555555"))));
    painter.drawRect(legendX, legendY + 3, 10, 10);
    painter.setPen(Qt::black);
    const QString elided =
        fm.elidedText(displayName, Qt::ElideRight, itemWidth - 18);
    painter.drawText(legendX + 16, legendY, itemWidth - 16, legendLineHeight,
                     Qt::AlignLeft | Qt::AlignVCenter, elided);
    legendX += itemWidth + 12;
  }

  // 绘制背景网格
  painter.setPen(QPen(QColor(200, 200, 200), 1, Qt::DashLine));
  for (int i = 0; i <= 4; ++i) {
    const int y = plotRect.top() + (plotRect.height() * i) / 4;
    painter.drawLine(plotRect.left(), y, plotRect.right(), y);
  }

  // 绘制坐标轴
  painter.setPen(QPen(Qt::black, 2));
  painter.drawLine(plotRect.left(), plotRect.top(), plotRect.left(),
                   plotRect.bottom());
  painter.drawLine(plotRect.left(), plotRect.bottom(), plotRect.right(),
                   plotRect.bottom());

  // 绘制曲线
  for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end();
       ++it) {
    const QString &fieldKey = it.key();
    const auto &series = it.value();

    if (series.isEmpty()) {
      continue;
    }

    const QColor color = colorByFieldKey_[fieldKey];
    painter.setPen(QPen(color, 2));

    QPointF prevPoint;
    bool firstPoint = true;

    for (const auto &sample : series) {
      if (sample.timestamp < minTime || sample.timestamp > maxTime) {
        continue;
      }
      const double relTime = sample.timestamp - minTime;
      const double relValue = sample.value - minValue;

      const double timeRatio = (maxTime > minTime) ? relTime / (maxTime - minTime)
                               : 0;
      const double valueRatio =
          (maxValue > minValue) ? relValue / (maxValue - minValue) : 0;

      const QPointF point(
          plotRect.left() + timeRatio * plotRect.width(),
          plotRect.bottom() - valueRatio * plotRect.height());

      if (firstPoint) {
        prevPoint = point;
        firstPoint = false;
      } else {
        painter.drawLine(prevPoint, point);
      }
      prevPoint = point;
    }
  }

  // 绘制时间轴标签
  painter.setPen(Qt::black);
  painter.setFont(QFont(QStringLiteral("Arial"), 9));
  for (int i = 0; i <= 4; ++i) {
    const double t = minTime + (maxTime - minTime) * i / 4;
    const int x = plotRect.left() + (plotRect.width() * i) / 4;
    QFontMetrics fm(painter.font());
    const QString label = QStringLiteral("%1").arg(t, 0, 'f', 1);
    const QString elided = fm.elidedText(label, Qt::ElideRight, 72);
    painter.drawText(x - 36, plotRect.bottom() + 15, 72, 20, Qt::AlignCenter,
                     elided);
  }

  // 绘制值轴标签
  for (int i = 0; i <= 4; ++i) {
    const double value = minValue + (maxValue - minValue) * i / 4;
    const int y = plotRect.bottom() - (plotRect.height() * i) / 4;
    painter.drawText(5, y - 10, 40, 20, Qt::AlignCenter,
                     QStringLiteral("%1").arg(value, 0, 'f', 1));
  }
}

void NvizTreePlotWidget::onPauseClicked() { setPaused(!isPaused_); }

void NvizTreePlotWidget::onClearClicked() { clearData(); }

void NvizTreePlotWidget::onResetClicked() {
  // 重置视图：清空固定缩放并回到自动缩放/滚动默认
  autoScale_ = true;
  autoScroll_ = true;
  autoScaleCheck_->setChecked(true);
  autoScrollCheck_->setChecked(true);
  viewStartTime_ = 0.0;
  viewEndTime_ = 0.0;
  hasViewRange_ = false;
  fixedMinValue_ = 0.0;
  fixedMaxValue_ = 1.0;
  requestRepaint();
}

void NvizTreePlotWidget::onAutoScaleToggled(bool checked) {
  autoScale_ = checked;
  if (!autoScale_) {
    // 采集当前范围作为固定值
    double minV = 1e100, maxV = -1e100;
    for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end(); ++it) {
      for (const auto &s : it.value()) {
        minV = std::min(minV, s.value);
        maxV = std::max(maxV, s.value);
      }
    }
    if (minV < maxV) {
      fixedMinValue_ = minV;
      fixedMaxValue_ = maxV;
    }
  }
  requestRepaint();
}

void NvizTreePlotWidget::onAutoScrollToggled(bool checked) {
  autoScroll_ = checked;
  double dataMaxTime = -1e100;
  for (auto it = seriesByFieldKey_.begin(); it != seriesByFieldKey_.end(); ++it) {
    for (const auto &sample : it.value()) {
      dataMaxTime = std::max(dataMaxTime, sample.timestamp);
    }
  }
  if (dataMaxTime > -1e90) {
    if (autoScroll_) {
      viewEndTime_ = dataMaxTime;
      viewStartTime_ = viewEndTime_ - timeWindow_;
    } else if (!hasViewRange_) {
      viewStartTime_ = dataMaxTime;
      viewEndTime_ = dataMaxTime;
    }
    hasViewRange_ = true;
  }
  requestRepaint();
}

}  // namespace recordlab::widgets
