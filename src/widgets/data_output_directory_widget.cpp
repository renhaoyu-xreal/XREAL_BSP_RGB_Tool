#include "recordlab/widgets/data_output_directory_widget.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace recordlab::widgets {

namespace {

QString uniqueExportPath(const QString &path) {
  if (!QFileInfo::exists(path)) {
    return path;
  }

  const QFileInfo info(path);
  const QDir parent = info.dir();
  const QString stem = info.completeBaseName();
  const QString suffix =
      info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
  QString candidate = parent.filePath(stem + QStringLiteral("_copy") + suffix);
  int counter = 2;
  while (QFileInfo::exists(candidate)) {
    candidate = parent.filePath(
        QStringLiteral("%1_copy%2%3").arg(stem).arg(counter).arg(suffix));
    ++counter;
  }
  return candidate;
}

bool copyPathRecursively(const QString &sourcePath, const QString &targetPath,
                         QString *errorMessage) {
  const QFileInfo sourceInfo(sourcePath);
  if (!sourceInfo.exists()) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("源路径不存在: %1").arg(sourcePath);
    }
    return false;
  }

  if (sourceInfo.isDir()) {
    QDir targetDir(targetPath);
    if (!targetDir.exists() && !QDir().mkpath(targetPath)) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("无法创建目录: %1").arg(targetPath);
      }
      return false;
    }
    const QDir sourceDir(sourcePath);
    const auto entries =
        sourceDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const auto &entry : entries) {
      const QString childTarget = targetDir.filePath(entry.fileName());
      if (!copyPathRecursively(entry.absoluteFilePath(), childTarget,
                               errorMessage)) {
        return false;
      }
    }
    return true;
  }

  QDir().mkpath(QFileInfo(targetPath).dir().absolutePath());
  if (!QFile::copy(sourcePath, targetPath)) {
    if (errorMessage) {
      *errorMessage =
          QStringLiteral("复制失败: %1 -> %2").arg(sourcePath, targetPath);
    }
    return false;
  }
  return true;
}

} // namespace

DataOutputDirectoryWidget::DataOutputDirectoryWidget(const QString &rootPath,
                                                     QWidget *parent)
    : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);

  pathLabel_ = new QLabel(this);
  pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pathLabel_->setStyleSheet(QStringLiteral("color: #555;"));
  layout->addWidget(pathLabel_);

  model_ = new QFileSystemModel(this);
  model_->setReadOnly(true);

  tree_ = new QTreeView(this);
  tree_->setModel(model_);
  tree_->setAlternatingRowColors(true);
  tree_->setSortingEnabled(true);
  tree_->sortByColumn(3, Qt::DescendingOrder);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->header()->setStretchLastSection(false);
  tree_->setColumnWidth(0, 260);
  layout->addWidget(tree_, 1);

  auto *buttons = new QHBoxLayout();
  buttons->setSpacing(6);
  refreshButton_ = new QPushButton(QStringLiteral("刷新目录"), this);
  openButton_ = new QPushButton(QStringLiteral("打开所在目录"), this);
  exportButton_ = new QPushButton(QStringLiteral("导出选中项"), this);
  for (auto *button : {refreshButton_, openButton_, exportButton_}) {
    button->setMinimumHeight(34);
    buttons->addWidget(button);
  }
  layout->addLayout(buttons);

  connect(refreshButton_, &QPushButton::clicked, this,
          &DataOutputDirectoryWidget::refresh);
  connect(openButton_, &QPushButton::clicked, this,
          &DataOutputDirectoryWidget::openSelectedDirectory);
  connect(exportButton_, &QPushButton::clicked, this,
          &DataOutputDirectoryWidget::exportSelectedItems);

  setRootPath(rootPath);
}

void DataOutputDirectoryWidget::setRootPath(const QString &rootPath) {
  rootPath_ = QDir::cleanPath(rootPath);
  QDir().mkpath(rootPath_);
  if (pathLabel_) {
    pathLabel_->setText(rootPath_);
  }
  if (model_ && tree_) {
    model_->setRootPath(rootPath_);
    tree_->setRootIndex(model_->index(rootPath_));
  }
}

void DataOutputDirectoryWidget::refresh() {
  setRootPath(rootPath_);
  emit messageReady(QStringLiteral("已刷新 data 输出目录: %1").arg(rootPath_));
}

QStringList DataOutputDirectoryWidget::selectedDataPaths() const {
  QStringList paths;
  if (!tree_ || !model_ || !tree_->selectionModel()) {
    return paths;
  }

  const QDir rootDir(rootPath_);
  const auto selectedRows = tree_->selectionModel()->selectedRows(0);
  for (const auto &index : selectedRows) {
    const QString path = model_->filePath(index);
    if (path.isEmpty()) {
      continue;
    }
    const QString relative = rootDir.relativeFilePath(path);
    if (relative == QStringLiteral(".") || relative.startsWith(QStringLiteral(".."))) {
      continue;
    }
    if (!paths.contains(path) && QFileInfo::exists(path)) {
      paths << path;
    }
  }
  return paths;
}

void DataOutputDirectoryWidget::openSelectedDirectory() {
  const QStringList paths = selectedDataPaths();
  QString directory = rootPath_;
  if (!paths.isEmpty()) {
    const QFileInfo info(paths.first());
    directory = info.isDir() ? info.absoluteFilePath() : info.dir().absolutePath();
  }
  QDir().mkpath(directory);
  const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
  if (opened) {
    emit messageReady(QStringLiteral("已打开目录: %1").arg(directory));
  } else {
    const QString message = QStringLiteral("无法打开目录: %1").arg(directory);
    emit messageReady(message);
    QMessageBox::warning(this, QStringLiteral("打开失败"), message);
  }
}

void DataOutputDirectoryWidget::exportSelectedItems() {
  const QStringList paths = selectedDataPaths();
  if (paths.isEmpty()) {
    QMessageBox::information(
        this, QStringLiteral("未选择 data 项"),
        QStringLiteral("请先在 data 输出目录中选择要导出的文件或文件夹"));
    return;
  }

  const QString targetDir =
      QFileDialog::getExistingDirectory(this, QStringLiteral("选择导出目标目录"));
  if (targetDir.isEmpty()) {
    return;
  }

  int copied = 0;
  for (const auto &sourcePath : paths) {
    const QFileInfo sourceInfo(sourcePath);
    const QString targetPath =
        uniqueExportPath(QDir(targetDir).filePath(sourceInfo.fileName()));
    QString errorMessage;
    if (!copyPathRecursively(sourcePath, targetPath, &errorMessage)) {
      emit messageReady(QStringLiteral("导出 data 文件失败: %1").arg(errorMessage));
      QMessageBox::warning(this, QStringLiteral("导出失败"), errorMessage);
      return;
    }
    ++copied;
  }

  emit messageReady(
      QStringLiteral("已导出 %1 个 data 项到: %2").arg(copied).arg(targetDir));
}

} // namespace recordlab::widgets
