#include "recordlab/widgets/nviz_message_tree_widget.h"

#include <QFile>
#include <QCheckBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace recordlab::widgets {

namespace {

enum Columns { NameColumn = 0, ValueColumn = 1, TypeColumn = 2 };
constexpr int FieldKeyRole = Qt::UserRole + 1;
constexpr int FieldDisplayRole = Qt::UserRole + 2;

double monotonicNowSec() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

QString normalizedGroupName(const nlohmann::json &config, int groupId) {
  QString name = QStringLiteral("GROUP_%1").arg(groupId);
  try {
    if (config.contains("GROUP_NAME") && config["GROUP_NAME"].is_string()) {
      name = QString::fromStdString(config["GROUP_NAME"].get<std::string>());
    }
  } catch (...) {
  }
  name.remove(QStringLiteral("MSG_GROUP_"));
  return name;
}

QString parseFieldType(const QString &fieldDef) {
  return fieldDef.section(QChar(' '), 0, 0).trimmed();
}

QString parseFieldName(const QString &fieldDef) {
  return fieldDef.section(QChar(' '), 1).trimmed();
}

int arrayCount(QString *fieldName) {
  const int bracketStart = fieldName->indexOf(QChar('['));
  if (bracketStart < 0) {
    return 1;
  }
  const int bracketEnd = fieldName->indexOf(QChar(']'), bracketStart);
  if (bracketEnd < 0) {
    return 1;
  }
  bool ok = false;
  const int count =
      fieldName->mid(bracketStart + 1, bracketEnd - bracketStart - 1).toInt(&ok);
  *fieldName = fieldName->left(bracketStart);
  return ok && count > 0 ? count : 1;
}

QString normalizedFieldName(QString rawName, int index, int count) {
  if (rawName.contains(QStringLiteral("ts_"))) {
    rawName = QStringLiteral("timestamp");
  }
  if (count > 1) {
    return QStringLiteral("%1[%2]").arg(rawName).arg(index);
  }
  return rawName;
}

} // namespace

NvizMessageTreeWidget::NvizMessageTreeWidget(QWidget *parent)
    : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  // 过滤开关：仅显示有数据的项
  filterCheck_ = new QCheckBox(QStringLiteral("仅显示有数据项"), this);
  filterCheck_->setChecked(false);
  layout->addWidget(filterCheck_);

  treeView_ = new QTreeView(this);
  treeView_->setUniformRowHeights(true);
  treeView_->setAnimated(true);
  treeView_->setAlternatingRowColors(true);
  treeView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  treeView_->setStyleSheet(QStringLiteral(
      "QTreeView { background-color: #f7f8fb; border: 1px solid #9aa4b2; }"
      "QTreeView::item:selected { background-color: #d7e7ff; }"));
  layout->addWidget(treeView_);
  connect(filterCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    showOnlyNonZero_ = checked;
    applyDataFilter();
  });
  resetModel();
}

bool NvizMessageTreeWidget::loadFromPlotJson(const QString &plotJsonPath) {
  QFile file(plotJsonPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    std::cerr << "[NvizMessageTreeWidget] Failed to open "
              << plotJsonPath.toStdString() << std::endl;
    return false;
  }

  resetModel();

  try {
    const auto config = nlohmann::json::parse(file.readAll().toStdString());
    for (auto it = config.begin(); it != config.end(); ++it) {
      const auto &messageConfig = it.value();
      if (!messageConfig.is_object() || !messageConfig.contains("GROUP_ID") ||
          !messageConfig.contains("MSG_ID")) {
        continue;
      }

      const int groupId = messageConfig.value("GROUP_ID", -1);
      const int msgId = messageConfig.value("MSG_ID", -1);
      if (groupId < 0 || msgId < 0) {
        continue;
      }

      const QString groupName = normalizedGroupName(messageConfig, groupId);
      const QString msgName = QString::fromStdString(it.key());
      auto *groupItem = ensureGroupItem(groupId, groupName);

      auto *messageNameItem = new QStandardItem(msgName);
      auto *messageValueItem = new QStandardItem(QStringLiteral("-- Hz"));
      auto *messageTypeItem = new QStandardItem(QStringLiteral("Hz"));
      messageValueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      groupItem->appendRow({messageNameItem, messageValueItem, messageTypeItem});

      const QString msgKey = messageKey(groupId, msgId);
      messageValueItems_.insert(msgKey, messageValueItem);

      if (!messageConfig.contains("struct") ||
          !messageConfig["struct"].is_array()) {
        continue;
      }

      for (const auto &fieldDefJson : messageConfig["struct"]) {
        if (!fieldDefJson.is_string()) {
          continue;
        }
        const QString fieldDef =
            QString::fromStdString(fieldDefJson.get<std::string>());
        const QString type = parseFieldType(fieldDef);
        QString rawName = parseFieldName(fieldDef);
        if (type.isEmpty() || rawName.isEmpty() ||
            rawName.contains(QStringLiteral("HIDE"))) {
          continue;
        }

        const int count = arrayCount(&rawName);
        for (int index = 0; index < count; ++index) {
          const QString fieldName = normalizedFieldName(rawName, index, count);
          const QString key = fieldKey(groupId, msgId, fieldName);
          const QString displayName =
              QStringLiteral("Nviz: %1/%2/%3").arg(groupName, msgName, fieldName);

          auto *fieldNameItem = new QStandardItem(fieldName);
          fieldNameItem->setCheckable(true);
          fieldNameItem->setEditable(false);
          fieldNameItem->setData(key, FieldKeyRole);
          fieldNameItem->setData(displayName, FieldDisplayRole);
          auto *fieldValueItem = new QStandardItem(QStringLiteral("--"));
          auto *fieldTypeItem = new QStandardItem(type);
          fieldValueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
          messageNameItem->appendRow(
              {fieldNameItem, fieldValueItem, fieldTypeItem});
          fieldBindings_.insert(
              key, FieldBinding{fieldNameItem, fieldValueItem, fieldTypeItem,
                                displayName});
        }
      }
    }

    treeView_->expandAll();
    treeView_->resizeColumnToContents(NameColumn);
    treeView_->resizeColumnToContents(ValueColumn);
    treeView_->resizeColumnToContents(TypeColumn);
    treeView_->collapseAll();
    for (int row = 0; row < model_->rowCount(); ++row) {
      treeView_->expand(model_->item(row, NameColumn)->index());
    }
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[NvizMessageTreeWidget] plot.json parse error: " << e.what()
              << std::endl;
    resetModel();
    return false;
  }
}

void NvizMessageTreeWidget::handleTreeData(const nlohmann::json &value) {
  static uint64_t updateCount = 0;
  static uint64_t missingMessageCount = 0;
  static uint64_t missingFieldCount = 0;
  if (!value.is_object()) {
    return;
  }

  const int groupId = value.value("group_id", -1);
  const int msgId = value.value("msg_id", -1);
  if (groupId < 0 || msgId < 0 || !value.contains("fields") ||
      !value["fields"].is_array()) {
    return;
  }

  const QString msgKey = messageKey(groupId, msgId);
  if (auto *messageValueItem = messageValueItems_.value(msgKey, nullptr)) {
    const double freq = value.contains("frequency_hz")
                            ? value.value("frequency_hz", 0.0)
                            : currentFrequencyForMessage(msgKey);
    const int precision = freq >= 100.0 ? 0 : 1;
    messageValueItem->setText(
        QStringLiteral("%1 Hz").arg(freq, 0, 'f', precision));
  } else {
    ++missingMessageCount;
    if (missingMessageCount <= 20 || missingMessageCount % 1000 == 0) {
      std::cerr << "[NvizMessageTreeWidget] message not in tree, group="
                << groupId << " msg=" << msgId << std::endl;
    }
  }

  double timestamp = value.value("timestamp", 0.0);
  if (timestamp > 1e12) {
    timestamp /= 1e9;
  } else if (timestamp > 1e9) {
    timestamp /= 1e6;
  }

  for (const auto &field : value["fields"]) {
    if (!field.is_object() || !field.contains("name")) {
      continue;
    }
    const QString fieldName =
        QString::fromStdString(field.value("name", std::string()));
    const QString key = fieldKey(groupId, msgId, fieldName);
    auto it = fieldBindings_.find(key);
    if (it == fieldBindings_.end()) {
      ++missingFieldCount;
      if (missingFieldCount <= 20 || missingFieldCount % 1000 == 0) {
        std::cerr << "[NvizMessageTreeWidget] field not in tree, key="
                  << key.toStdString() << std::endl;
      }
      continue;
    }

    const auto rawValue = field.value("value", nlohmann::json());
    it->valueItem->setText(valueText(rawValue));
    ++updateCount;
    if (updateCount <= 20 || updateCount % 1000 == 0) {
      std::cerr << "[NvizMessageTreeWidget] updated key=" << key.toStdString()
                << " value=" << valueText(rawValue).toStdString()
                << " count=" << updateCount << std::endl;
    }

    // 对所有选中的字段发送样本
    if (selectedFieldKeys_.contains(key) && rawValue.is_number()) {
      emit selectedFieldSample(key, it->displayName, timestamp,
                               rawValue.get<double>());
    }
  }

  if (showOnlyNonZero_) {
    applyDataFilter();
  }
}

void NvizMessageTreeWidget::clearFieldSelection() {
  if (selectedFieldKeys_.isEmpty() && !selectedFieldItem_) {
    return;
  }
  suppressItemChanged_ = true;
  for (auto it = selectedFieldKeys_.begin(); it != selectedFieldKeys_.end(); ++it) {
    const auto bindingIt = fieldBindings_.find(it.key());
    if (bindingIt != fieldBindings_.end() && bindingIt->nameItem) {
      bindingIt->nameItem->setCheckState(Qt::Unchecked);
    }
  }
  if (selectedFieldItem_) {
    selectedFieldItem_->setCheckState(Qt::Unchecked);
  }
  suppressItemChanged_ = false;
  selectedFieldKeys_.clear();
  selectedFieldItem_ = nullptr;
  selectedFieldKey_.clear();
  selectedFieldDisplayName_.clear();
  emit fieldSelectionsChanged({}, {});
  emit fieldSelectionChanged(QString(), QString());
}

void NvizMessageTreeWidget::onItemChanged(QStandardItem *item) {
  if (suppressItemChanged_ || !item || !item->isCheckable()) {
    return;
  }

  const QString key = item->data(FieldKeyRole).toString();
  const QString displayName = item->data(FieldDisplayRole).toString();
  if (key.isEmpty()) {
    return;
  }

  // 多选逻辑：维护selectedFieldKeys_集合
  if (item->checkState() == Qt::Checked) {
    selectedFieldKeys_[key] = true;
  } else {
    selectedFieldKeys_.remove(key);
  }

  // 发送多选信号
  QStringList displayNames;
  QStringList fieldKeys;
  for (auto it = selectedFieldKeys_.begin(); it != selectedFieldKeys_.end(); ++it) {
    const QString &fieldKey = it.key();
    if (fieldBindings_.contains(fieldKey)) {
      displayNames << fieldBindings_[fieldKey].displayName;
      fieldKeys << fieldKey;
    }
  }
  emit fieldSelectionsChanged(displayNames, fieldKeys);
  emit fieldSelectionChanged(
      displayNames.isEmpty() ? QString() : displayNames.last(),
      fieldKeys.isEmpty() ? QString() : fieldKeys.last());
}

void NvizMessageTreeWidget::applyDataFilter() {
  if (!treeView_ || !model_) {
    return;
  }

  const auto applyChildren = [this](QStandardItem *parentItem, const auto &self) -> bool {
    if (!parentItem) {
      return false;
    }

    bool anyVisible = false;
    for (int row = 0; row < parentItem->rowCount(); ++row) {
      auto *child = parentItem->child(row, NameColumn);
      if (!child) {
        continue;
      }

      bool childVisible = true;
      if (showOnlyNonZero_) {
        childVisible = itemHasVisibleData(child);
        if (child->rowCount() > 0) {
          childVisible = self(child, self);
        }
      } else if (child->rowCount() > 0) {
        self(child, self);
      }

      treeView_->setRowHidden(row, parentItem->index(), !childVisible);
      anyVisible = anyVisible || childVisible;
    }
    return anyVisible;
  };

  for (int row = 0; row < model_->rowCount(); ++row) {
    auto *groupItem = model_->item(row, NameColumn);
    bool visible = true;
    if (showOnlyNonZero_) {
      visible = applyChildren(groupItem, applyChildren);
    } else {
      applyChildren(groupItem, applyChildren);
    }
    treeView_->setRowHidden(row, QModelIndex(), !visible);
  }
}

bool NvizMessageTreeWidget::itemHasVisibleData(QStandardItem *nameItem) const {
  if (!nameItem) {
    return false;
  }

  const QString key = nameItem->data(FieldKeyRole).toString();
  if (!key.isEmpty()) {
    const auto it = fieldBindings_.find(key);
    return it != fieldBindings_.end() && fieldHasData(it.value());
  }

  for (int row = 0; row < nameItem->rowCount(); ++row) {
    if (itemHasVisibleData(nameItem->child(row, NameColumn))) {
      return true;
    }
  }
  return false;
}

bool NvizMessageTreeWidget::fieldHasData(const FieldBinding &binding) const {
  if (!binding.valueItem) {
    return false;
  }
  bool ok = false;
  const double value = binding.valueItem->text().toDouble(&ok);
  return ok && std::isfinite(value);
}

void NvizMessageTreeWidget::selectFieldFromIndex(const QModelIndex &index) {
  if (!index.isValid() || !model_) {
    return;
  }

  auto *item = model_->itemFromIndex(index.sibling(index.row(), NameColumn));
  if (!item || !item->isCheckable() ||
      item->data(FieldKeyRole).toString().isEmpty()) {
    return;
  }

  if (item->checkState() != Qt::Checked) {
    item->setCheckState(Qt::Checked);
  }
}

void NvizMessageTreeWidget::resetModel() {
  groupItems_.clear();
  messageValueItems_.clear();
  fieldBindings_.clear();
  messageArrivalTimes_.clear();
  selectedFieldItem_ = nullptr;
  selectedFieldKey_.clear();
  selectedFieldDisplayName_.clear();

  if (model_) {
    model_->deleteLater();
  }
  model_ = new QStandardItemModel(treeView_);
  model_->setHorizontalHeaderLabels(
      {QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Type")});
  treeView_->setModel(model_);
  treeView_->header()->setStretchLastSection(true);
  connect(model_, &QStandardItemModel::itemChanged, this,
          &NvizMessageTreeWidget::onItemChanged);
  connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged,
          this, [this](const QModelIndex &current, const QModelIndex &) {
            selectFieldFromIndex(current);
          });
}

QStandardItem *NvizMessageTreeWidget::ensureGroupItem(int groupId,
                                                      const QString &groupName) {
  const QString key = QString::number(groupId);
  if (auto *existing = groupItems_.value(key, nullptr)) {
    return existing;
  }

  auto *nameItem = new QStandardItem(groupName);
  auto *valueItem = new QStandardItem();
  auto *typeItem = new QStandardItem();
  model_->appendRow({nameItem, valueItem, typeItem});
  groupItems_.insert(key, nameItem);
  return nameItem;
}

QString NvizMessageTreeWidget::messageKey(int groupId, int msgId) const {
  return QStringLiteral("%1:%2").arg(groupId).arg(msgId);
}

QString NvizMessageTreeWidget::fieldKey(int groupId, int msgId,
                                        const QString &fieldName) const {
  return QStringLiteral("%1:%2:%3").arg(groupId).arg(msgId).arg(fieldName);
}

QString NvizMessageTreeWidget::valueText(const nlohmann::json &value) const {
  if (!value.is_number()) {
    return QStringLiteral("--");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    return QStringLiteral("--");
  }
  return QStringLiteral("%1").arg(number, 0, 'g', 10);
}

double NvizMessageTreeWidget::currentFrequencyForMessage(const QString &key) {
  auto &times = messageArrivalTimes_[key];
  const double now = monotonicNowSec();
  times.push_back(now);
  while (!times.empty() && times.front() < now - 1.0) {
    times.pop_front();
  }
  return times.size() > 1 ? static_cast<double>(times.size() - 1) /
                                std::max(0.001, times.back() - times.front())
                          : 0.0;
}

} // namespace recordlab::widgets
