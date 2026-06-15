#pragma once

#include "recordlab/core/qt_json_compat.h"

#include <QHash>
#include <QString>
#include <QWidget>

#include <deque>

class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QCheckBox;

namespace recordlab::widgets {

class NvizMessageTreeWidget : public QWidget {
  Q_OBJECT

public:
  explicit NvizMessageTreeWidget(QWidget *parent = nullptr);

  bool loadFromPlotJson(const QString &plotJsonPath);
  void handleTreeData(const nlohmann::json &value);
  void clearFieldSelection();

signals:
  void fieldSelectionChanged(const QString &displayName,
                             const QString &fieldKey);
  void fieldSelectionsChanged(const QStringList &displayNames,
                              const QStringList &fieldKeys);
  void selectedFieldSample(const QString &fieldKey, const QString &displayName,
                           double timestamp, double value);

private:
  struct FieldBinding {
    QStandardItem *nameItem = nullptr;
    QStandardItem *valueItem = nullptr;
    QStandardItem *typeItem = nullptr;
    QString displayName;
  };

  QTreeView *treeView_ = nullptr;
  QStandardItemModel *model_ = nullptr;
  QCheckBox *filterCheck_ = nullptr;
  bool showOnlyNonZero_ = false;
  QHash<QString, QStandardItem *> groupItems_;
  QHash<QString, QStandardItem *> messageValueItems_;
  QHash<QString, FieldBinding> fieldBindings_;
  QHash<QString, std::deque<double>> messageArrivalTimes_;
  QStandardItem *selectedFieldItem_ = nullptr;
  QString selectedFieldKey_;
  QString selectedFieldDisplayName_;
  QHash<QString, bool> selectedFieldKeys_;  // 多选支持
  bool suppressItemChanged_ = false;

  void onItemChanged(QStandardItem *item);
  void applyDataFilter();
  bool itemHasVisibleData(QStandardItem *nameItem) const;
  bool fieldHasData(const FieldBinding &binding) const;
  void selectFieldFromIndex(const QModelIndex &index);
  void resetModel();
  QStandardItem *ensureGroupItem(int groupId, const QString &groupName);
  QString messageKey(int groupId, int msgId) const;
  QString fieldKey(int groupId, int msgId, const QString &fieldName) const;
  QString valueText(const nlohmann::json &value) const;
  double currentFrequencyForMessage(const QString &key);
};

} // namespace recordlab::widgets
