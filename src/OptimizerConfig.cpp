#include "OptimizerConfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double Epsilon = 1e-7;

// 读取整数参数并在缺失时返回默认值
int readInt(const QJsonObject& object, const char* key, int fallback)
{
    return object.contains(key) ? object.value(key).toInt(fallback) : fallback;
}

// 读取浮点参数并在缺失时返回默认值
double readDouble(const QJsonObject& object, const char* key, double fallback)
{
    return object.contains(key) ? object.value(key).toDouble(fallback) : fallback;
}

// 读取布尔参数并在缺失时返回默认值
bool readBool(const QJsonObject& object, const char* key, bool fallback)
{
    return object.contains(key) ? object.value(key).toBool(fallback) : fallback;
}

// 读取文本参数并在缺失时返回默认值
QString readString(const QJsonObject& object, const char* key, const QString& fallback)
{
    return object.contains(key) ? object.value(key).toString(fallback) : fallback;
}

// 读取浮点数组并过滤无效内容
QVector<double> readDoubleArray(
    const QJsonObject& object,
    const char* key,
    const QVector<double>& fallback)
{
    if (!object.value(key).isArray()) return fallback;
    QVector<double> values;
    const QJsonArray array = object.value(key).toArray();
    for (const QJsonValue& value : array) {
        const double number = value.toDouble(-1.0);
        if (number >= 0.0) values.append(number);
    }
    return values.isEmpty() ? fallback : values;
}

// 将浮点列表转换为 JSON 数组
QJsonArray toJsonArray(const QVector<double>& values)
{
    QJsonArray array;
    for (double value : values) array.append(value);
    return array;
}

// 读取新版或旧版格式的焊接宽度范围
QVector<WidthRange> readRanges(
    const QJsonObject& object,
    const char* rangeKey,
    const char* legacyWidthKey,
    const QVector<WidthRange>& fallback)
{
    if (object.value(rangeKey).isArray()) {
        QVector<WidthRange> ranges;
        const QJsonArray array = object.value(rangeKey).toArray();
        for (const QJsonValue& value : array) {
            if (!value.isObject()) continue;
            const QJsonObject rangeObject = value.toObject();
            WidthRange range;
            range.minimum = readDouble(rangeObject, "min", 0.0);
            range.maximum = readDouble(rangeObject, "max", range.minimum);
            if (range.minimum > range.maximum) std::swap(range.minimum, range.maximum);
            ranges.append(range);
        }
        if (!ranges.isEmpty()) return ranges;
    }
    if (object.value(legacyWidthKey).isArray()) {
        QVector<WidthRange> ranges;
        for (const QJsonValue& value : object.value(legacyWidthKey).toArray()) {
            const double width = value.toDouble(-1.0);
            if (width > 0.0) ranges.append({ width, width });
        }
        if (!ranges.isEmpty()) return ranges;
    }
    return fallback;
}

// 将焊接宽度范围转换为 JSON 数组
QJsonArray rangesToJson(const QVector<WidthRange>& ranges)
{
    QJsonArray array;
    for (const WidthRange& range : ranges) {
        QJsonObject object;
        object.insert(QStringLiteral("min"), range.minimum);
        object.insert(QStringLiteral("max"), range.maximum);
        array.append(object);
    }
    return array;
}

// 读取单个均衡目标的启用状态和评分参数
void readObjective(
    const QJsonObject& parent,
    const QString& key,
    BalanceObjectiveSetting* setting)
{
    if (!setting || !parent.value(key).isObject()) return;
    const QJsonObject object = parent.value(key).toObject();
    setting->enabled = readBool(object, "enabled", setting->enabled);
    setting->priority = readInt(object, "priority", setting->priority);
    setting->weight = readDouble(object, "weight", setting->weight);
    setting->tolerance = readDouble(object, "tolerance", setting->tolerance);
}

// 将单个均衡目标转换为 JSON 对象
QJsonObject objectiveToJson(const BalanceObjectiveSetting& setting)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), setting.enabled);
    object.insert(QStringLiteral("priority"), setting.priority);
    object.insert(QStringLiteral("weight"), setting.weight);
    object.insert(QStringLiteral("tolerance"), setting.tolerance);
    return object;
}

// 判断给定宽度是否落入任一配置范围
bool containsWidth(const QVector<WidthRange>& ranges, double width)
{
    for (const WidthRange& range : ranges) {
        if (width >= range.minimum - Epsilon && width <= range.maximum + Epsilon) return true;
    }
    return false;
}

// 判断两段宽度范围是否重叠
bool rangesOverlap(const WidthRange& left, const WidthRange& right)
{
    return left.minimum <= right.maximum + Epsilon && right.minimum <= left.maximum + Epsilon;
}
}

// 创建程序内置的默认配置
OptimizerConfig OptimizerConfig::defaults()
{
    return OptimizerConfig();
}

// 从 JSON 文件加载配置并兼容早期参数格式
bool OptimizerConfig::loadFromFile(
    const QString& path,
    OptimizerConfig* config,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!config) {
        if (errorMessage) *errorMessage = QStringLiteral("参数接收对象为空。");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法打开参数文件：%1").arg(path);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("参数文件不是有效的 JSON：%1").arg(parseError.errorString());
        }
        return false;
    }

    OptimizerConfig result = defaults();
    const QJsonObject root = document.object();
    result.inputMode = readString(root, "inputMode", result.inputMode).trimmed().toLower();
    result.rawWorksheetName = readString(root, "rawWorksheetName", result.rawWorksheetName);
    result.worksheetName = readString(root, "worksheetName", result.worksheetName);
    result.plateMarkerPattern = readString(root, "plateMarkerPattern", result.plateMarkerPattern);
    result.outputSuffix = readString(root, "outputSuffix", result.outputSuffix);
    result.headerRow = readInt(root, "headerRow", result.headerRow);

    const QJsonObject columns = root.value(QStringLiteral("columns")).toObject();
    result.columns.plateId = readInt(columns, "plateId", result.columns.plateId);
    result.columns.material = readInt(columns, "material", result.columns.material);
    result.columns.plateWidth = readInt(columns, "plateWidth", result.columns.plateWidth);
    result.columns.plateThickness = readInt(columns, "plateThickness", result.columns.plateThickness);
    result.columns.plateNumber = readInt(columns, "plateNumber", result.columns.plateNumber);
    result.columns.usedLength = readInt(columns, "usedLength", result.columns.usedLength);
    result.columns.remainingLength = readInt(columns, "remainingLength", result.columns.remainingLength);
    result.columns.specification = readInt(columns, "specification", result.columns.specification);
    result.columns.bevelCount = readInt(columns, "bevelCount", result.columns.bevelCount);
    result.columns.lapWeldCount = readInt(columns, "lapWeldCount", result.columns.lapWeldCount);
    result.columns.rawSpecification = readInt(columns, "rawSpecification", result.columns.rawSpecification);
    result.columns.rawMaterial = readInt(columns, "rawMaterial", result.columns.rawMaterial);
    result.columns.rawLength = readInt(columns, "rawLength", result.columns.rawLength);
    result.columns.rawBevel = readInt(columns, "rawBevel", result.columns.rawBevel);
    result.columns.rawOnlineAuto = readInt(columns, "rawOnlineAuto", result.columns.rawOnlineAuto);
    result.columns.rawOnlineManual = readInt(columns, "rawOnlineManual", result.columns.rawOnlineManual);

    const QJsonObject nesting = root.value(QStringLiteral("nesting")).toObject();
    result.groupByMaterial = readBool(nesting, "groupByMaterial", result.groupByMaterial);
    result.groupByThickness = readBool(nesting, "groupByThickness", result.groupByThickness);
    result.groupByWidth = readBool(nesting, "groupByWidth", result.groupByWidth);
    result.defaultStockLength = readDouble(nesting, "defaultStockLength", result.defaultStockLength);
    result.kerf = readDouble(nesting, "kerf", result.kerf);
    result.endAllowance = readDouble(nesting, "endAllowance", result.endAllowance);
    result.lengthThresholds = readDoubleArray(nesting, "lengthThresholds", result.lengthThresholds);
    std::sort(result.lengthThresholds.begin(), result.lengthThresholds.end());

    if (nesting.value(QStringLiteral("stockLengthRules")).isArray()) {
        result.stockLengthRules.clear();
        const QJsonArray rules = nesting.value(QStringLiteral("stockLengthRules")).toArray();
        for (const QJsonValue& value : rules) {
            if (!value.isObject()) continue;
            const QJsonObject object = value.toObject();
            StockLengthRule rule;
            rule.enabled = readBool(object, "enabled", rule.enabled);
            rule.material = readString(object, "material", rule.material).trimmed();
            rule.thickness = readDouble(object, "thickness", rule.thickness);
            rule.width = readDouble(object, "width", rule.width);
            rule.length = readDouble(object, "length", result.defaultStockLength);
            rule.priority = readInt(object, "priority", rule.priority);
            result.stockLengthRules.append(rule);
        }
    }

    const QJsonObject routing = root.value(QStringLiteral("weldRouting")).toObject();
    result.manualWeldRanges = readRanges(
        routing, "manualRanges", "manualWidths", result.manualWeldRanges);
    result.robotWeldRanges = readRanges(
        routing, "robotRanges", "robotWidths", result.robotWeldRanges);
    result.mediumWeldRanges = readRanges(
        routing, "mediumRanges", "mediumWidths", result.mediumWeldRanges);
    result.largeWeldRanges = readRanges(
        routing, "largeRanges", "largeWidths", result.largeWeldRanges);

    const QJsonObject balance = root.value(QStringLiteral("balance")).toObject();
    readObjective(balance, QStringLiteral("bevel"), &result.bevelObjective);
    readObjective(balance, QStringLiteral("manualWeld"), &result.manualWeldObjective);
    readObjective(balance, QStringLiteral("robotWeld"), &result.robotWeldObjective);
    // 兼容第一版参数中的权重
    if (balance.contains(QStringLiteral("bevelWeight"))) {
        result.bevelObjective.weight = readDouble(balance, "bevelWeight", result.bevelObjective.weight);
    }
    if (balance.contains(QStringLiteral("lapWeldWeight"))) {
        const double legacyWeight = readDouble(balance, "lapWeldWeight", 1.0);
        result.manualWeldObjective.weight = legacyWeight;
        result.robotWeldObjective.weight = legacyWeight;
    }
    result.prefixBalanceWeight = readDouble(balance, "prefixBalanceWeight", result.prefixBalanceWeight);
    result.adjacentLoadWeight = readDouble(balance, "adjacentLoadWeight", result.adjacentLoadWeight);
    result.rollingWindowWeight = readDouble(balance, "rollingWindowWeight", result.rollingWindowWeight);
    result.rollingWindow = readInt(balance, "rollingWindow", result.rollingWindow);
    result.highLoadFactor = readDouble(balance, "highLoadFactor", result.highLoadFactor);
    result.localSearchIterations = readInt(balance, "localSearchIterations", result.localSearchIterations);

    const QJsonObject timing = root.value(QStringLiteral("timing")).toObject();
    result.downstreamMinimumLength = readDouble(
        timing, "downstreamMinimumLength", result.downstreamMinimumLength);
    result.bufferCapacity = readInt(timing, "bufferCapacity", result.bufferCapacity);
    result.sprayIncrementSeconds = readDouble(
        timing, "sprayIncrementSeconds", result.sprayIncrementSeconds);
    result.sawFeedSpeedMmPerSecond = readDouble(
        timing, "sawFeedSpeedMmPerSecond", result.sawFeedSpeedMmPerSecond);
    result.sawFirstFeedSeconds = readDouble(
        timing, "sawFirstFeedSeconds", result.sawFirstFeedSeconds);
    result.sawMaximumFeedSeconds = readDouble(
        timing, "sawMaximumFeedSeconds", result.sawMaximumFeedSeconds);
    result.sawClampSeconds = readDouble(timing, "sawClampSeconds", result.sawClampSeconds);
    result.sawReleaseSeconds = readDouble(timing, "sawReleaseSeconds", result.sawReleaseSeconds);
    result.sawUnloadSeconds = readDouble(timing, "sawUnloadSeconds", result.sawUnloadSeconds);
    result.sawPlateResetSeconds = readDouble(
        timing, "sawPlateResetSeconds", result.sawPlateResetSeconds);
    result.sawCutSecondsUnder35 = readDouble(
        timing, "sawCutSecondsUnder35", result.sawCutSecondsUnder35);
    result.sawCutSecondsUnder61 = readDouble(
        timing, "sawCutSecondsUnder61", result.sawCutSecondsUnder61);
    result.sawCutSecondsUnder90 = readDouble(
        timing, "sawCutSecondsUnder90", result.sawCutSecondsUnder90);
    result.sawCutSecondsOther = readDouble(
        timing, "sawCutSecondsOther", result.sawCutSecondsOther);
    result.bevelStationCount = readInt(timing, "bevelStationCount", result.bevelStationCount);
    result.bevelCycleSeconds = readDouble(timing, "bevelCycleSeconds", result.bevelCycleSeconds);
    result.tackCycleSeconds = readDouble(timing, "tackCycleSeconds", result.tackCycleSeconds);
    result.manualWeldStationCount = readInt(
        timing, "manualWeldStationCount", result.manualWeldStationCount);
    result.manualWeldCycleSeconds = readDouble(
        timing, "manualWeldCycleSeconds", result.manualWeldCycleSeconds);
    result.robotWeldStationCount = readInt(
        timing, "robotWeldStationCount", result.robotWeldStationCount);
    result.robotWeldFixedSeconds = readDouble(
        timing, "robotWeldFixedSeconds", result.robotWeldFixedSeconds);
    result.robotWeldSpeedMmPerSecond = readDouble(
        timing, "robotWeldSpeedMmPerSecond", result.robotWeldSpeedMmPerSecond);

    if (!result.validate(errorMessage)) return false;
    *config = result;
    return true;
}

// 校验配置并保存为 JSON 文件
bool OptimizerConfig::saveToFile(const QString& path, QString* errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    if (!validate(errorMessage)) return false;

    QJsonObject columnsObject;
    columnsObject.insert(QStringLiteral("plateId"), columns.plateId);
    columnsObject.insert(QStringLiteral("material"), columns.material);
    columnsObject.insert(QStringLiteral("plateWidth"), columns.plateWidth);
    columnsObject.insert(QStringLiteral("plateThickness"), columns.plateThickness);
    columnsObject.insert(QStringLiteral("plateNumber"), columns.plateNumber);
    columnsObject.insert(QStringLiteral("usedLength"), columns.usedLength);
    columnsObject.insert(QStringLiteral("remainingLength"), columns.remainingLength);
    columnsObject.insert(QStringLiteral("specification"), columns.specification);
    columnsObject.insert(QStringLiteral("bevelCount"), columns.bevelCount);
    columnsObject.insert(QStringLiteral("lapWeldCount"), columns.lapWeldCount);
    columnsObject.insert(QStringLiteral("rawSpecification"), columns.rawSpecification);
    columnsObject.insert(QStringLiteral("rawMaterial"), columns.rawMaterial);
    columnsObject.insert(QStringLiteral("rawLength"), columns.rawLength);
    columnsObject.insert(QStringLiteral("rawBevel"), columns.rawBevel);
    columnsObject.insert(QStringLiteral("rawOnlineAuto"), columns.rawOnlineAuto);
    columnsObject.insert(QStringLiteral("rawOnlineManual"), columns.rawOnlineManual);

    QJsonArray rulesArray;
    for (const StockLengthRule& rule : stockLengthRules) {
        QJsonObject object;
        object.insert(QStringLiteral("enabled"), rule.enabled);
        object.insert(QStringLiteral("material"), rule.material);
        object.insert(QStringLiteral("thickness"), rule.thickness);
        object.insert(QStringLiteral("width"), rule.width);
        object.insert(QStringLiteral("length"), rule.length);
        object.insert(QStringLiteral("priority"), rule.priority);
        rulesArray.append(object);
    }
    QJsonObject nestingObject;
    nestingObject.insert(QStringLiteral("groupByMaterial"), groupByMaterial);
    nestingObject.insert(QStringLiteral("groupByThickness"), groupByThickness);
    nestingObject.insert(QStringLiteral("groupByWidth"), groupByWidth);
    nestingObject.insert(QStringLiteral("defaultStockLength"), defaultStockLength);
    nestingObject.insert(QStringLiteral("kerf"), kerf);
    nestingObject.insert(QStringLiteral("endAllowance"), endAllowance);
    nestingObject.insert(QStringLiteral("lengthThresholds"), toJsonArray(lengthThresholds));
    nestingObject.insert(QStringLiteral("stockLengthRules"), rulesArray);

    QJsonObject routingObject;
    routingObject.insert(QStringLiteral("manualRanges"), rangesToJson(manualWeldRanges));
    routingObject.insert(QStringLiteral("robotRanges"), rangesToJson(robotWeldRanges));
    routingObject.insert(QStringLiteral("mediumRanges"), rangesToJson(mediumWeldRanges));
    routingObject.insert(QStringLiteral("largeRanges"), rangesToJson(largeWeldRanges));

    QJsonObject balanceObject;
    balanceObject.insert(QStringLiteral("bevel"), objectiveToJson(bevelObjective));
    balanceObject.insert(QStringLiteral("manualWeld"), objectiveToJson(manualWeldObjective));
    balanceObject.insert(QStringLiteral("robotWeld"), objectiveToJson(robotWeldObjective));
    balanceObject.insert(QStringLiteral("prefixBalanceWeight"), prefixBalanceWeight);
    balanceObject.insert(QStringLiteral("adjacentLoadWeight"), adjacentLoadWeight);
    balanceObject.insert(QStringLiteral("rollingWindowWeight"), rollingWindowWeight);
    balanceObject.insert(QStringLiteral("rollingWindow"), rollingWindow);
    balanceObject.insert(QStringLiteral("highLoadFactor"), highLoadFactor);
    balanceObject.insert(QStringLiteral("localSearchIterations"), localSearchIterations);

    QJsonObject timingObject;
    timingObject.insert(QStringLiteral("downstreamMinimumLength"), downstreamMinimumLength);
    timingObject.insert(QStringLiteral("bufferCapacity"), bufferCapacity);
    timingObject.insert(QStringLiteral("sprayIncrementSeconds"), sprayIncrementSeconds);
    timingObject.insert(QStringLiteral("sawFeedSpeedMmPerSecond"), sawFeedSpeedMmPerSecond);
    timingObject.insert(QStringLiteral("sawFirstFeedSeconds"), sawFirstFeedSeconds);
    timingObject.insert(QStringLiteral("sawMaximumFeedSeconds"), sawMaximumFeedSeconds);
    timingObject.insert(QStringLiteral("sawClampSeconds"), sawClampSeconds);
    timingObject.insert(QStringLiteral("sawReleaseSeconds"), sawReleaseSeconds);
    timingObject.insert(QStringLiteral("sawUnloadSeconds"), sawUnloadSeconds);
    timingObject.insert(QStringLiteral("sawPlateResetSeconds"), sawPlateResetSeconds);
    timingObject.insert(QStringLiteral("sawCutSecondsUnder35"), sawCutSecondsUnder35);
    timingObject.insert(QStringLiteral("sawCutSecondsUnder61"), sawCutSecondsUnder61);
    timingObject.insert(QStringLiteral("sawCutSecondsUnder90"), sawCutSecondsUnder90);
    timingObject.insert(QStringLiteral("sawCutSecondsOther"), sawCutSecondsOther);
    timingObject.insert(QStringLiteral("bevelStationCount"), bevelStationCount);
    timingObject.insert(QStringLiteral("bevelCycleSeconds"), bevelCycleSeconds);
    timingObject.insert(QStringLiteral("tackCycleSeconds"), tackCycleSeconds);
    timingObject.insert(QStringLiteral("manualWeldStationCount"), manualWeldStationCount);
    timingObject.insert(QStringLiteral("manualWeldCycleSeconds"), manualWeldCycleSeconds);
    timingObject.insert(QStringLiteral("robotWeldStationCount"), robotWeldStationCount);
    timingObject.insert(QStringLiteral("robotWeldFixedSeconds"), robotWeldFixedSeconds);
    timingObject.insert(QStringLiteral("robotWeldSpeedMmPerSecond"), robotWeldSpeedMmPerSecond);

    QJsonObject root;
    root.insert(QStringLiteral("inputMode"), inputMode);
    root.insert(QStringLiteral("rawWorksheetName"), rawWorksheetName);
    root.insert(QStringLiteral("worksheetName"), worksheetName);
    root.insert(QStringLiteral("plateMarkerPattern"), plateMarkerPattern);
    root.insert(QStringLiteral("outputSuffix"), outputSuffix);
    root.insert(QStringLiteral("headerRow"), headerRow);
    root.insert(QStringLiteral("columns"), columnsObject);
    root.insert(QStringLiteral("nesting"), nestingObject);
    root.insert(QStringLiteral("weldRouting"), routingObject);
    root.insert(QStringLiteral("balance"), balanceObject);
    root.insert(QStringLiteral("timing"), timingObject);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法保存参数文件：%1").arg(path);
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("写入参数文件失败：%1").arg(path);
        return false;
    }
    return true;
}

// 检查分组、焊接路线、均衡和时序参数是否合法
bool OptimizerConfig::validate(QString* errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    if (inputMode != QStringLiteral("auto") &&
        inputMode != QStringLiteral("raw") &&
        inputMode != QStringLiteral("detail")) {
        if (errorMessage) *errorMessage = QStringLiteral("输入模式只能是 auto、raw 或 detail。");
        return false;
    }
    if (worksheetName.trimmed().isEmpty() || plateMarkerPattern.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("工作表名称和钢板行识别规则不能为空。");
        return false;
    }
    const int columnValues[] = {
        columns.plateId, columns.material, columns.plateWidth, columns.plateThickness,
        columns.plateNumber, columns.usedLength, columns.remainingLength,
        columns.specification, columns.bevelCount, columns.lapWeldCount,
        columns.rawSpecification, columns.rawMaterial, columns.rawLength,
        columns.rawBevel, columns.rawOnlineAuto, columns.rawOnlineManual
    };
    for (int value : columnValues) {
        if (value <= 0) {
            if (errorMessage) *errorMessage = QStringLiteral("所有列号都必须大于 0。");
            return false;
        }
    }
    if (headerRow <= 0 || defaultStockLength <= 0.0 || kerf < 0.0 ||
        endAllowance < 0.0 || endAllowance >= defaultStockLength) {
        if (errorMessage) *errorMessage = QStringLiteral("表头行、钢板长度、切缝或首尾余量设置不正确。");
        return false;
    }
    for (const StockLengthRule& rule : stockLengthRules) {
        if (rule.length <= 0.0 || rule.thickness < 0.0 || rule.width < 0.0) {
            if (errorMessage) *errorMessage = QStringLiteral("钢板长度规则中存在无效数值。");
            return false;
        }
    }
    if (manualWeldObjective.enabled && manualWeldRanges.isEmpty() &&
        robotWeldRanges.isEmpty() && mediumWeldRanges.isEmpty() && largeWeldRanges.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("启用焊接均衡时，必须填写至少一种焊接宽度范围。");
        return false;
    }
    const QVector<QVector<WidthRange>> routeRanges {
        manualWeldRanges, robotWeldRanges, mediumWeldRanges, largeWeldRanges
    };
    const QStringList routeNames {
        QStringLiteral("小径人工接焊"), QStringLiteral("小径机器接焊"),
        QStringLiteral("中径机器接焊"), QStringLiteral("大径机器接焊")
    };
    for (int route = 0; route < routeRanges.size(); ++route) {
        for (const WidthRange& range : routeRanges[route]) {
            if (range.minimum <= 0.0 || range.maximum < range.minimum) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("%1宽度范围设置不正确。").arg(routeNames[route]);
                }
                return false;
            }
        }
    }
    for (int leftRoute = 0; leftRoute < routeRanges.size(); ++leftRoute) {
        for (int rightRoute = leftRoute + 1; rightRoute < routeRanges.size(); ++rightRoute) {
            for (const WidthRange& left : routeRanges[leftRoute]) {
                for (const WidthRange& right : routeRanges[rightRoute]) {
                    if (!rangesOverlap(left, right)) continue;
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("%1和%2宽度范围不能重叠。")
                            .arg(routeNames[leftRoute], routeNames[rightRoute]);
                    }
                    return false;
                }
            }
        }
    }
    const BalanceObjectiveSetting objectives[] = { bevelObjective, manualWeldObjective };
    bool anyEnabled = false;
    for (const BalanceObjectiveSetting& objective : objectives) {
        anyEnabled = anyEnabled || objective.enabled;
        if (objective.priority <= 0 || objective.weight < 0.0 || objective.tolerance < 0.0) {
            if (errorMessage) *errorMessage = QStringLiteral("均衡目标的优先级、权重或允许偏差设置不正确。");
            return false;
        }
    }
    if (!anyEnabled) {
        if (errorMessage) *errorMessage = QStringLiteral("坡口、焊接至少启用一个均衡目标。");
        return false;
    }
    if (prefixBalanceWeight < 0.0 || adjacentLoadWeight < 0.0 ||
        rollingWindowWeight < 0.0 || rollingWindow <= 0 ||
        highLoadFactor <= 0.0 || localSearchIterations < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("均衡权重、窗口或迭代次数设置不正确。");
        return false;
    }
    if (downstreamMinimumLength < 0.0 || bufferCapacity < 0 ||
        sprayIncrementSeconds < 0.0 || sawFeedSpeedMmPerSecond <= 0.0 ||
        sawFirstFeedSeconds < 0.0 || sawMaximumFeedSeconds < 0.0 ||
        sawClampSeconds < 0.0 || sawReleaseSeconds < 0.0 ||
        sawUnloadSeconds < 0.0 || sawPlateResetSeconds < 0.0 ||
        sawCutSecondsUnder35 < 0.0 || sawCutSecondsUnder61 < 0.0 ||
        sawCutSecondsUnder90 < 0.0 || sawCutSecondsOther < 0.0 ||
        bevelStationCount <= 0 || bevelCycleSeconds < 0.0 ||
        tackCycleSeconds < 0.0 || manualWeldStationCount <= 0 ||
        manualWeldCycleSeconds < 0.0 || robotWeldStationCount <= 0 ||
        robotWeldFixedSeconds < 0.0 || robotWeldSpeedMmPerSecond <= 0.0) {
        if (errorMessage) *errorMessage = QStringLiteral("时序参数设置不正确。");
        return false;
    }
    return true;
}

// 按加工资料中的固定动作和工件数量计算整张钢板喷码时间
double OptimizerConfig::sprayPlateSeconds(int workpieceCount) const
{
    if (workpieceCount <= 0) return 0.0;
    return 102.0 + sprayIncrementSeconds * workpieceCount;
}

// 按照材质和规格选择优先级最高的钢板长度
double OptimizerConfig::stockLengthFor(
    const QString& material,
    double thickness,
    double width) const
{
    const StockLengthRule* best = nullptr;
    int bestSpecificity = -1;
    for (const StockLengthRule& rule : stockLengthRules) {
        if (!rule.enabled) continue;
        if (!rule.material.isEmpty() && rule.material.compare(material, Qt::CaseInsensitive) != 0) continue;
        if (rule.thickness > 0.0 && std::abs(rule.thickness - thickness) > Epsilon) continue;
        if (rule.width > 0.0 && std::abs(rule.width - width) > Epsilon) continue;
        const int specificity = (!rule.material.isEmpty() ? 1 : 0) +
            (rule.thickness > 0.0 ? 1 : 0) + (rule.width > 0.0 ? 1 : 0);
        if (!best || rule.priority < best->priority ||
            (rule.priority == best->priority && specificity > bestSpecificity)) {
            best = &rule;
            bestSpecificity = specificity;
        }
    }
    return best ? best->length : defaultStockLength;
}

// 判断工件宽度是否属于人工接焊路线
bool OptimizerConfig::isManualWeldWidth(double width) const
{
    return containsWidth(manualWeldRanges, width);
}

// 判断工件宽度是否属于机器接焊路线
bool OptimizerConfig::isRobotWeldWidth(double width) const
{
    return containsWidth(robotWeldRanges, width) ||
        isMediumWeldWidth(width) || isLargeWeldWidth(width);
}

// 判断工件宽度是否属于中径机器接焊路线
bool OptimizerConfig::isMediumWeldWidth(double width) const
{
    return containsWidth(mediumWeldRanges, width);
}

// 判断工件宽度是否属于大径机器接焊路线
bool OptimizerConfig::isLargeWeldWidth(double width) const
{
    return containsWidth(largeWeldRanges, width);
}
