#include "recordlab/widgets/image_display_widget.h"

#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace recordlab::widgets {

namespace {

class ZoomableImageViewer : public QGraphicsView {
public:
  explicit ZoomableImageViewer(const QPixmap &pixmap, QWidget *parent = nullptr)
      : QGraphicsView(parent) {
    // 使用 scene + pixmap item 承载整张图，便于拖拽和平滑缩放。
    scene_ = new QGraphicsScene(this);
    setScene(scene_);

    pixmapItem_ = new QGraphicsPixmapItem(pixmap);
    scene_->addItem(pixmapItem_);

    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  }

  void scaleImage(double factor) {
    // 在限定范围内调整缩放比，防止无穷放大或缩得过小。
    currentScale_ *= factor;
    if (currentScale_ < 0.1) {
      currentScale_ = 0.1;
    } else if (currentScale_ > 12.0) {
      currentScale_ = 12.0;
    }
    resetTransform();
    scale(currentScale_, currentScale_);
  }

  void resetScale() {
    // 恢复到 1:1 缩放，给用户一个明确的回到初始视图入口。
    currentScale_ = 1.0;
    resetTransform();
    scale(currentScale_, currentScale_);
  }

protected:
  void wheelEvent(QWheelEvent *event) override {
    // 鼠标滚轮用于缩放图像，其余事件保持 QGraphicsView 默认行为。
    if (!event) {
      return;
    }
    if (event->angleDelta().y() > 0) {
      scaleImage(1.25);
    } else if (event->angleDelta().y() < 0) {
      scaleImage(0.8);
    } else {
      QGraphicsView::wheelEvent(event);
      return;
    }
    event->accept();
  }

private:
  QGraphicsScene *scene_ = nullptr;
  QGraphicsPixmapItem *pixmapItem_ = nullptr;
  double currentScale_ = 1.0;
};

}  // namespace

ClickableLabel::ClickableLabel(QWidget *parent) : QLabel(parent) {
  // 预览图支持点击放大，因此始终使用手型光标提示可交互性。
  setCursor(Qt::PointingHandCursor);
}

void ClickableLabel::mouseReleaseEvent(QMouseEvent *event) {
  // 左键抬起时发出 clicked 信号，同时保留 QLabel 自身的默认处理。
  if (event && event->button() == Qt::LeftButton) {
    emit clicked();
  }
  QLabel::mouseReleaseEvent(event);
}

ZoomableImageDialog::ZoomableImageDialog(const QPixmap &pixmap, QWidget *parent)
    : QDialog(parent), pixmap_(pixmap) {
  // 构造一个可缩放、可保存的原图查看对话框。
  setWindowTitle(QStringLiteral("图像查看"));
  resize(980, 720);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(8, 8, 8, 8);
  rootLayout->setSpacing(8);

  viewer_ = new ZoomableImageViewer(pixmap_, this);
  rootLayout->addWidget(viewer_, 1);

  auto *buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch(1);

  auto *zoomInButton = new QPushButton(QStringLiteral("+"), this);
  auto *zoomOutButton = new QPushButton(QStringLiteral("-"), this);
  auto *zoomResetButton = new QPushButton(QStringLiteral("重置"), this);
  auto *saveButton = new QPushButton(QStringLiteral("保存截图"), this);
  zoomInButton->setFixedSize(40, 30);
  zoomOutButton->setFixedSize(40, 30);
  zoomResetButton->setFixedSize(60, 30);
  saveButton->setFixedSize(80, 30);

  buttonLayout->addWidget(zoomInButton);
  buttonLayout->addWidget(zoomOutButton);
  buttonLayout->addWidget(zoomResetButton);
  buttonLayout->addWidget(saveButton);
  buttonLayout->addStretch(1);
  rootLayout->addLayout(buttonLayout);

  connect(zoomInButton, &QPushButton::clicked, this, [this]() {
    if (auto *viewer = static_cast<ZoomableImageViewer *>(viewer_)) {
      viewer->scaleImage(1.25);
    }
  });
  connect(zoomOutButton, &QPushButton::clicked, this, [this]() {
    if (auto *viewer = static_cast<ZoomableImageViewer *>(viewer_)) {
      viewer->scaleImage(0.8);
    }
  });
  connect(zoomResetButton, &QPushButton::clicked, this, [this]() {
    if (auto *viewer = static_cast<ZoomableImageViewer *>(viewer_)) {
      viewer->resetScale();
    }
  });
  connect(saveButton, &QPushButton::clicked, this,
          &ZoomableImageDialog::saveScreenshot);
}

void ZoomableImageDialog::saveScreenshot() {
  // 按用户选择的扩展名保存为 PGM 或常规位图格式。
  const QString filePath = QFileDialog::getSaveFileName(
      this, QStringLiteral("保存截图"), QString(),
      QStringLiteral("PGM Files (*.pgm);;PNG Files (*.png);;JPG Files (*.jpg);;"
                     "All Files (*)"));
  if (filePath.isEmpty()) {
    return;
  }

  if (filePath.endsWith(QStringLiteral(".pgm"), Qt::CaseInsensitive)) {
    savePixmapAsPgm(filePath, pixmap_);
    return;
  }

  pixmap_.save(filePath);
}

bool ZoomableImageDialog::savePixmapAsPgm(const QString &filePath,
                                          const QPixmap &pixmap) const {
  // 将当前图像保存为二进制 PGM，方便和旧版产物格式保持一致。
  if (pixmap.isNull()) {
    return false;
  }

  const QImage image = pixmap.toImage().convertToFormat(QImage::Format_Grayscale8);
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }

  const QByteArray header =
      QStringLiteral("P5\n%1 %2\n255\n").arg(image.width()).arg(image.height())
          .toLatin1();
  if (file.write(header) != header.size()) {
    return false;
  }

  for (int row = 0; row < image.height(); ++row) {
    const auto *line =
        reinterpret_cast<const char *>(image.constScanLine(row));
    if (file.write(line, image.width()) != image.width()) {
      return false;
    }
  }
  return true;
}

ImageDisplayWidget::ImageDisplayWidget(const QString &title, QWidget *parent)
    : QWidget(parent) {
  // 构造相机预览卡片：标题栏、缩略图区域和点击放大能力。
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);

  titleLabel_ = new QLabel(title, this);
  QFont titleFont = titleLabel_->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() - 1);
  titleLabel_->setFont(titleFont);
  titleLabel_->setAlignment(Qt::AlignCenter);
  titleLabel_->setStyleSheet(
      QStringLiteral("QLabel { background-color: #E0E0E0; border: 1px solid "
                     "#888; padding: 1px; }"));
  titleLabel_->setFixedHeight(20);
  layout->addWidget(titleLabel_);

  imageLabel_ = new ClickableLabel(this);
  imageLabel_->setAlignment(Qt::AlignCenter);
  imageLabel_->setMinimumSize(340, 255);
  // 不设置 MaximumHeight，允许图像区域随窗口自由拉伸
  imageLabel_->setText(QStringLiteral("等待图像流"));
  imageLabel_->setStyleSheet(
      QStringLiteral("QLabel { background-color: #F5F5F5; border: 2px solid "
                     "#888; color: #666; }"));
  layout->addWidget(imageLabel_, 1);

  connect(imageLabel_, &ClickableLabel::clicked, this, [this]() {
    QImage zoomImage = zoomImageProvider_ ? zoomImageProvider_() : QImage{};
    if (zoomImage.isNull()) {
      zoomImage = lastPreviewImage_;
    }
    if (zoomImage.isNull()) {
      return;
    }
    const QPixmap dialogPixmap =
        renderOverlayOnPixmap(QPixmap::fromImage(zoomImage), lastOverlayLines_);
    ZoomableImageDialog dialog(dialogPixmap, this);
    dialog.exec();
  });
}

void ImageDisplayWidget::setZoomImageProvider(std::function<QImage()> provider) {
  // 注入一个懒加载回调，用于点击放大时获取当前最佳原图。
  zoomImageProvider_ = std::move(provider);
}

void ImageDisplayWidget::showCameraFrame(const QImage &previewImage,
                                         const QStringList &overlayLines) {
  // 记录最新预览图和叠字信息，并刷新当前缩略图展示。
  if (previewImage.isNull()) {
    return;
  }
  lastPreviewImage_ = previewImage;
  lastOverlayLines_ = overlayLines;
  refreshPreview();
}

void ImageDisplayWidget::clearImage(const QString &placeholder) {
  // 清空当前预览并显示占位文案，常用于页面切换或等待数据阶段。
  lastPreviewImage_ = QImage{};
  lastOverlayLines_.clear();
  if (imageLabel_) {
    imageLabel_->clear();
    imageLabel_->setText(placeholder);
  }
}

QSize ImageDisplayWidget::displayTargetSize() const {
  // 根据当前控件尺寸推导一个合理的缩放目标，避免预览图过大。
  if (!imageLabel_) {
    return QSize(640, 400);
  }
  const QSize currentSize = imageLabel_->size();
  if (currentSize.width() > 16 && currentSize.height() > 16) {
    return currentSize.boundedTo(QSize(1024, 640));
  }
  const QSize minimum = imageLabel_->minimumSize();
  return QSize(qMax(minimum.width(), 340), qMax(minimum.height(), 255))
      .boundedTo(QSize(1024, 640));
}

void ImageDisplayWidget::resizeEvent(QResizeEvent *event) {
  // 控件尺寸变化时重新渲染缩略图，保持图像清晰度和叠字位置一致。
  QWidget::resizeEvent(event);
  if (!lastPreviewImage_.isNull()) {
    refreshPreview();
  }
}

void ImageDisplayWidget::refreshPreview() {
  // 按当前目标尺寸重绘预览图，并叠加 metadata 覆盖层。
  if (!imageLabel_ || lastPreviewImage_.isNull()) {
    return;
  }

  QPixmap pixmap = QPixmap::fromImage(lastPreviewImage_);
  const QSize targetSize = displayTargetSize();
  if ((pixmap.width() - targetSize.width()) * (pixmap.width() - targetSize.width()) +
          (pixmap.height() - targetSize.height()) * (pixmap.height() - targetSize.height()) >
      64) {
    pixmap = pixmap.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
  }
  imageLabel_->setText(QString());
  imageLabel_->setPixmap(renderOverlayOnPixmap(pixmap, lastOverlayLines_));
}

QPixmap ImageDisplayWidget::renderOverlayOnPixmap(
    const QPixmap &pixmap, const QStringList &overlayLines) const {
  // 在缩略图左上角绘制半透明覆盖信息，如分辨率、曝光和统计值。
  if (pixmap.isNull() || overlayLines.isEmpty()) {
    return pixmap;
  }

  QPixmap rendered = pixmap.copy();
  QPainter painter(&rendered);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setPen(QColor(QStringLiteral("#FF0000")));
  painter.setFont(QFont(QStringLiteral("Arial"), 9));

  const QFontMetrics metrics = painter.fontMetrics();
  int maxWidth = 0;
  int lineCount = 0;
  for (const auto &line : overlayLines) {
    if (line.isEmpty()) {
      continue;
    }
    maxWidth = qMax(maxWidth, metrics.horizontalAdvance(line));
    ++lineCount;
  }

  if (lineCount > 0) {
    const int padding = 6;
    const int lineHeight = metrics.height() + 2;
    const QRect backgroundRect(8, 8, maxWidth + padding * 2,
                               lineCount * lineHeight + padding * 2 - 2);
    painter.fillRect(backgroundRect, QColor(255, 255, 255, 90));

    int y = backgroundRect.top() + padding + metrics.ascent();
    for (const auto &line : overlayLines) {
      if (!line.isEmpty()) {
        painter.drawText(backgroundRect.left() + padding, y, line);
        y += lineHeight;
      }
    }
  }

  return rendered;
}

} // namespace recordlab::widgets
