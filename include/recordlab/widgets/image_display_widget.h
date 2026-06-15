#pragma once

#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QFileDialog;
class QGraphicsPixmapItem;
class QGraphicsScene;
class QGraphicsView;
class QResizeEvent;
class QWheelEvent;

namespace recordlab::widgets {

class ClickableLabel : public QLabel {
  Q_OBJECT

public:
  explicit ClickableLabel(QWidget *parent = nullptr);

signals:
  void clicked();

protected:
  void mouseReleaseEvent(QMouseEvent *event) override;
};

class ZoomableImageDialog : public QDialog {
  Q_OBJECT

public:
  explicit ZoomableImageDialog(const QPixmap &pixmap, QWidget *parent = nullptr);

private:
  void saveScreenshot();
  bool savePixmapAsPgm(const QString &filePath, const QPixmap &pixmap) const;

  QGraphicsView *viewer_ = nullptr;
  QPixmap pixmap_;
};

class ImageDisplayWidget : public QWidget {
  Q_OBJECT

public:
  explicit ImageDisplayWidget(const QString &title, QWidget *parent = nullptr);

  void setZoomImageProvider(std::function<QImage()> provider);
  void showCameraFrame(const QImage &previewImage,
                       const QStringList &overlayLines = {});
  void clearImage(const QString &placeholder = QStringLiteral("等待图像流"));
  QSize displayTargetSize() const;

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void refreshPreview();
  QPixmap renderOverlayOnPixmap(const QPixmap &pixmap,
                                const QStringList &overlayLines) const;

  ClickableLabel *imageLabel_ = nullptr;
  QLabel *titleLabel_ = nullptr;
  QImage lastPreviewImage_;
  QStringList lastOverlayLines_;
  std::function<QImage()> zoomImageProvider_;
};

} // namespace recordlab::widgets
