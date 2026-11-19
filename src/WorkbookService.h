#pragma once

#include "OptimizerConfig.h"
#include "SteelPlateModel.h"

// 保存单个结果工作表的名称、表头和数据行
struct WorkbookSheetData
{
    QString name;
    QStringList headers;
    QVector<QVector<QVariant>> rows;
};

// 负责识别 Excel 输入类型、读取数据并导出排产结果
class WorkbookService
{
public:
    static bool load(
        const QString& inputPath,
        const OptimizerConfig& config,
        bool includeSmallDiameter,
        bool includeMediumDiameter,
        bool includeLargeDiameter,
        WorkbookData* data,
        QString* errorMessage); // 导入工作簿并按管径选择生成钢板数据

    static bool exportReordered(
        const WorkbookData& data,
        const QVector<int>& order,
        const QString& outputPath,
        QString* errorMessage); // 按优化后的钢板顺序导出原工作簿数据

    static bool exportResultTables(
        const QVector<WorkbookSheetData>& sheets,
        const QString& outputPath,
        QString* errorMessage); // 将多张结果表一次性导出到同一工作簿

    static QString defaultOutputPath(
        const QString& inputPath,
        const QString& suffix); // 根据输入路径和后缀生成默认导出路径
};
