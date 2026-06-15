#include "recordlab/widgets/simple_curve_plot_widget.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace recordlab::widgets {

namespace {

struct PanelStyle {
  QString title;
  QColor color;
  QColor background;
};

struct CurveStats {
  double minValue = 0.0;
  double maxValue = 0.0;
  double meanValue = 0.0;
  double stdValue = 0.0;
  double pkpkValue = 0.0;
  bool valid = false;
};

QVector<PanelStyle> panelStyles() {
  // 为 X/Y/Z 三个面板定义统一的标题、线色和背景色。
  return {{QStringLiteral("X / Value"), QColor(QStringLiteral("#cc453a")),
           QColor(QStringLiteral("#fff1ef"))},
          {QStringLiteral("Y"), QColor(QStringLiteral("#2b8a57")),
           QColor(QStringLiteral("#effbf4"))},
          {QStringLiteral("Z"), QColor(QStringLiteral("#2d69c7")),
           QColor(QStringLiteral("#eef4ff"))}};
}

double sampleValue(const CurveSample &sample, int axis) {
  // 按轴号提取样本中的 x/y/z 值；标量模式仍复用 x 通道。
  switch (axis) {
  case 0:
    return sample.x;
  case 1:
    return sample.y;
  default:
    return sample.z;
  }
}

double normalizedStep(double estimatedStep, double rawDelta) {
  // 对时间步长做平滑和异常大间隔裁剪，避免时间轴被单个脏点拉坏。
  const double fallbackStep = std::clamp(estimatedStep, 0.002, 0.05);
  if (rawDelta <= 1e-6) {
    return fallbackStep;
  }

  const double gapThreshold = std::max(0.08, fallbackStep * 6.0);
  if (rawDelta > gapThreshold) {
    return fallbackStep;
  }

  return rawDelta;
}

CurveStats calculateStats(const QVector<CurveSample> &samples, int axis) {
  // 计算当前轴向的 min/max/mean/std/pkpk，供底部统计文本展示。
  CurveStats stats;
  if (samples.isEmpty()) {
    return stats;
  }

  stats.minValue = sampleValue(samples.front(), axis);
  stats.maxValue = stats.minValue;
  double sum = 0.0;
  double sumSquares = 0.0;
  for (const auto &sample : samples) {
    const double value = sampleValue(sample, axis);
    stats.minValue = std::min(stats.minValue, value);
    stats.maxValue = std::max(stats.maxValue, value);
    sum += value;
    sumSquares += value * value;
  }

  const double count = static_cast<double>(samples.size());
  stats.meanValue = sum / count;
  const double variance =
      std::max(0.0, sumSquares / count - stats.meanValue * stats.meanValue);
  stats.stdValue = std::sqrt(variance);
  stats.pkpkValue = stats.maxValue - stats.minValue;
  stats.valid = true;
  return stats;
}

}  // namespace

SimpleCurvePlotWidget::SimpleCurvePlotWidget(QWidget *parent) : QWidget(parent) {
  // 初始化曲线面板的默认尺寸和占位文案。
  setMinimumHeight(190);
  setAutoFillBackground(true);
  placeholderText_ = QStringLiteral(
      "选择 IMU / time_delay / record_timer 后显示实时曲线。");
}

void SimpleCurvePlotWidget::appendSample(const CurveSample &sample,
                                         bool scalarMode) {
  appendSamples({sample}, scalarMode);
}

void SimpleCurvePlotWidget::appendSamples(const QVector<CurveSample> &samples,
                                          bool scalarMode) {
  if (samples.isEmpty()) {
    return;
  }

  scalarMode_ = scalarMode;

  for (const auto &sample : samples) {
    CurveSample normalized = sample;
    const double rawTimestamp = sample.timestamp;

    if (hasLastRawTimestamp_ && rawTimestamp > 0.0 &&
        rawTimestamp < lastRawTimestamp_ - 1.0) {
      // 设备重连/热插拔后的会话切换仍然直接清窗。
      samples_.clear();
      resetTimelineState();
    }

    if (samples_.isEmpty()) {
      normalized.timestamp = rawTimestamp > 0.0 ? rawTimestamp : 0.0;
    } else {
      const double rawDelta =
          (hasLastRawTimestamp_ && rawTimestamp > 0.0)
              ? (rawTimestamp - lastRawTimestamp_)
              : 0.0;
      const double usedStep = normalizedStep(estimatedStepSeconds_, rawDelta);
      normalized.timestamp = samples_.back().timestamp + usedStep;
      if (rawDelta > 1e-6 &&
          rawDelta < std::max(0.08, estimatedStepSeconds_ * 6.0)) {
        estimatedStepSeconds_ = estimatedStepSeconds_ * 0.85 + rawDelta * 0.15;
      }
    }

    if (rawTimestamp > 0.0) {
      lastRawTimestamp_ = rawTimestamp;
      hasLastRawTimestamp_ = true;
    }

    samples_.push_back(normalized);
  }

  while (samples_.size() > maxSampleCount_) {
    samples_.removeFirst();
  }

  if (timeWindowSeconds_ > 0.0 && !samples_.isEmpty()) {
    const double cutoff = samples_.back().timestamp - timeWindowSeconds_;
    while (samples_.size() > 1 && samples_.front().timestamp < cutoff) {
      samples_.removeFirst();
    }
  }

  if (batchMode_) {
    pendingUpdate_ = true;
  } else {
    update();
  }
}

void SimpleCurvePlotWidget::beginBatchUpdate() {
  // 开启批量更新模式，延迟 repaint 直到 flushUpdates。
  batchMode_ = true;
  pendingUpdate_ = false;
}

void SimpleCurvePlotWidget::flushUpdates() {
  // 在批量更新结束时统一触发一次 repaint。
  if (batchMode_ && pendingUpdate_) {
    update();
  }
  batchMode_ = false;
  pendingUpdate_ = false;
}

void SimpleCurvePlotWidget::clearData() {
  // 清空曲线缓存并重置时间轴状态。
  samples_.clear();
  pendingUpdate_ = false;
  resetTimelineState();
  update();
}

void SimpleCurvePlotWidget::setSamples(const QVector<CurveSample> &samples,
                                       bool scalarMode) {
  samples_ = samples;
  scalarMode_ = scalarMode;
  resetTimelineState();
  if (!samples_.isEmpty()) {
    lastRawTimestamp_ = samples_.back().timestamp;
    hasLastRawTimestamp_ = true;
  }
  while (samples_.size() > maxSampleCount_) {
    samples_.removeFirst();
  }
  if (timeWindowSeconds_ > 0.0 && !samples_.isEmpty()) {
    const double cutoff = samples_.back().timestamp - timeWindowSeconds_;
    while (samples_.size() > 1 && samples_.front().timestamp < cutoff) {
      samples_.removeFirst();
    }
  }
  update();
}

void SimpleCurvePlotWidget::setSelectedDataName(const QString &dataName) {
  // 记录当前选中的数据名，用于空态文案和标题提示。
  selectedDataName_ = dataName;
  update();
}

void SimpleCurvePlotWidget::setPlaceholderText(const QString &text) {
  // 更新无数据时显示的占位文本。
  placeholderText_ = text;
  update();
}

void SimpleCurvePlotWidget::resetTimelineState() {
  // 重置原始时间戳和估计步长，用于新会话重新建轴。
  lastRawTimestamp_ = 0.0;
  hasLastRawTimestamp_ = false;
  estimatedStepSeconds_ = 0.01;
}

void SimpleCurvePlotWidget::paintEvent(QPaintEvent *event) {
  // 自绘三联图曲线面板、网格、坐标轴和统计信息。
  Q_UNUSED(event);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.fillRect(rect(), QColor(QStringLiteral("#fafafa")));

  const QRect frame = rect().adjusted(8, 8, -8, -8);
  painter.setPen(QPen(QColor(QStringLiteral("#c9c9c9"))));
  painter.drawRect(frame);

  if (samples_.isEmpty()) {
    painter.setPen(QColor(QStringLiteral("#666666")));
    painter.drawText(frame.adjusted(12, 12, -12, -12),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     selectedDataName_.isEmpty()
                         ? placeholderText_
                         : QStringLiteral("%1\n等待数据...").arg(selectedDataName_));
    return;
  }

  const auto styles = panelStyles();
  const int spacing = 10;
  const int panelWidth = std::max(80, (frame.width() - spacing * 2) / 3);
  const int titleHeight = 24;
  const int axisFooterHeight = 26;
  const int statsHeight = 42;
  const int chartTop = frame.top() + titleHeight + 6;
  const int chartBottom = frame.bottom() - axisFooterHeight - statsHeight - 8;
  const int chartHeight = std::max(34, chartBottom - chartTop);

  const double visibleStart = samples_.front().timestamp;
  double visibleEnd = samples_.back().timestamp;
  if (visibleEnd < visibleStart) {
    visibleEnd = visibleStart;
  }
  const double visibleSpan = std::max(0.001, visibleEnd - visibleStart);
  for (int axis = 0; axis < 3; ++axis) {
    const QRect panelRect(frame.left() + axis * (panelWidth + spacing),
                          frame.top(), panelWidth, frame.height());
    const QRect chartRect(panelRect.left() + 44, chartTop,
                          panelRect.width() - 56, chartHeight);
    const QRect statsRect(panelRect.left() + 8, panelRect.bottom() - statsHeight,
                          panelRect.width() - 16, statsHeight - 4);
    const auto style = styles.at(axis);
    const QString title =
        scalarMode_ && axis == 0 ? QStringLiteral("Value") : style.title;

    painter.fillRect(panelRect, style.background);
    painter.setPen(QPen(QColor(QStringLiteral("#d6d6d6"))));
    painter.drawRect(panelRect);

    painter.setPen(QColor(QStringLiteral("#4f4f4f")));
    painter.drawText(panelRect.adjusted(8, 4, -8, -4),
                     Qt::AlignTop | Qt::AlignHCenter, title);

    if (scalarMode_ && axis > 0) {
      painter.setPen(QColor(QStringLiteral("#999999")));
      painter.drawText(chartRect, Qt::AlignCenter, QStringLiteral("标量数据"));
      painter.drawText(statsRect, Qt::AlignCenter, QStringLiteral("--"));
      continue;
    }

    const CurveStats stats = calculateStats(samples_, axis);
    if (!stats.valid) {
      continue;
    }

    double minValue = stats.minValue;
    double maxValue = stats.maxValue;
    double margin = (maxValue - minValue) * 0.15;
    if (margin < 1e-6) {
      margin = 1.0;
    }
    minValue -= margin;
    maxValue += margin;

    painter.setPen(QPen(QColor(QStringLiteral("#8b8b8b")), 1));
    painter.drawLine(chartRect.bottomLeft(), chartRect.bottomRight());
    painter.drawLine(chartRect.bottomLeft(), chartRect.topLeft());

    painter.setPen(QPen(QColor(QStringLiteral("#e1e1e1")), 1, Qt::DashLine));
    for (int tick = 0; tick <= 2; ++tick) {
      const qreal ratio = tick / 2.0;
      const int y =
          chartRect.bottom() - static_cast<int>(ratio * chartRect.height());
      painter.drawLine(chartRect.left(), y, chartRect.right(), y);
      const double tickValue = minValue + ratio * (maxValue - minValue);
      painter.setPen(QColor(QStringLiteral("#6b6b6b")));
      painter.drawText(QRect(panelRect.left() + 2, y - 10, 38, 20),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1").arg(tickValue, 0, 'f', 2));
      painter.setPen(QPen(QColor(QStringLiteral("#e1e1e1")), 1, Qt::DashLine));
    }

    for (int tick = 0; tick <= 2; ++tick) {
      const qreal ratio = tick / 2.0;
      const int x =
          chartRect.left() + static_cast<int>(ratio * chartRect.width());
      painter.drawLine(x, chartRect.top(), x, chartRect.bottom());
    }

    QPainterPath path;
    for (int i = 0; i < samples_.size(); ++i) {
      const auto &sample = samples_.at(i);
      const double relativeTimestamp =
          std::max(0.0, sample.timestamp - visibleStart);
      const double tRatio = visibleSpan <= 1e-9
                                ? 0.0
                                : std::clamp(relativeTimestamp / visibleSpan,
                                             0.0, 1.0);
      const double value = sampleValue(sample, axis);
      const double denominator = maxValue - minValue;
      const double vRatio =
          denominator <= 1e-9 ? 0.5 : (value - minValue) / denominator;
      const qreal x = chartRect.left() + tRatio * chartRect.width();
      const qreal y = chartRect.bottom() - vRatio * chartRect.height();
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }

    painter.setPen(QPen(style.color, 2));
    painter.drawPath(path);

    painter.setPen(QColor(QStringLiteral("#6b6b6b")));
    painter.drawText(QRect(chartRect.left(), chartRect.bottom() + 4, 72, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("%1").arg(visibleStart, 0, 'f', 1));
    painter.drawText(
        QRect(chartRect.center().x() - 44, chartRect.bottom() + 4, 88, 18),
        Qt::AlignHCenter | Qt::AlignVCenter,
        QStringLiteral("%1").arg(visibleStart + visibleSpan / 2.0, 0, 'f', 1));
    painter.drawText(QRect(chartRect.right() - 72, chartRect.bottom() + 4, 72,
                           18),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("%1").arg(visibleEnd, 0, 'f', 1));

    painter.setPen(style.color.darker(105));
    painter.drawText(
        statsRect, Qt::AlignCenter | Qt::TextWordWrap,
        QStringLiteral("mean:%1\nstd:%2  pkpk:%3")
            .arg(stats.meanValue, 0, 'f', 6)
            .arg(stats.stdValue, 0, 'f', 6)
            .arg(stats.pkpkValue, 0, 'f', 6));
  }
}

}  // namespace recordlab::widgets
