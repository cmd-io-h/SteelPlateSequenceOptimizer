/**
 * @file main.cpp
 * @brief 启动图形界面或命令行排产流程
 * @author cmd-io-h
 */

#include "MainWindow.h"
#include "OptimizerConfig.h"
#include "SequenceOptimizer.h"
#include "WorkbookService.h"

#include "xlsxdocument.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace
{
// 在发布目录和开发目录中查找默认参数文件
QString defaultConfigPath()
{
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/config/default.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../../config/default.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../config/default.json")
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) return QFileInfo(path).absoluteFilePath();
    }
    return QString();
}

// 将命令行处理失败原因写入独立错误文件
void writeCliError(const QString& outputPath, const QString& stage, const QString& message)
{
    if (outputPath.isEmpty()) return;
    QFile file(outputPath + QStringLiteral(".error.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream << stage << QStringLiteral("：") << message << '\n';
}
}

// 选择命令行批处理模式或启动图形界面模式
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("XiGeo"));
    QCoreApplication::setApplicationName(QStringLiteral("SteelPlateSequenceOptimizer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("钢板分组套料与坡口/焊接流水线均衡工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption inputOption(
        QStringList() << QStringLiteral("i") << QStringLiteral("input"),
        QStringLiteral("输入原始工件表或钢板明细 Excel"), QStringLiteral("xlsx"));
    QCommandLineOption outputOption(
        QStringList() << QStringLiteral("o") << QStringLiteral("output"),
        QStringLiteral("输出 Excel"), QStringLiteral("xlsx"));
    QCommandLineOption configOption(
        QStringList() << QStringLiteral("c") << QStringLiteral("config"),
        QStringLiteral("JSON 参数文件"), QStringLiteral("json"));
    QCommandLineOption smokeOption(
        QStringList() << QStringLiteral("qxlsx-smoke"),
        QStringLiteral("执行 QXlsx 写入/读取自检"), QStringLiteral("xlsx"));
    parser.addOption(inputOption);
    parser.addOption(outputOption);
    parser.addOption(configOption);
    parser.addOption(smokeOption);
    parser.process(application);

    // QXlsx 自检模式用于确认运行环境能够正常读写工作簿
    if (parser.isSet(smokeOption)) {
        const QString smokePath = parser.value(smokeOption);
        QXlsx::Document created;
        created.write(1, 1, QStringLiteral("钢板明细"));
        if (!created.saveAs(smokePath)) return 10;
        QXlsx::Document reopened(smokePath);
        if (!reopened.load() || reopened.sheetNames().isEmpty()) return 11;
        return 0;
    }

    const QString inputPath = parser.value(inputOption);
    const QString outputPath = parser.value(outputOption);
    // 同时提供输入和输出路径时直接执行无界面的批处理流程
    if (!inputPath.isEmpty() || !outputPath.isEmpty()) {
        if (inputPath.isEmpty() || outputPath.isEmpty()) return 2;

        OptimizerConfig config = OptimizerConfig::defaults();
        const QString configPath = parser.isSet(configOption)
            ? parser.value(configOption)
            : defaultConfigPath();
        QString error;
        if (!configPath.isEmpty() &&
            !OptimizerConfig::loadFromFile(configPath, &config, &error)) {
            writeCliError(outputPath, QStringLiteral("参数加载失败"), error);
            return 3;
        }

        WorkbookData data;
        if (!WorkbookService::load(
                inputPath, config,
                true, false, false,
                &data, &error)) {
            writeCliError(outputPath, QStringLiteral("Excel读取失败"), error);
            return 4;
        }
        const QVector<int> order = SequenceOptimizer::optimize(data.plates, config);
        if (!WorkbookService::exportReordered(data, order, outputPath, &error)) {
            writeCliError(outputPath, QStringLiteral("Excel导出失败"), error);
            return 5;
        }
        return 0;
    }

    // 未提供命令行文件参数时启动桌面操作界面
    MainWindow window;
    window.show();
    return application.exec();
}
