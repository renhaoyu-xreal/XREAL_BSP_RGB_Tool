#pragma once

#include <QVector>
#include <QWidget>

class QString;

namespace recordlab::widgets {

struct CurveSample {
    double timestamp = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

class SimpleCurvePlotWidget : public QWidget {
public:
    explicit SimpleCurvePlotWidget(QWidget* parent = nullptr);

    void appendSample(const CurveSample& sample, bool scalarMode);
    void appendSamples(const QVector<CurveSample>& samples, bool scalarMode);
    void beginBatchUpdate();
    void flushUpdates();
    void clearData();
    void setSamples(const QVector<CurveSample>& samples, bool scalarMode);
    void setSelectedDataName(const QString& dataName);
    void setPlaceholderText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void resetTimelineState();

    QVector<CurveSample> samples_;
    QString placeholderText_;
    QString selectedDataName_;
    bool scalarMode_ = false;
    bool batchMode_ = false;
    bool pendingUpdate_ = false;
    double timeWindowSeconds_ = 5.0;
    double lastRawTimestamp_ = 0.0;
    bool hasLastRawTimestamp_ = false;
    double estimatedStepSeconds_ = 0.01;
    int maxSampleCount_ = 600;
};

}  // namespace recordlab::widgets
