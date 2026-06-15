#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QPainter>
#include <QRect>
#include <QString>
#include <QWidget>

#include <QCheckBox>
#include <QPushButton>


namespace recordlab::widgets {

class NvizTreePlotWidget : public QWidget {
public:
  explicit NvizTreePlotWidget(QWidget *parent = nullptr);
  ~NvizTreePlotWidget() = default;

  void appendSample(const QString &fieldKey, const QString &displayName,
                    double timestamp, double value);
  void clearData();
  void setPaused(bool paused);
  bool isPaused() const;

private:
  struct SeriesState {
    double timestamp;
    double value;
  };

  QHash<QString, QList<SeriesState>> seriesByFieldKey_;
  QHash<QString, QColor> colorByFieldKey_;
  QHash<QString, QString> displayNameByFieldKey_;
  bool isPaused_ = false;
  double timeWindow_ = 5.0;  // 5秒时间窗口
  int nextColorIndex_ = 0;

  QWidget *plotCanvas_ = nullptr;
  QPushButton *pauseButton_ = nullptr;
  QPushButton *clearButton_ = nullptr;
  QPushButton *resetButton_ = nullptr;
  QCheckBox *autoScaleCheck_ = nullptr;
  QCheckBox *autoScrollCheck_ = nullptr;

  // 缩放/视图状态
  bool autoScale_ = true;
  bool autoScroll_ = true;
  double fixedMinValue_ = 0.0;
  double fixedMaxValue_ = 1.0;
  double viewStartTime_ = 0.0;
  double viewEndTime_ = 0.0;
  bool hasViewRange_ = false;

  QColor assignColorForField(const QString &fieldKey);
  static QList<QColor> fieldColors();

  void onPauseClicked();
  void onClearClicked();
  void onResetClicked();
  void onAutoScaleToggled(bool checked);
  void onAutoScrollToggled(bool checked);
  void requestRepaint();
  void renderPlot(QPainter &painter, const QRect &rect);

  friend class NvizTreePlotCanvas;
};

}  // namespace recordlab::widgets
