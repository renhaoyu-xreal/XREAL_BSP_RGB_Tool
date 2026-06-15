#pragma once

#include <QString>
#include <QStringList>

namespace recordlab::bsp {

struct XrealRuntimeInfo {
    QString projectRoot;
    QString runtimeRoot;
    QString sitePackagesPath;
    QString manifestPath;
    QString qtLibDir;
    QString qtPluginsDir;
    QString shibokenDir;
    QString xrealPackageDir;
    QString nativeLibDir;
    QString glassesServerPath;
    QString requestedQtVersion;
    QString currentProcessQtVersion;
    QString effectiveQtVersion;
    QString effectiveQtSource;
    bool runtimeRootAvailable = false;
    bool sitePackagesAvailable = false;
    bool manifestAvailable = false;
    bool qtLibAvailable = false;
    bool qtPluginsAvailable = false;
    bool shibokenAvailable = false;
    bool xrealPackageAvailable = false;
    bool nativeLibAvailable = false;
    bool glassesServerAvailable = false;
    QStringList blockers;

    bool projectLocalRuntimeAvailable() const;
    bool effectiveQtCompatible() const;
    QString summary() const;
};

class XrealRuntimeLocator {
public:
    static XrealRuntimeInfo probe(const QString& projectRoot);
};

}  // namespace recordlab::bsp
