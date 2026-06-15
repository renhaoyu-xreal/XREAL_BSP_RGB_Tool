#pragma once

#include "recordlab/core/legacy_config_loader.h"

#include <QList>
#include <QString>

#include <optional>

namespace recordlab::core {

struct DetectedUsbProduct {
    UsbProductCatalogEntry catalogEntry;
    QString rawLine;
    QString vidHex;
    QString pidHex;
};

QString usbIdHex(int value);
QList<DetectedUsbProduct> detectUsbProducts(const QList<UsbProductCatalogEntry>& catalog);
std::optional<DetectedUsbProduct> chooseUsbProductForAgent(
    const QList<DetectedUsbProduct>& products,
    const QString& agentName);

}  // namespace recordlab::core
