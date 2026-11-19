#pragma once

#include <QString>
#include <QVector>

// 定义钢板明细和原始工件表中的关键列位置
struct ColumnSettings
{
    // 已生成“钢板明细”中的列号（1-based）
    int plateId = 1;          // A
    int material = 2;         // B
    int plateWidth = 3;       // C
    int plateThickness = 4;   // D
    int plateNumber = 5;      // E
    int usedLength = 7;       // G
    int remainingLength = 8;  // H
    int specification = 19;   // S
    int bevelCount = 25;      // Y
    int lapWeldCount = 30;    // AD

    // 原始工件表中的列号（1-based）
    int rawSpecification = 11; // K，格式通常为“宽度*厚度”
    int rawMaterial = 12;      // L
    int rawLength = 15;        // O
    int rawBevel = 16;         // P
    int rawOnlineAuto = 20;    // T
    int rawOnlineManual = 21;  // U
};

// 定义按材质和规格覆盖默认钢板长度的规则
struct StockLengthRule
{
    bool enabled = true;
    QString material;       // 空白表示任意材质
    double thickness = 0.0; // 0 表示任意厚度
    double width = 0.0;     // 0 表示任意宽度
    double length = 6000.0;
    int priority = 100;
};

// 表示包含最小值和最大值的焊接宽度范围
struct WidthRange
{
    double minimum = 0.0;
    double maximum = 0.0;
};

// 定义单个流水线均衡目标的评分参数
struct BalanceObjectiveSetting
{
    bool enabled = true;
    int priority = 1;        // 数字越小，优先级越高
    double weight = 1.0;     // 同一优先级内的相对权重
    double tolerance = 0.02; // 归一化误差在此范围内视为同等结果
};

// 汇总套料、焊接路线、均衡算法和时序仿真参数
struct OptimizerConfig
{
    // auto: 自动识别；raw: 原始工件表；detail: 已生成钢板明细
    QString inputMode = QStringLiteral("auto");
    QString rawWorksheetName;
    QString worksheetName = QStringLiteral("钢板明细");
    QString plateMarkerPattern = QStringLiteral("^G");
    QString outputSuffix = QStringLiteral("_钢板明细_流水线均衡");
    int headerRow = 1;
    ColumnSettings columns;

    // 套料硬约束，规格就是“厚度 + 宽度”
    bool groupByMaterial = true;
    bool groupByThickness = true;
    bool groupByWidth = true;
    double defaultStockLength = 6000.0;
    double kerf = 0.0;
    double endAllowance = 0.0; // 每张钢板首尾余量合计
    QVector<double> lengthThresholds { 0.0 }; // 0 表示默认不启用长短分界
    QVector<StockLengthRule> stockLengthRules;

    // 焊接路线，小径人工单独统计，小径机器、中径和大径合并为机器接焊负荷
    QVector<WidthRange> manualWeldRanges { { 22.0, 34.0 } };
    QVector<WidthRange> robotWeldRanges { { 42.0, 60.0 } };
    QVector<WidthRange> mediumWeldRanges { { 76.0, 114.0 } };
    QVector<WidthRange> largeWeldRanges { { 140.0, 377.0 } };

    // 界面只显示“坡口、焊接”两个目标，人工与机器焊仍作为两股负荷分别均衡
    BalanceObjectiveSetting bevelObjective { true, 1, 1.0, 0.02 };
    BalanceObjectiveSetting manualWeldObjective { true, 2, 1.0, 0.02 };
    BalanceObjectiveSetting robotWeldObjective { true, 2, 1.0, 0.02 };
    double prefixBalanceWeight = 2.0;
    double adjacentLoadWeight = 4.0;
    double rollingWindowWeight = 0.4;
    int rollingWindow = 5;
    double highLoadFactor = 1.0;
    int localSearchIterations = 2500;

    // 时序模拟参数，单位均为秒或毫米
    double downstreamMinimumLength = 600.0;
    int bufferCapacity = 3;
    double sprayIncrementSeconds = 18.0;
    double sawFeedSpeedMmPerSecond = 200.0;
    double sawFirstFeedSeconds = 10.0;
    double sawMaximumFeedSeconds = 10.0;
    double sawClampSeconds = 2.0;
    double sawReleaseSeconds = 2.0;
    double sawUnloadSeconds = 15.0;
    double sawPlateResetSeconds = 30.0;
    double sawCutSecondsUnder35 = 8.0;
    double sawCutSecondsUnder61 = 12.0;
    double sawCutSecondsUnder90 = 13.0;
    double sawCutSecondsOther = 15.0;
    int bevelStationCount = 1;
    double bevelCycleSeconds = 85.0;
    double tackCycleSeconds = 160.0;
    int manualWeldStationCount = 2;
    double manualWeldCycleSeconds = 178.0;
    int robotWeldStationCount = 2;
    double robotWeldFixedSeconds = 109.0;
    double robotWeldSpeedMmPerSecond = 5.0;

    static OptimizerConfig defaults(); // 创建默认参数配置
    static bool loadFromFile(const QString& path, OptimizerConfig* config, QString* errorMessage); // 从文件读取参数配置
    bool saveToFile(const QString& path, QString* errorMessage) const; // 将当前参数配置保存到文件
    bool validate(QString* errorMessage) const; // 检查参数配置是否合法
    double stockLengthFor(const QString& material, double thickness, double width) const; // 获取指定材质和规格的钢板长度
    bool isManualWeldWidth(double width) const; // 判断宽度是否采用小径人工接焊
    bool isRobotWeldWidth(double width) const; // 判断宽度是否采用小径机器接焊
    bool isMediumWeldWidth(double width) const; // 判断宽度是否属于中径范围
    bool isLargeWeldWidth(double width) const; // 判断宽度是否属于大径范围
    double sprayPlateSeconds(int workpieceCount) const; // 按钢板工件数计算喷码总时间
};
