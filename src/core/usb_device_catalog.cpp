#include "recordlab/core/usb_device_catalog.h"

#include <QProcess>
#include <QRegularExpression>

namespace recordlab::core {

QString usbIdHex(int value)
{
    return QStringLiteral("0x%1")
        .arg(value, 4, 16, QLatin1Char('0'))
        .toLower();
}

QList<DetectedUsbProduct> detectUsbProducts(const QList<UsbProductCatalogEntry>& catalog)
{
    QProcess process;
    process.start(QStringLiteral("lsusb"));
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(300);
        return {};
    }
    if (process.exitCode() != 0) {
        return {};
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QRegularExpression idPattern(
        QStringLiteral("\\bID\\s+([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})\\b"));
    const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    QList<DetectedUsbProduct> detected;
    for (const auto& line : lines) {
        const auto match = idPattern.match(line);
        if (!match.hasMatch()) {
            continue;
        }

        bool vidOk = false;
        bool pidOk = false;
        const int vid = match.captured(1).toInt(&vidOk, 16);
        const int pid = match.captured(2).toInt(&pidOk, 16);
        if (!vidOk || !pidOk) {
            continue;
        }

        for (const auto& entry : catalog) {
            if (entry.vid == vid && entry.pid == pid) {
                detected.push_back({entry, line, usbIdHex(vid), usbIdHex(pid)});
            }
        }
    }

    return detected;
}

std::optional<DetectedUsbProduct> chooseUsbProductForAgent(
    const QList<DetectedUsbProduct>& products,
    const QString& agentName)
{
    QList<DetectedUsbProduct> candidates;
    const QString trimmedAgent = agentName.trimmed();
    for (const auto& product : products) {
        if (trimmedAgent.isEmpty() || product.catalogEntry.agentName == trimmedAgent) {
            candidates.push_back(product);
        }
    }
    if (candidates.isEmpty()) {
        return std::nullopt;
    }

    for (const auto& product : candidates) {
        if (product.catalogEntry.sshPreferred) {
            return product;
        }
    }
    return candidates.front();
}

}  // namespace recordlab::core
