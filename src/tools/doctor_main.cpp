#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <iostream>

#include "recordlab/bsp/bsp_guide_status.h"
#include "recordlab/bsp/native_glasses_adapter.h"
#include "recordlab/core/app_context.h"

namespace {

QString resolveAppRoot()
{
    // 优先读取环境变量中的工程根目录，其次从可执行文件位置反推。
    QString appRoot = qEnvironmentVariable("RECORDLABC_ROOT");
    if (!appRoot.isEmpty()) {
        return QDir::cleanPath(appRoot);
    }

    QDir appDir(QCoreApplication::applicationDirPath());
    if (appDir.dirName() == QStringLiteral("build")) {
        appDir.cdUp();
    }
    return QDir::cleanPath(appDir.absolutePath());
}

QString resolvePythonExecutable()
{
    // 允许通过环境变量覆盖 Python 解释器，方便复用已有环境。
    const QString fromEnv = qEnvironmentVariable("RECORDLAB_PYTHON_EXECUTABLE").trimmed();
    return fromEnv.isEmpty() ? QStringLiteral("python3") : fromEnv;
}

nlohmann::json runPythonEnvironmentCheck(const QString& appRoot)
{
    // 调用 Python 侧环境检查脚本，并将结果整合成统一 JSON 结构。
    nlohmann::json fallback = {
        {"available", false},
        {"ready", false},
        {"summary", "python environment check unavailable"},
        {"required_missing", nlohmann::json::array()},
        {"optional_missing", nlohmann::json::array()},
    };

    const QString scriptPath = QDir(appRoot).filePath(QStringLiteral("scripts/check_environment.py"));
    if (!QFileInfo::exists(scriptPath)) {
        fallback["error"] = QStringLiteral("missing script: %1").arg(scriptPath).toStdString();
        return fallback;
    }

    const QString pythonExecutable = resolvePythonExecutable();
    QProcess process;
    process.setProgram(pythonExecutable);
    process.setArguments({
        scriptPath,
        QStringLiteral("--project-root"),
        appRoot,
        QStringLiteral("--json"),
    });
    process.start();

    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(5000);
        fallback["error"] = "python environment check timeout";
        fallback["command"] = {
            {"program", pythonExecutable.toStdString()},
            {"script", scriptPath.toStdString()},
        };
        return fallback;
    }

    const QByteArray stdoutBytes = process.readAllStandardOutput();
    const QByteArray stderrBytes = process.readAllStandardError();
    if (stdoutBytes.trimmed().isEmpty()) {
        fallback["error"] = "python environment check produced empty stdout";
        fallback["stderr"] = stderrBytes.toStdString();
        fallback["command"] = {
            {"program", pythonExecutable.toStdString()},
            {"script", scriptPath.toStdString()},
            {"exit_code", process.exitCode()},
        };
        return fallback;
    }

    auto parsed = nlohmann::json::parse(stdoutBytes.constData(), nullptr, false);
    if (parsed.is_discarded()) {
        fallback["error"] = "failed to parse python environment json";
        fallback["stdout"] = stdoutBytes.toStdString();
        fallback["stderr"] = stderrBytes.toStdString();
        fallback["command"] = {
            {"program", pythonExecutable.toStdString()},
            {"script", scriptPath.toStdString()},
            {"exit_code", process.exitCode()},
        };
        return fallback;
    }

    parsed["available"] = true;
    parsed["command"] = {
        {"program", pythonExecutable.toStdString()},
        {"script", scriptPath.toStdString()},
        {"exit_code", process.exitCode()},
        {"stderr", stderrBytes.toStdString()},
    };
    return parsed;
}

void printHumanReadable(const recordlab::core::AppContext& context,
                        const recordlab::bsp::NativeGlassesPreflightReport& preflight,
                        const recordlab::bsp::GuideStatusReport& guideStatus,
                        const nlohmann::json& pythonEnvironment)
{
    // 将诊断结果渲染成适合终端查看的多段文本摘要。
    std::cout << "================ RecordLabC Doctor ================\n";
    std::cout << "app_root: " << context.paths().appRoot.toStdString() << "\n";
    std::cout << "config:   " << context.paths().appConfigPath.toStdString() << "\n";
    std::cout << "guide:    " << context.paths().appGuidePath.toStdString() << "\n";
    std::cout << "wheel:    " << preflight.wheelPath.toStdString() << "\n";
    std::cout << "pyi:      " << preflight.pyiPath.toStdString() << "\n";
    std::cout << "guide_status: " << guideStatus.summary().toStdString() << "\n";
    std::cout << "preflight:    " << preflight.summary().toStdString() << "\n";
    if (pythonEnvironment.contains("summary") && pythonEnvironment["summary"].is_string()) {
        std::cout << "python_env:   " << pythonEnvironment["summary"].get<std::string>() << "\n";
    }

    if (!context.startupWarnings().isEmpty()) {
        std::cout << "startup_warnings:\n";
        for (const auto& warning : context.startupWarnings()) {
            std::cout << "  - " << warning.toStdString() << "\n";
        }
    }

    if (!preflight.blockers.isEmpty()) {
        std::cout << "bsp_blockers:\n";
        for (const auto& blocker : preflight.blockers) {
            std::cout << "  - " << blocker.toStdString() << "\n";
        }
    }

    if (pythonEnvironment.contains("required_missing") && pythonEnvironment["required_missing"].is_array()
        && !pythonEnvironment["required_missing"].empty()) {
        std::cout << "python_required_missing:\n";
        for (const auto& item : pythonEnvironment["required_missing"]) {
            if (item.is_string()) {
                std::cout << "  - " << item.get<std::string>() << "\n";
            }
        }
    }

    if (pythonEnvironment.contains("optional_missing") && pythonEnvironment["optional_missing"].is_array()
        && !pythonEnvironment["optional_missing"].empty()) {
        std::cout << "python_optional_missing:\n";
        for (const auto& item : pythonEnvironment["optional_missing"]) {
            if (item.is_string()) {
                std::cout << "  - " << item.get<std::string>() << "\n";
            }
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    // doctor 聚合启动上下文、BSP 预检、指南状态和 Python 环境检查。
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("recordlabc_doctor"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("RecordLabC 启动诊断工具"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("json"), QStringLiteral("以 JSON 输出完整诊断信息")});
    parser.addOption({QStringLiteral("strict"), QStringLiteral("如果存在阻塞项则返回非零退出码")});
    parser.process(app);

    const auto appRoot = resolveAppRoot();
    const auto context = recordlab::core::AppContext::create(appRoot);
    const recordlab::bsp::NativeGlassesAdapter adapter(appRoot);
    const auto preflight = adapter.preflight();
    const auto guideStatus = recordlab::bsp::BspGuideStatus::evaluate(context);
    const auto pythonEnvironment = runPythonEnvironmentCheck(appRoot);

    nlohmann::json result = {
        {"app_root", appRoot.toStdString()},
        {"startup_ready", context.isReady()},
        {"startup_error", context.startupError().toStdString()},
        {"startup_warnings", nlohmann::json::array()},
        {"guide_status",
         {{"summary", guideStatus.summary().toStdString()},
          {"structural_percent", guideStatus.structuralPercent()},
          {"operational_percent", guideStatus.operationalPercent()},
          {"blocking_items", nlohmann::json::array()}}},
        {"bsp_preflight", preflight.toJson()},
        {"python_environment", pythonEnvironment},
    };

    for (const auto& warning : context.startupWarnings()) {
        result["startup_warnings"].push_back(warning.toStdString());
    }
    for (const auto& item : guideStatus.blockingItems()) {
        result["guide_status"]["blocking_items"].push_back(item.toStdString());
    }

    if (parser.isSet(QStringLiteral("json"))) {
        std::cout << result.dump(2) << std::endl;
    } else {
        printHumanReadable(context, preflight, guideStatus, pythonEnvironment);
    }

    if (!context.startupError().isEmpty()) {
        return 1;
    }

    if (parser.isSet(QStringLiteral("strict")) && !preflight.blockers.isEmpty()) {
        return 2;
    }

    return 0;
}
