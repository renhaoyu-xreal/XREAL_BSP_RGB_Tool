#pragma once

#include <QString>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QPushButton;
class QTreeView;

namespace recordlab::widgets {

class DataOutputDirectoryWidget : public QWidget {
  Q_OBJECT

public:
  explicit DataOutputDirectoryWidget(const QString &rootPath,
                                     QWidget *parent = nullptr);

  void setRootPath(const QString &rootPath);
  QString rootPath() const { return rootPath_; }

signals:
  void messageReady(const QString &message);

public slots:
  void refresh();
  void openSelectedDirectory();
  void exportSelectedItems();

private:
  QStringList selectedDataPaths() const;

  QString rootPath_;
  QLabel *pathLabel_ = nullptr;
  QFileSystemModel *model_ = nullptr;
  QTreeView *tree_ = nullptr;
  QPushButton *refreshButton_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QPushButton *exportButton_ = nullptr;
};

} // namespace recordlab::widgets
