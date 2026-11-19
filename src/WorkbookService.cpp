#include "WorkbookService.h"

#include "xlsxcell.h"
#include "xlsxdocument.h"
#include "xlsxworksheet.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Epsilon = 1e-7;

struct RawWorkpiece
{
    int sourceRow = 0;
    QString material;
    double width = 0.0;
    double thickness = 0.0;
    double length = 0.0;
    double bevelCount = 0.0;
    double lapWeldCount = 0.0;
    QVector<QVariant> sourceValues;
    QVector<QVariant> detail;
};

struct NestingBin
{
    QVector<int> itemIndexes;
    double used = 0.0;
};

// 从单元格中读取非负数值，空值和无效文本按零处理
double positiveNumber(const QVariant& value)
{
    bool ok = false;
    double number = value.toDouble(&ok);
    if (!ok) {
        QString text = value.toString().trimmed();
        text.remove(QLatin1Char(','));
        number = text.toDouble(&ok);
    }
    return ok && number > 0.0 ? number : 0.0;
}

// 读取指定单元格并清理首尾空白
QString readText(QXlsx::Document& document, int row, int column)
{
    return document.read(row, column).toString().trimmed();
}

// 生成稳定的数值分组键
QString numberKey(double value)
{
    return QString::number(value, 'g', 12);
}

// 生成不带多余尾零的数值文本
QString numberText(double value)
{
    if (std::abs(value - std::round(value)) <= Epsilon) {
        return QString::number(static_cast<qlonglong>(std::llround(value)));
    }
    return QString::number(value, 'f', 3).remove(QRegularExpression(QStringLiteral("0+$")))
        .remove(QRegularExpression(QStringLiteral("\\.$")));
}

// 估算文本在 Excel 中需要的列宽，中文按两个字符计算
int displayTextWidth(const QString& text)
{
    int width = 0;
    for (QChar character : text) {
        width += character.unicode() > 0x00FF ? 2 : 1;
    }
    return width;
}

// 从宽度和厚度组合文本中解析规格
bool parseSpecification(const QString& source, double* width, double* thickness)
{
    QString text = source.trimmed();
    text.replace(QChar(0x00D7), QLatin1Char('x'));
    text.replace(QChar(0xFF0A), QLatin1Char('x'));
    text.replace(QLatin1Char('*'), QLatin1Char('x'));
    const QRegularExpression expression(
        QStringLiteral("^\\s*(\\d+(?:\\.\\d+)?)\\s*[xX]\\s*(\\d+(?:\\.\\d+)?)\\s*$"));
    const QRegularExpressionMatch match = expression.match(text);
    if (!match.hasMatch()) return false;
    bool widthOk = false;
    bool thicknessOk = false;
    const double parsedWidth = match.captured(1).toDouble(&widthOk);
    const double parsedThickness = match.captured(2).toDouble(&thicknessOk);
    if (!widthOk || !thicknessOk || parsedWidth <= 0.0 || parsedThickness <= 0.0) return false;
    if (width) *width = parsedWidth;
    if (thickness) *thickness = parsedThickness;
    return true;
}

// 统计指定文字在单元格内容中的出现次数
int textCount(const QString& text, const QString& token)
{
    if (text.isEmpty() || token.isEmpty()) return 0;
    int count = 0;
    int position = 0;
    while ((position = text.indexOf(token, position, Qt::CaseSensitive)) >= 0) {
        ++count;
        position += token.size();
    }
    return count;
}

// 按照坡口文字和标记字符计算坡口数量
int bevelCount(const QString& value)
{
    const QString text = value.trimmed().toUpper();
    int count = textCount(text, QStringLiteral("坡口"));
    for (QChar character : text) {
        if (character == QLatin1Char('M') || character == QLatin1Char('H')) ++count;
    }
    return count;
}

// 将坡口标记统一转换为可导出的坡口文字
QString normalizedBevelText(const QString& value, int count)
{
    if (count <= 0) return value.trimmed();
    QString result;
    for (int index = 0; index < count; ++index) result += QStringLiteral("坡口");
    return result;
}

// 优先选择人工确认的线上装配信息
QString chooseOnlineInfo(const QString& automatic, const QString& manual)
{
    const QString manualText = manual.trimmed();
    const QString automaticText = automatic.trimmed();
    if (!manualText.isEmpty() && manualText != QStringLiteral("/")) return manualText;
    if (!automaticText.isEmpty()) return automaticText;
    return manualText;
}

// 将原始工件行转换为钢板明细使用的工件字段
QVector<QVariant> transformRawDetail(
    const QVector<QVariant>& source,
    const OptimizerConfig& config,
    double* bevel,
    double* lapWeld)
{
    auto value = [&source](int zeroBasedIndex) -> QVariant {
        return zeroBasedIndex >= 0 && zeroBasedIndex < source.size()
            ? source[zeroBasedIndex] : QVariant();
    };
    const QString rawBevel = value(config.columns.rawBevel - 1).toString();
    const int bevelValue = bevelCount(rawBevel);
    const QString online = chooseOnlineInfo(
        value(config.columns.rawOnlineAuto - 1).toString(),
        value(config.columns.rawOnlineManual - 1).toString());
    const int lapValue = textCount(online, QStringLiteral("搭焊"));
    const int buttWeld = textCount(online, QStringLiteral("对焊"));
    QVariant sleeve;
    if (buttWeld > 0) sleeve = buttWeld;
    else if (online.contains(QStringLiteral("套管"))) sleeve = 0;

    QVector<QVariant> detail;
    detail.reserve(39);
    detail << value(8) << value(0) << value(1) << value(2) << value(3)
           << value(4) << value(7) << value(8) << value(9) << value(10)
           << value(11) << value(12) << value(13) << value(14)
           << normalizedBevelText(rawBevel, bevelValue) << bevelValue
           << value(16) << value(17) << value(18) << online;
    if (online.trimmed().isEmpty() || online.trimmed() == QStringLiteral("/")) detail << QVariant();
    else detail << lapValue;
    detail << sleeve;
    for (int index = 21; index < 38; ++index) detail << value(index + 1);
    if (bevel) *bevel = bevelValue;
    if (lapWeld) *lapWeld = lapValue;
    return detail;
}

// 返回新生成钢板明细的固定表头
QVector<QString> generatedHeaders()
{
    return {
        QStringLiteral("钢板明细"), QStringLiteral("材质"), QStringLiteral("宽度(mm)"),
        QStringLiteral("厚度(mm)"), QStringLiteral("钢板编号"), QStringLiteral("工件数"),
        QStringLiteral("已用长度(mm)"), QStringLiteral("余料(mm)"), QStringLiteral("利用率"),
        QStringLiteral("工件名称"), QStringLiteral("序号"), QStringLiteral("船名"),
        QStringLiteral("一贯番号"), QStringLiteral("CODE号"), QStringLiteral("分段名"),
        QStringLiteral("分段名"), QStringLiteral("管名"), QStringLiteral("需求日期"),
        QStringLiteral("规格"), QStringLiteral("材质"), QStringLiteral("加工系列"),
        QStringLiteral("区域"), QStringLiteral("长度"), QStringLiteral("坡口"),
        QStringLiteral("坡口"), QStringLiteral("弯曲"), QStringLiteral("泵压(Mpa)"),
        QStringLiteral("表面处理"), QStringLiteral("线上装配信息(简写)"), QStringLiteral("搭焊"),
        QStringLiteral("套管"), QStringLiteral("五金1"), QStringLiteral("数量"),
        QStringLiteral("五金2"), QStringLiteral("数量"), QStringLiteral("五金3"),
        QStringLiteral("数量"), QStringLiteral("五金4"), QStringLiteral("数量"),
        QStringLiteral("船号"), QStringLiteral("管种"), QStringLiteral("管件号"),
        QStringLiteral("法兰装配转角"), QStringLiteral("重量"), QStringLiteral("内面积"),
        QStringLiteral("外面积"), QStringLiteral("分类"), QStringLiteral("流水号")
    };
}

// 计算工件加入钢板后增加的长度和切缝
double incrementalLength(const NestingBin& bin, double itemLength, double kerf)
{
    return itemLength + (bin.itemIndexes.isEmpty() ? 0.0 : kerf);
}

// 重新计算一张钢板内全部工件的占用长度
double usedLengthOf(
    const NestingBin& bin,
    const QVector<RawWorkpiece>& records,
    double kerf)
{
    double used = 0.0;
    for (int itemIndex : bin.itemIndexes) used += records[itemIndex].length;
    if (bin.itemIndexes.size() > 1) used += kerf * (bin.itemIndexes.size() - 1);
    return used;
}

// 计算工件对人工接焊路线产生的负荷
double manualWeldLoad(const RawWorkpiece& item, const OptimizerConfig& config)
{
    return config.isManualWeldWidth(item.width) ? item.lapWeldCount : 0.0;
}

// 计算工件对机器接焊路线产生的负荷
double robotWeldLoad(const RawWorkpiece& item, const OptimizerConfig& config)
{
    return config.isRobotWeldWidth(item.width) ? item.lapWeldCount : 0.0;
}

// 判断工件是否需要进入坡口或焊接后续工序
bool requiresDownstreamProcess(const RawWorkpiece& item)
{
    return item.bevelCount > Epsilon || item.lapWeldCount > Epsilon;
}

// 使用长度降序最佳适配确定钢板数量和初始装载方案
QVector<NestingBin> bestFitDecreasing(
    const QVector<RawWorkpiece>& records,
    const QVector<int>& itemIndexes,
    double stockLength,
    const OptimizerConfig& config)
{
    QVector<int> sorted = itemIndexes;
    std::sort(sorted.begin(), sorted.end(), [&records](int left, int right) {
        if (std::abs(records[left].length - records[right].length) > Epsilon) {
            return records[left].length > records[right].length;
        }
        return records[left].sourceRow < records[right].sourceRow;
    });
    const double capacity = stockLength - config.endAllowance;
    QVector<NestingBin> bins;
    for (int itemIndex : sorted) {
        int bestBin = -1;
        double bestRemaining = std::numeric_limits<double>::max();
        for (int index = 0; index < bins.size(); ++index) {
            const double after = bins[index].used +
                incrementalLength(bins[index], records[itemIndex].length, config.kerf);
            if (after <= capacity + Epsilon && capacity - after < bestRemaining) {
                bestBin = index;
                bestRemaining = capacity - after;
            }
        }
        if (bestBin < 0) {
            NestingBin bin;
            bin.itemIndexes.append(itemIndex);
            bin.used = records[itemIndex].length;
            bins.append(bin);
        } else {
            bins[bestBin].used += incrementalLength(
                bins[bestBin], records[itemIndex].length, config.kerf);
            bins[bestBin].itemIndexes.append(itemIndex);
        }
    }
    return bins;
}

// 在首次套料已经确定钢板数量后，对同材质同规格工件重新分配
// 第一阶段按长度降序逐件放置，只比较不超出钢板有效长度的候选钢板
// 贪心评分包含长度层级5、工件数量2、后续与离线工件各4、工序负荷8和利用率0.1
// 工序负荷分再乘目标权重并除以优先级，分数越小表示当前钢板越接近组内平均值
// 第二阶段在钢板之间尝试单件移动、一对一交换、一对多交换和多对多交换
// 局部搜索以坡口、人工接焊、机器接焊的归一化平方差作为主要评分
// 工件数权重2、已用长度权重0.2、长度层级权重5组成次要评分
// 综合评分等于工序评分加0.75倍次要评分，工序评分最多允许比当前结果增加2%
// 只有综合评分下降才接受候选方案，容量超限、空钢板或评分未改善时恢复原方案
QVector<NestingBin> redistributeFixedCount(
    const QVector<RawWorkpiece>& records,
    const QVector<int>& itemIndexes,
    const QVector<NestingBin>& fallback,
    double stockLength,
    const OptimizerConfig& config)
{
    // 只有一张钢板时不存在跨板均衡空间，直接保留首次套料结果
    if (fallback.size() <= 1) return fallback;

    // 长工件优先放置，减少后续因剩余空间零散而无法装入的风险
    QVector<int> sorted = itemIndexes;
    std::sort(sorted.begin(), sorted.end(), [&records](int left, int right) {
        if (std::abs(records[left].length - records[right].length) > Epsilon) {
            return records[left].length > records[right].length;
        }
        return records[left].sourceRow < records[right].sourceRow;
    });

    // 汇总整个钢板组的长度层级和工序负荷，作为每张钢板的均衡目标
    QVector<int> thresholdTotals(config.lengthThresholds.size(), 0);
    double totalBevel = 0.0;
    double totalManualWeld = 0.0;
    double totalRobotWeld = 0.0;
    int totalDownstreamItems = 0;
    for (int itemIndex : itemIndexes) {
        const RawWorkpiece& item = records[itemIndex];
        totalBevel += item.bevelCount;
        totalManualWeld += manualWeldLoad(item, config);
        totalRobotWeld += robotWeldLoad(item, config);
        if (requiresDownstreamProcess(item)) ++totalDownstreamItems;
        for (int threshold = 0; threshold < config.lengthThresholds.size(); ++threshold) {
            if (config.lengthThresholds[threshold] <= 0.0) continue;
            if (item.length > config.lengthThresholds[threshold]) {
                ++thresholdTotals[threshold];
            }
        }
    }

    // 保持首次套料确定的钢板数量不变，在有效长度范围内重新进行贪心分配
    const double capacity = stockLength - config.endAllowance;
    QVector<NestingBin> bins(fallback.size());
    bool greedySucceeded = true;
    for (int itemIndex : sorted) {
        int bestBin = -1;
        double bestScore = std::numeric_limits<double>::max();
        for (int binIndex = 0; binIndex < bins.size(); ++binIndex) {
            const double increment = incrementalLength(
                bins[binIndex], records[itemIndex].length, config.kerf);
            if (bins[binIndex].used + increment > capacity + Epsilon) continue;
            double score = 0.0;

            // 优先分散不同长度层级的工件，同时兼顾每张钢板上的工件数量
            for (int threshold = 0; threshold < config.lengthThresholds.size(); ++threshold) {
                if (config.lengthThresholds[threshold] <= 0.0) continue;
                int count = 0;
                for (int existing : bins[binIndex].itemIndexes) {
                    if (records[existing].length > config.lengthThresholds[threshold]) ++count;
                }
                if (records[itemIndex].length > config.lengthThresholds[threshold]) ++count;
                const double target = static_cast<double>(thresholdTotals[threshold]) / bins.size();
                score += 5.0 * count / std::max(1.0, target);
            }
            const double itemTarget = static_cast<double>(itemIndexes.size()) / bins.size();
            score += 2.0 * (bins[binIndex].itemIndexes.size() + 1) / std::max(1.0, itemTarget);

            // 计算工件放入当前钢板后的坡口、人工接焊、机器接焊负荷
            double binBevel = records[itemIndex].bevelCount;
            double binManualWeld = manualWeldLoad(records[itemIndex], config);
            double binRobotWeld = robotWeldLoad(records[itemIndex], config);
            int binDownstreamItems = requiresDownstreamProcess(records[itemIndex]) ? 1 : 0;
            for (int existing : bins[binIndex].itemIndexes) {
                const RawWorkpiece& item = records[existing];
                binBevel += item.bevelCount;
                binManualWeld += manualWeldLoad(item, config);
                binRobotWeld += robotWeldLoad(item, config);
                if (requiresDownstreamProcess(item)) ++binDownstreamItems;
            }
            const int totalDirectItems = itemIndexes.size() - totalDownstreamItems;
            const int binDirectItems = bins[binIndex].itemIndexes.size() + 1 -
                binDownstreamItems;
            // 将需要后续加工和直接离线的工件分散到不同钢板，平滑下游到件节奏
            if (totalDownstreamItems > 0 && totalDirectItems > 0) {
                const double downstreamTarget = static_cast<double>(totalDownstreamItems) /
                    bins.size();
                const double directTarget = static_cast<double>(totalDirectItems) / bins.size();
                score += 4.0 * binDownstreamItems / std::max(1.0, downstreamTarget);
                score += 4.0 * binDirectItems / std::max(1.0, directTarget);
            }
            // 按参数中的优先级和权重，把工序负荷加入当前钢板的放置评分
            auto addProcessBalance = [&score, &bins](
                const BalanceObjectiveSetting& setting,
                double binLoad,
                double totalLoad) {
                if (!setting.enabled || totalLoad <= Epsilon) return;
                const double target = totalLoad / bins.size();
                const double priorityFactor = 1.0 / std::max(1, setting.priority);
                score += 8.0 * setting.weight * priorityFactor *
                    binLoad / std::max(1.0, target);
            };
            addProcessBalance(config.bevelObjective, binBevel, totalBevel);
            addProcessBalance(config.manualWeldObjective, binManualWeld, totalManualWeld);
            addProcessBalance(config.manualWeldObjective, binRobotWeld, totalRobotWeld);

            score += 0.1 * (bins[binIndex].used + increment) / std::max(1.0, capacity);
            if (score < bestScore - Epsilon ||
                (std::abs(score - bestScore) <= Epsilon &&
                 (bestBin < 0 || bins[binIndex].used < bins[bestBin].used))) {
                bestBin = binIndex;
                bestScore = score;
            }
        }
        if (bestBin < 0) {
            greedySucceeded = false;
            break;
        }
        bins[bestBin].used += incrementalLength(
            bins[bestBin], records[itemIndex].length, config.kerf);
        bins[bestBin].itemIndexes.append(itemIndex);
    }
    // 每张钢板至少保留一个工件，避免为了均衡产生空钢板
    if (greedySucceeded) {
        for (const NestingBin& bin : bins) {
            if (bin.itemIndexes.isEmpty()) {
                greedySucceeded = false;
                break;
            }
        }
    }
    // 贪心重建无法满足容量或非空约束时，退回首次套料的可行方案
    if (!greedySucceeded) bins = fallback;
    for (NestingBin& bin : bins) {
        bin.used = usedLengthOf(bin, records, config.kerf);
    }

    // 使用归一化平方差衡量当前值偏离组内平均目标的程度
    auto squaredDifference = [](double value, double target) {
        const double normalized = (value - target) / std::max(1.0, target);
        return normalized * normalized;
    };
    // 次要评分，衡量工件数、已用长度和各长度层级在钢板之间的均匀程度
    auto segmentScore = [&](const QVector<NestingBin>& candidateBins) {
        double score = 0.0;
        double totalUsed = 0.0;
        for (const NestingBin& bin : candidateBins) totalUsed += bin.used;
        const double itemTarget = static_cast<double>(itemIndexes.size()) / candidateBins.size();
        const double usedTarget = totalUsed / candidateBins.size();
        for (const NestingBin& bin : candidateBins) {
            score += 2.0 * squaredDifference(bin.itemIndexes.size(), itemTarget);
            score += 0.2 * squaredDifference(bin.used, usedTarget);

            QVector<int> thresholdCounts(config.lengthThresholds.size(), 0);
            for (int existing : bin.itemIndexes) {
                const RawWorkpiece& item = records[existing];
                for (int threshold = 0; threshold < config.lengthThresholds.size(); ++threshold) {
                    if (config.lengthThresholds[threshold] > 0.0 &&
                        item.length > config.lengthThresholds[threshold]) {
                        ++thresholdCounts[threshold];
                    }
                }
            }
            for (int threshold = 0; threshold < config.lengthThresholds.size(); ++threshold) {
                if (config.lengthThresholds[threshold] <= 0.0) continue;
                const double target = static_cast<double>(thresholdTotals[threshold]) /
                    candidateBins.size();
                score += 5.0 * squaredDifference(thresholdCounts[threshold], target);
            }
        }
        return score;
    };
    // 主要评分，衡量下游工件、坡口、人工接焊和机器接焊负荷的均匀程度
    auto processScore = [&](const QVector<NestingBin>& candidateBins) {
        double score = 0.0;
        const double downstreamTarget = static_cast<double>(totalDownstreamItems) /
            candidateBins.size();
        const int totalDirectItems = itemIndexes.size() - totalDownstreamItems;
        const double directTarget = static_cast<double>(totalDirectItems) /
            candidateBins.size();
        for (const NestingBin& bin : candidateBins) {
            double bevel = 0.0;
            double manualWeld = 0.0;
            double robotWeld = 0.0;
            int downstreamItems = 0;
            for (int existing : bin.itemIndexes) {
                const RawWorkpiece& item = records[existing];
                bevel += item.bevelCount;
                manualWeld += manualWeldLoad(item, config);
                robotWeld += robotWeldLoad(item, config);
                if (requiresDownstreamProcess(item)) ++downstreamItems;
            }
            if (totalDownstreamItems > 0 && totalDownstreamItems < itemIndexes.size()) {
                score += 4.0 * squaredDifference(downstreamItems, downstreamTarget);
                const int directItems = bin.itemIndexes.size() - downstreamItems;
                score += 4.0 * squaredDifference(directItems, directTarget);
            }
            auto addProcessVariance = [&score, &candidateBins, &squaredDifference](
                const BalanceObjectiveSetting& setting,
                double binLoad,
                double totalLoad) {
                if (!setting.enabled || totalLoad <= Epsilon) return;
                const double target = totalLoad / candidateBins.size();
                const double priorityFactor = 1.0 / std::max(1, setting.priority);
                score += 8.0 * setting.weight * priorityFactor *
                    squaredDifference(binLoad, target);
            };
            addProcessVariance(config.bevelObjective, bevel, totalBevel);
            addProcessVariance(config.manualWeldObjective, manualWeld, totalManualWeld);
            addProcessVariance(config.manualWeldObjective, robotWeld, totalRobotWeld);
        }
        return score;
    };

    // 工序均衡是主要目标，段数和长度分布作为较低优先级的辅助目标
    double currentSegmentScore = segmentScore(bins);
    double currentProcessScore = processScore(bins);
    const double segmentSecondaryWeight = 0.75;
    double currentCombinedScore = currentProcessScore +
        segmentSecondaryWeight * currentSegmentScore;
    // 固定种子的伪随机序列使同一批数据的优化结果可以重复
    quint32 state = 0xA341316Cu ^ static_cast<quint32>(itemIndexes.size());
    auto nextRandom = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    const int iterations = std::max(
        5000, std::min(30000, config.localSearchIterations * 4));
    // 在两张钢板之间尝试单件移动、一对一交换和多件交换
    for (int iteration = 0; iteration < iterations; ++iteration) {
        int left = static_cast<int>(nextRandom() % static_cast<quint32>(bins.size()));
        int right = static_cast<int>(nextRandom() % static_cast<quint32>(bins.size() - 1));
        if (right >= left) ++right;
        if (nextRandom() % 2u == 0u) std::swap(left, right);
        if (bins[left].itemIndexes.isEmpty() || bins[right].itemIndexes.isEmpty()) continue;

        const NestingBin originalLeft = bins[left];
        const NestingBin originalRight = bins[right];
        const int operation = iteration % 6;
        if (operation == 0 && bins[left].itemIndexes.size() > 1) {
            const int itemPosition = static_cast<int>(
                nextRandom() % static_cast<quint32>(bins[left].itemIndexes.size()));
            bins[right].itemIndexes.append(bins[left].itemIndexes.takeAt(itemPosition));
        } else if (operation == 5 && bins[left].itemIndexes.size() > 2) {
            const int maximumExchange = std::min(6, bins[left].itemIndexes.size() - 1);
            const int exchangeCount = 2 + static_cast<int>(
                nextRandom() % static_cast<quint32>(maximumExchange - 1));
            const int rightPosition = static_cast<int>(
                nextRandom() % static_cast<quint32>(bins[right].itemIndexes.size()));
            const int rightItem = bins[right].itemIndexes.takeAt(rightPosition);
            QVector<int> leftItems;
            leftItems.reserve(exchangeCount);
            for (int exchange = 0; exchange < exchangeCount; ++exchange) {
                const int leftPosition = static_cast<int>(
                    nextRandom() % static_cast<quint32>(bins[left].itemIndexes.size()));
                leftItems.append(bins[left].itemIndexes.takeAt(leftPosition));
            }
            bins[left].itemIndexes.append(rightItem);
            for (int leftItem : leftItems) bins[right].itemIndexes.append(leftItem);
        } else if (operation >= 3) {
            const int maximumExchange = std::min(
                4, std::min(bins[left].itemIndexes.size(), bins[right].itemIndexes.size()));
            if (maximumExchange < 2) continue;
            const int exchangeCount = 2 + static_cast<int>(
                nextRandom() % static_cast<quint32>(maximumExchange - 1));
            QVector<int> leftItems;
            QVector<int> rightItems;
            leftItems.reserve(exchangeCount);
            rightItems.reserve(exchangeCount);
            for (int exchange = 0; exchange < exchangeCount; ++exchange) {
                const int leftPosition = static_cast<int>(
                    nextRandom() % static_cast<quint32>(bins[left].itemIndexes.size()));
                leftItems.append(bins[left].itemIndexes.takeAt(leftPosition));
                const int rightPosition = static_cast<int>(
                    nextRandom() % static_cast<quint32>(bins[right].itemIndexes.size()));
                rightItems.append(bins[right].itemIndexes.takeAt(rightPosition));
            }
            for (int rightItem : rightItems) bins[left].itemIndexes.append(rightItem);
            for (int leftItem : leftItems) bins[right].itemIndexes.append(leftItem);
        } else {
            const int leftPosition = static_cast<int>(
                nextRandom() % static_cast<quint32>(bins[left].itemIndexes.size()));
            const int rightPosition = static_cast<int>(
                nextRandom() % static_cast<quint32>(bins[right].itemIndexes.size()));
            std::swap(bins[left].itemIndexes[leftPosition], bins[right].itemIndexes[rightPosition]);
        }
        // 每次调整后重新核算长度，超出钢板容量的候选方案立即撤销
        bins[left].used = usedLengthOf(bins[left], records, config.kerf);
        bins[right].used = usedLengthOf(bins[right], records, config.kerf);
        if (bins[left].used > capacity + Epsilon || bins[right].used > capacity + Epsilon) {
            bins[left] = originalLeft;
            bins[right] = originalRight;
            continue;
        }
        // 工序评分不得明显变差，且综合评分改善时才接受本次调整
        const double candidateSegmentScore = segmentScore(bins);
        const double candidateProcessScore = processScore(bins);
        const double candidateCombinedScore = candidateProcessScore +
            segmentSecondaryWeight * candidateSegmentScore;
        const double processTolerance = std::max(
            1e-10, currentProcessScore * 0.02);
        const bool processNotMateriallyWorse =
            candidateProcessScore <= currentProcessScore + processTolerance;
        const bool accepted = processNotMateriallyWorse &&
            candidateCombinedScore < currentCombinedScore - 1e-10;
        if (accepted) {
            currentSegmentScore = candidateSegmentScore;
            currentProcessScore = candidateProcessScore;
            currentCombinedScore = candidateCombinedScore;
        } else {
            bins[left] = originalLeft;
            bins[right] = originalRight;
        }
    }
    return bins;
}

// 按照启用的材质和规格条件生成套料分组键
QString groupKey(const RawWorkpiece& record, const OptimizerConfig& config)
{
    QStringList values;
    values << (config.groupByMaterial ? record.material : QStringLiteral("*"));
    values << (config.groupByThickness ? numberKey(record.thickness) : QStringLiteral("*"));
    values << (config.groupByWidth ? numberKey(record.width) : QStringLiteral("*"));
    return values.join(QChar(0x001F));
}

// 判断工件管径是否属于本次套料范围
bool diameterSelected(
    double width,
    bool includeSmallDiameter,
    bool includeMediumDiameter,
    bool includeLargeDiameter)
{
    if (width < 76.0) return includeSmallDiameter;
    if (width <= 114.0) return includeMediumDiameter;
    return includeLargeDiameter;
}

// 判断工作簿中是否已经存在可识别的钢板明细行
bool hasPlateRows(
    QXlsx::Document& document,
    const OptimizerConfig& config)
{
    if (!document.selectSheet(config.worksheetName)) return false;
    QXlsx::Worksheet* worksheet = document.currentWorksheet();
    if (!worksheet) return false;
    const QXlsx::CellRange dimension = worksheet->dimension();
    if (!dimension.isValid()) return false;
    const QRegularExpression expression(config.plateMarkerPattern);
    for (int row = config.headerRow + 1; row <= dimension.lastRow(); ++row) {
        if (expression.match(readText(document, row, config.columns.plateId)).hasMatch()) return true;
    }
    return false;
}

// 读取已有钢板明细并保留原始行、格式和工件时序数据
bool loadPlateDetail(
    QXlsx::Document& document,
    const QString& inputPath,
    const OptimizerConfig& config,
    bool includeSmallDiameter,
    bool includeMediumDiameter,
    bool includeLargeDiameter,
    WorkbookData* data,
    QString* errorMessage)
{
    if (!document.selectSheet(config.worksheetName)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("找不到工作表“%1”。实际工作表：%2")
                .arg(config.worksheetName, document.sheetNames().join(QStringLiteral("，")));
        }
        return false;
    }
    QXlsx::Worksheet* worksheet = document.currentWorksheet();
    const QXlsx::CellRange dimension = worksheet ? worksheet->dimension() : QXlsx::CellRange();
    if (!worksheet || !dimension.isValid() || dimension.lastRow() <= config.headerRow) {
        if (errorMessage) *errorMessage = QStringLiteral("钢板明细中没有可处理的数据。");
        return false;
    }
    const QRegularExpression plateExpression(config.plateMarkerPattern);
    QVector<int> startRows;
    for (int row = config.headerRow + 1; row <= dimension.lastRow(); ++row) {
        const QString plateId = readText(document, row, config.columns.plateId);
        if (!plateId.isEmpty() && plateExpression.match(plateId).hasMatch()) startRows.append(row);
    }
    if (startRows.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("没有找到以 G 开头的钢板行。");
        return false;
    }

    WorkbookData result;
    result.inputPath = QFileInfo(inputPath).absoluteFilePath();
    result.worksheetName = config.worksheetName;
    result.lastColumn = dimension.lastColumn();
    result.lastRow = dimension.lastRow();
    result.generatedFromRaw = false;
    result.sourceHeaders.reserve(dimension.lastColumn());
    for (int column = 1; column <= dimension.lastColumn(); ++column) {
        QString header = readText(document, config.headerRow, column);
        if (header.isEmpty()) header = QStringLiteral("原表第%1列").arg(column);
        result.sourceHeaders.append(header);
    }
    result.plates.reserve(startRows.size());
    for (int blockIndex = 0; blockIndex < startRows.size(); ++blockIndex) {
        SteelPlateBlock block;
        block.originalIndex = blockIndex;
        block.startRow = startRows[blockIndex];
        block.endRow = blockIndex + 1 < startRows.size()
            ? startRows[blockIndex + 1] - 1 : dimension.lastRow();
        block.plateId = readText(document, block.startRow, config.columns.plateId);
        block.material = readText(document, block.startRow, config.columns.material);
        block.width = positiveNumber(document.read(block.startRow, config.columns.plateWidth));
        block.thickness = positiveNumber(document.read(block.startRow, config.columns.plateThickness));
        block.plateNumber = document.read(block.startRow, config.columns.plateNumber).toInt();
        block.usedLength = positiveNumber(document.read(block.startRow, config.columns.usedLength));
        block.stockLength = block.usedLength +
            positiveNumber(document.read(block.startRow, config.columns.remainingLength));
        block.workpieceCount = document.read(block.startRow, 6).toInt();

        for (int row = block.startRow; row <= block.endRow; ++row) {
            if (block.specification.isEmpty()) {
                block.specification = readText(document, row, config.columns.specification);
            }
            block.bevelCount += positiveNumber(document.read(row, config.columns.bevelCount));
            block.lapWeldCount += positiveNumber(document.read(row, config.columns.lapWeldCount));

            WorkpieceProcessData processData;
            processData.workpieceId = readText(document, row, 10);
            processData.length = positiveNumber(document.read(row, 23));
            processData.width = block.width;
            processData.bevelCount = positiveNumber(
                document.read(row, config.columns.bevelCount));
            processData.lapWeldCount = positiveNumber(
                document.read(row, config.columns.lapWeldCount));
            if (!processData.workpieceId.isEmpty() || processData.length > Epsilon) {
                processData.sourceValues.reserve(dimension.lastColumn());
                for (int column = 1; column <= dimension.lastColumn(); ++column) {
                    processData.sourceValues.append(document.read(row, column));
                }
                block.workpieces.append(processData);
            }

            RowSnapshot rowSnapshot;
            rowSnapshot.height = worksheet->rowHeight(row);
            rowSnapshot.hidden = worksheet->isRowHidden(row);
            rowSnapshot.rowFormat = worksheet->rowFormat(row);
            rowSnapshot.cells.resize(dimension.lastColumn());
            for (int column = 1; column <= dimension.lastColumn(); ++column) {
                QXlsx::Cell* cell = worksheet->cellAt(row, column);
                if (!cell) continue;
                CellSnapshot& snapshot = rowSnapshot.cells[column - 1];
                snapshot.exists = true;
                snapshot.value = cell->value();
                snapshot.format = cell->format();
                snapshot.hasFormula = cell->hasFormula();
                if (snapshot.hasFormula) snapshot.formula = cell->formula();
            }
            block.rows.append(rowSnapshot);
        }
        if (block.width <= 0.0) parseSpecification(block.specification, &block.width, &block.thickness);
        if (!diameterSelected(
                block.width,
                includeSmallDiameter,
                includeMediumDiameter,
                includeLargeDiameter)) {
            continue;
        }
        // 首个保留钢板仍写回原明细数据区起始行
        if (result.plates.isEmpty()) block.startRow = startRows.first();
        for (WorkpieceProcessData& workpiece : block.workpieces) workpiece.width = block.width;
        if (config.isManualWeldWidth(block.width)) block.manualWeldCount = block.lapWeldCount;
        else if (config.isRobotWeldWidth(block.width)) block.robotWeldCount = block.lapWeldCount;
        else block.unclassifiedWeldCount = block.lapWeldCount;
        if (block.specification.isEmpty() && block.width > 0.0 && block.thickness > 0.0) {
            block.specification = numberText(block.thickness) + QStringLiteral("×") + numberText(block.width);
        }
        result.validWorkpieceCount += block.workpieceCount;
        result.plates.append(block);
    }
    *data = result;
    return true;
}

// 读取原始工件表并完成分组、套料和钢板内工件均衡
bool loadRawAndNest(
    QXlsx::Document& document,
    const QString& inputPath,
    const OptimizerConfig& config,
    bool includeSmallDiameter,
    bool includeMediumDiameter,
    bool includeLargeDiameter,
    WorkbookData* data,
    QString* errorMessage)
{
    QString sheetName = config.rawWorksheetName.trimmed();
    if (sheetName.isEmpty() && !document.sheetNames().isEmpty()) sheetName = document.sheetNames().first();
    if (sheetName.isEmpty() || !document.selectSheet(sheetName)) {
        if (errorMessage) *errorMessage = QStringLiteral("找不到原始工件工作表。");
        return false;
    }
    QXlsx::Worksheet* worksheet = document.currentWorksheet();
    const QXlsx::CellRange dimension = worksheet ? worksheet->dimension() : QXlsx::CellRange();
    if (!worksheet || !dimension.isValid() || dimension.lastRow() <= config.headerRow) {
        if (errorMessage) *errorMessage = QStringLiteral("原始工件表中没有可处理的数据。");
        return false;
    }

    QVector<RawWorkpiece> records;
    QStringList warnings;
    int skipped = 0;
    const int sourceColumnCount = std::max(39, dimension.lastColumn());
    for (int row = config.headerRow + 1; row <= dimension.lastRow(); ++row) {
        const QString specText = readText(document, row, config.columns.rawSpecification);
        const QString material = readText(document, row, config.columns.rawMaterial);
        const double length = positiveNumber(document.read(row, config.columns.rawLength));
        double width = 0.0;
        double thickness = 0.0;
        if (length <= 0.0) {
            ++skipped;
            continue;
        }
        if (!parseSpecification(specText, &width, &thickness)) {
            ++skipped;
            warnings.append(QStringLiteral("第 %1 行规格无法解析：%2").arg(row).arg(specText));
            continue;
        }
        if (material.isEmpty()) {
            ++skipped;
            warnings.append(QStringLiteral("第 %1 行材质为空。").arg(row));
            continue;
        }
        // 在钢板分组和套料前排除未勾选管径
        if (!diameterSelected(
                width,
                includeSmallDiameter,
                includeMediumDiameter,
                includeLargeDiameter)) {
            continue;
        }

        QVector<QVariant> source;
        source.reserve(sourceColumnCount);
        for (int column = 1; column <= sourceColumnCount; ++column) {
            source.append(document.read(row, column));
        }
        RawWorkpiece record;
        record.sourceRow = row;
        record.material = material;
        record.width = width;
        record.thickness = thickness;
        record.length = length;
        record.sourceValues = source;
        record.detail = transformRawDetail(source, config, &record.bevelCount, &record.lapWeldCount);
        records.append(record);
    }
    if (records.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("当前勾选管径中没有找到长度、规格和材质有效的工件");
        }
        return false;
    }

    QMap<QString, QVector<int>> groups;
    for (int index = 0; index < records.size(); ++index) {
        groups[groupKey(records[index], config)].append(index);
    }

    WorkbookData result;
    result.inputPath = QFileInfo(inputPath).absoluteFilePath();
    result.worksheetName = config.worksheetName;
    result.lastColumn = 48;
    result.generatedFromRaw = true;
    result.validWorkpieceCount = records.size();
    result.skippedWorkpieceCount = skipped;
    result.warnings = warnings;
    result.generatedHeaders = generatedHeaders();
    result.sourceHeaders.reserve(dimension.lastColumn());
    for (int column = 1; column <= dimension.lastColumn(); ++column) {
        QString header = readText(document, config.headerRow, column);
        if (header.isEmpty()) header = QStringLiteral("原表第%1列").arg(column);
        result.sourceHeaders.append(header);
    }
    int groupSerial = 0;
    int plateSerial = 0;
    for (auto groupIterator = groups.constBegin(); groupIterator != groups.constEnd(); ++groupIterator) {
        const QVector<int>& itemIndexes = groupIterator.value();
        const RawWorkpiece& first = records[itemIndexes.first()];
        const QString groupMaterial = config.groupByMaterial ? first.material : QString();
        const double groupThickness = config.groupByThickness ? first.thickness : 0.0;
        const double groupWidth = config.groupByWidth ? first.width : 0.0;
        const double stockLength = config.stockLengthFor(
            groupMaterial, groupThickness, groupWidth);
        const double capacity = stockLength - config.endAllowance;
        for (int itemIndex : itemIndexes) {
            if (records[itemIndex].length > capacity + Epsilon) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                        "第 %1 行工件长度 %2 mm，超过该组可用钢板长度 %3 mm。")
                        .arg(records[itemIndex].sourceRow)
                        .arg(numberText(records[itemIndex].length))
                        .arg(numberText(capacity));
                }
                return false;
            }
        }

        QVector<NestingBin> bins = bestFitDecreasing(
            records, itemIndexes, stockLength, config);
        bins = redistributeFixedCount(records, itemIndexes, bins, stockLength, config);
        ++groupSerial;
        const QString groupId = QStringLiteral("G%1").arg(groupSerial, 3, 10, QLatin1Char('0'));
        QString groupName = groupId;
        groupName += QStringLiteral("_") +
            (groupMaterial.isEmpty() ? QStringLiteral("混合材质") : groupMaterial);
        groupName += QStringLiteral("_W") +
            (groupWidth > 0.0 ? numberText(groupWidth) : QStringLiteral("混合"));
        groupName += QStringLiteral("_T") +
            (groupThickness > 0.0 ? numberText(groupThickness) : QStringLiteral("混合"));

        for (int binIndex = 0; binIndex < bins.size(); ++binIndex) {
            NestingBin& bin = bins[binIndex];
            QVector<int> manualWeldItems;
            QVector<int> robotWeldItems;
            QVector<int> unclassifiedWeldItems;
            QVector<int> noWeldItems;
            for (int itemIndex : bin.itemIndexes) {
                const RawWorkpiece& item = records[itemIndex];
                if (item.lapWeldCount <= Epsilon) {
                    noWeldItems.append(itemIndex);
                } else if (config.isManualWeldWidth(item.width)) {
                    manualWeldItems.append(itemIndex);
                } else if (config.isRobotWeldWidth(item.width)) {
                    robotWeldItems.append(itemIndex);
                } else {
                    unclassifiedWeldItems.append(itemIndex);
                }
            }
            auto byDescendingLength = [&records](int left, int right) {
                if (std::abs(records[left].length - records[right].length) > Epsilon) {
                    return records[left].length > records[right].length;
                }
                return records[left].sourceRow < records[right].sourceRow;
            };
            std::sort(manualWeldItems.begin(), manualWeldItems.end(), byDescendingLength);
            std::sort(robotWeldItems.begin(), robotWeldItems.end(), byDescendingLength);
            std::sort(unclassifiedWeldItems.begin(), unclassifiedWeldItems.end(), byDescendingLength);
            std::sort(noWeldItems.begin(), noWeldItems.end(), byDescendingLength);
            bin.itemIndexes.clear();
            bin.itemIndexes.reserve(manualWeldItems.size() + robotWeldItems.size() +
                unclassifiedWeldItems.size() + noWeldItems.size());
            bool hasPreviousBevel = false;
            bool previousHasBevel = false;
            QVector<QVector<int>*> weldPattern = {
                &manualWeldItems, &noWeldItems,
                &robotWeldItems, &noWeldItems,
                &unclassifiedWeldItems, &noWeldItems
            };
            int weldPatternPosition = 0;
            auto hasBevelState = [&](const QVector<int>& candidates, bool hasBevel) {
                for (int itemIndex : candidates) {
                    if ((records[itemIndex].bevelCount > Epsilon) == hasBevel) return true;
                }
                return false;
            };
            auto hasAnyBevelState = [&](bool hasBevel) {
                return hasBevelState(manualWeldItems, hasBevel) ||
                    hasBevelState(robotWeldItems, hasBevel) ||
                    hasBevelState(unclassifiedWeldItems, hasBevel) ||
                    hasBevelState(noWeldItems, hasBevel);
            };
            // 先确定坡口交替目标，再在符合坡口状态的工件中匹配焊接循环
            // 焊接循环依次为人工焊、非焊接、机器焊、非焊接
            // 同条件下保留长度降序，类别不足时选择最接近的可行顺序
            while (!manualWeldItems.isEmpty() || !robotWeldItems.isEmpty() ||
                   !unclassifiedWeldItems.isEmpty() || !noWeldItems.isEmpty()) {
                const bool desiredBevel = hasPreviousBevel ? !previousHasBevel : true;
                const bool targetBevel = hasAnyBevelState(desiredBevel)
                    ? desiredBevel : !desiredBevel;
                int selectedPatternPosition = -1;
                for (int offset = 0; offset < weldPattern.size(); ++offset) {
                    const int candidatePosition =
                        (weldPatternPosition + offset) % weldPattern.size();
                    if (hasBevelState(*weldPattern[candidatePosition], targetBevel)) {
                        selectedPatternPosition = candidatePosition;
                        break;
                    }
                }
                if (selectedPatternPosition < 0) break;
                QVector<int>& candidates = *weldPattern[selectedPatternPosition];
                int selectedItemPosition = 0;
                for (int index = 0; index < candidates.size(); ++index) {
                    if ((records[candidates[index]].bevelCount > Epsilon) == targetBevel) {
                        selectedItemPosition = index;
                        break;
                    }
                }
                const int itemIndex = candidates.takeAt(selectedItemPosition);
                bin.itemIndexes.append(itemIndex);
                previousHasBevel = records[itemIndex].bevelCount > Epsilon;
                hasPreviousBevel = true;
                weldPatternPosition =
                    (selectedPatternPosition + 1) % weldPattern.size();
            }
            SteelPlateBlock block;
            block.originalIndex = plateSerial++;
            block.plateId = groupName;
            block.plateNumber = binIndex + 1;
            block.material = groupMaterial.isEmpty() ? QStringLiteral("混合") : groupMaterial;
            block.width = groupWidth;
            block.thickness = groupThickness;
            block.specification = groupWidth > 0.0 && groupThickness > 0.0
                ? numberText(groupThickness) + QStringLiteral("×") + numberText(groupWidth)
                : QStringLiteral("混合规格");
            block.stockLength = stockLength;
            block.usedLength = bin.used + config.endAllowance;
            block.workpieceCount = bin.itemIndexes.size();
            for (int itemPosition = 0; itemPosition < bin.itemIndexes.size(); ++itemPosition) {
                const RawWorkpiece& item = records[bin.itemIndexes[itemPosition]];
                WorkpieceProcessData processData;
                processData.workpieceId = item.detail.isEmpty()
                    ? QString() : item.detail.first().toString().trimmed();
                processData.length = item.length;
                processData.width = item.width;
                processData.bevelCount = item.bevelCount;
                processData.lapWeldCount = item.lapWeldCount;
                processData.sourceValues = item.sourceValues;
                block.workpieces.append(processData);
                block.bevelCount += item.bevelCount;
                block.lapWeldCount += item.lapWeldCount;
                if (config.isManualWeldWidth(item.width)) block.manualWeldCount += item.lapWeldCount;
                else if (config.isRobotWeldWidth(item.width)) block.robotWeldCount += item.lapWeldCount;
                else block.unclassifiedWeldCount += item.lapWeldCount;

                QVector<QVariant> row;
                row.reserve(48);
                if (itemPosition == 0) {
                    row << block.plateId << block.material;
                    if (block.width > 0.0) row << block.width; else row << QVariant();
                    if (block.thickness > 0.0) row << block.thickness; else row << QVariant();
                    row << block.plateNumber << block.workpieceCount << block.usedLength
                        << (block.stockLength - block.usedLength)
                        << (block.stockLength > 0.0 ? block.usedLength / block.stockLength : 0.0);
                } else {
                    for (int column = 0; column < 9; ++column) row << QVariant();
                }
                for (const QVariant& value : item.detail) row << value;
                block.generatedRows.append(row);
            }
            result.plates.append(block);
        }
    }
    result.lastRow = 1;
    for (const SteelPlateBlock& block : result.plates) result.lastRow += block.generatedRows.size() + 2;
    if (!result.plates.isEmpty()) result.lastRow -= 2;
    *data = result;
    return true;
}

// 将原始工件套料结果写入新建的钢板明细工作簿
bool exportGenerated(
    const WorkbookData& data,
    const QVector<int>& order,
    const QString& outputPath,
    QString* errorMessage)
{
    if (QFile::exists(outputPath) && !QFile::remove(outputPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法覆盖输出文件，请先关闭该 Excel。");
        return false;
    }
    QXlsx::Document document;
    const QString initialSheet = document.sheetNames().isEmpty()
        ? QString() : document.sheetNames().first();
    if (!initialSheet.isEmpty() && initialSheet != data.worksheetName) {
        document.renameSheet(initialSheet, data.worksheetName);
    }
    if (!document.selectSheet(data.worksheetName)) {
        document.addSheet(data.worksheetName);
        document.selectSheet(data.worksheetName);
    }
    QXlsx::Worksheet* worksheet = document.currentWorksheet();
    if (!worksheet) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建输出工作表。");
        return false;
    }

    QXlsx::Format bodyFormat;
    bodyFormat.setFontName(QStringLiteral("宋体"));
    bodyFormat.setFontSize(10);
    bodyFormat.setHorizontalAlignment(QXlsx::Format::AlignHCenter);
    bodyFormat.setVerticalAlignment(QXlsx::Format::AlignVCenter);
    bodyFormat.setBorderStyle(QXlsx::Format::BorderThin);
    bodyFormat.setBorderColor(QColor(QStringLiteral("#D0D0D0")));

    QXlsx::Format headerFormat = bodyFormat;
    headerFormat.setFontBold(true);
    headerFormat.setFontSize(11);
    headerFormat.setTextWrap(true);
    headerFormat.setFillPattern(QXlsx::Format::PatternSolid);
    headerFormat.setPatternForegroundColor(QColor(QStringLiteral("#D9D9D9")));
    // QXlsx 1.4.6 的前景和背景写入顺序与 OOXML 命名相反，两者同时设置可确保 Excel 显示纯色
    headerFormat.setPatternBackgroundColor(QColor(QStringLiteral("#D9D9D9")));

    QXlsx::Format plateFormat = bodyFormat;
    plateFormat.setFillPattern(QXlsx::Format::PatternSolid);
    plateFormat.setPatternForegroundColor(QColor(QStringLiteral("#E7E6E6")));
    plateFormat.setPatternBackgroundColor(QColor(QStringLiteral("#E7E6E6")));

    QXlsx::Format percentFormat = bodyFormat;
    percentFormat.setNumberFormat(QStringLiteral("0.0%"));
    QXlsx::Format platePercentFormat = plateFormat;
    platePercentFormat.setNumberFormat(QStringLiteral("0.0%"));

    for (int column = 0; column < data.generatedHeaders.size(); ++column) {
        worksheet->write(1, column + 1, data.generatedHeaders[column], headerFormat);
    }
    worksheet->setRowHeight(1, 1, 30.0);

    int targetRow = 2;
    for (int orderPosition = 0; orderPosition < order.size(); ++orderPosition) {
        const SteelPlateBlock& block = data.plates[order[orderPosition]];
        for (int sourceRow = 0; sourceRow < block.generatedRows.size(); ++sourceRow) {
            const QVector<QVariant>& values = block.generatedRows[sourceRow];
            for (int column = 0; column < data.lastColumn; ++column) {
                QXlsx::Format format = sourceRow == 0 && column < 9 ? plateFormat : bodyFormat;
                if (column == 8) format = sourceRow == 0 ? platePercentFormat : bodyFormat;
                const QVariant value = column < values.size() ? values[column] : QVariant();
                if (!value.isValid() || value.isNull()) worksheet->writeBlank(targetRow, column + 1, format);
                else worksheet->write(targetRow, column + 1, value, format);
            }
            worksheet->setRowHeight(targetRow, targetRow, 22.0);
            ++targetRow;
        }
        if (orderPosition + 1 < order.size()) {
            worksheet->setRowHeight(targetRow, targetRow + 1, 9.0);
            targetRow += 2;
        }
    }

    const double widths[] = {
        28, 14, 12, 12, 12, 10, 15, 12, 10, 18, 8, 14,
        12, 11, 14, 14, 18, 12, 13, 12, 12, 10, 10, 14,
        9, 10, 11, 12, 32, 8, 8, 26, 8, 26, 8, 26,
        8, 26, 8, 12, 12, 20, 14, 10, 10, 10, 10, 20
    };
    for (int column = 0; column < 48; ++column) {
        worksheet->setColumnWidth(column + 1, column + 1, widths[column]);
    }
    if (!document.saveAs(outputPath)) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("QXlsx 保存套料结果失败。");
        return false;
    }

    QXlsx::Document check(outputPath);
    if (!check.load() || !check.selectSheet(data.worksheetName)) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("生成后复核失败：无法重新打开输出文件。");
        return false;
    }
    int plateCount = 0;
    int workpieceCount = 0;
    QXlsx::Worksheet* checkSheet = check.currentWorksheet();
    const QXlsx::CellRange range = checkSheet->dimension();
    for (int row = 2; row <= range.lastRow(); ++row) {
        if (!check.read(row, 1).toString().trimmed().isEmpty()) ++plateCount;
        if (!check.read(row, 10).toString().trimmed().isEmpty()) ++workpieceCount;
    }
    if (plateCount != data.plates.size() || workpieceCount != data.validWorkpieceCount) {
        QFile::remove(outputPath);
        if (errorMessage) {
            *errorMessage = QStringLiteral("生成后完整性校验失败：钢板 %1/%2，工件 %3/%4。")
                .arg(plateCount).arg(data.plates.size())
                .arg(workpieceCount).arg(data.validWorkpieceCount);
        }
        return false;
    }
    return true;
}
}

// 根据输入模式自动选择读取钢板明细或执行原始工件套料
bool WorkbookService::load(
    const QString& inputPath,
    const OptimizerConfig& config,
    bool includeSmallDiameter,
    bool includeMediumDiameter,
    bool includeLargeDiameter,
    WorkbookData* data,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!data) {
        if (errorMessage) *errorMessage = QStringLiteral("工作簿数据接收对象为空。");
        return false;
    }
    if (!QFileInfo::exists(inputPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("Excel 文件不存在：%1").arg(inputPath);
        return false;
    }
    if (!config.validate(errorMessage)) return false;

    QXlsx::Document document(inputPath);
    if (!document.load()) {
        if (errorMessage) *errorMessage = QStringLiteral("QXlsx 无法打开 Excel 文件。");
        return false;
    }
    const bool detailDetected = document.sheetNames().contains(config.worksheetName) &&
        hasPlateRows(document, config);
    if (config.inputMode == QStringLiteral("detail") ||
        (config.inputMode == QStringLiteral("auto") &&
         (detailDetected || document.sheetNames().contains(config.worksheetName)))) {
        return loadPlateDetail(
            document, inputPath, config,
            includeSmallDiameter, includeMediumDiameter, includeLargeDiameter,
            data, errorMessage);
    }
    return loadRawAndNest(
        document, inputPath, config,
        includeSmallDiameter, includeMediumDiameter, includeLargeDiameter,
        data, errorMessage);
}

// 按照优化顺序导出生成数据或重排后的原钢板明细
bool WorkbookService::exportReordered(
    const WorkbookData& data,
    const QVector<int>& order,
    const QString& outputPath,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (data.plates.isEmpty() || order.size() != data.plates.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("钢板顺序与已读取的数据不一致。");
        return false;
    }
    QSet<int> uniqueIndexes;
    for (int index : order) {
        if (index < 0 || index >= data.plates.size()) {
            if (errorMessage) *errorMessage = QStringLiteral("钢板顺序中存在无效编号。");
            return false;
        }
        uniqueIndexes.insert(index);
    }
    if (uniqueIndexes.size() != data.plates.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("钢板顺序中存在重复或遗漏。");
        return false;
    }

    const QFileInfo inputInfo(data.inputPath);
    const QFileInfo outputInfo(outputPath);
    if (inputInfo.absoluteFilePath().compare(outputInfo.absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        if (errorMessage) *errorMessage = QStringLiteral("输出文件不能覆盖原始 Excel。");
        return false;
    }
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建输出目录。");
        return false;
    }
    if (data.generatedFromRaw) {
        return exportGenerated(data, order, outputPath, errorMessage);
    }

    if (QFile::exists(outputPath) && !QFile::remove(outputPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法覆盖输出文件，请先关闭该 Excel。");
        return false;
    }
    if (!QFile::copy(data.inputPath, outputPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("复制原始 Excel 失败。");
        return false;
    }
    QXlsx::Document document(outputPath);
    if (!document.load() || !document.selectSheet(data.worksheetName)) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("无法打开复制后的工作簿。");
        return false;
    }
    QXlsx::Worksheet* worksheet = document.currentWorksheet();
    if (!worksheet) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("输出工作表无效。");
        return false;
    }

    int targetRow = data.plates.first().startRow;
    for (int plateIndex : order) {
        const SteelPlateBlock& block = data.plates[plateIndex];
        for (const RowSnapshot& sourceRow : block.rows) {
            if (sourceRow.height > 0.0) worksheet->setRowHeight(targetRow, targetRow, sourceRow.height);
            if (sourceRow.rowFormat.isValid()) worksheet->setRowFormat(targetRow, targetRow, sourceRow.rowFormat);
            worksheet->setRowHidden(targetRow, targetRow, sourceRow.hidden);
            for (int column = 1; column <= data.lastColumn; ++column) {
                const CellSnapshot& snapshot = sourceRow.cells[column - 1];
                if (snapshot.exists) {
                    if (snapshot.hasFormula) {
                        worksheet->writeFormula(
                            targetRow, column, snapshot.formula, snapshot.format,
                            snapshot.value.toDouble());
                    } else if (!snapshot.value.isValid() || snapshot.value.isNull()) {
                        worksheet->writeBlank(targetRow, column, snapshot.format);
                    } else {
                        worksheet->write(targetRow, column, snapshot.value, snapshot.format);
                    }
                } else if (worksheet->cellAt(targetRow, column)) {
                    worksheet->writeBlank(targetRow, column);
                }
            }
            ++targetRow;
        }
    }
    // 清空未勾选管径原来占用的尾部数据行，导出文件只保留本次排产范围
    for (int row = targetRow; row <= data.lastRow; ++row) {
        for (int column = 1; column <= data.lastColumn; ++column) {
            if (worksheet->cellAt(row, column)) worksheet->writeBlank(row, column);
        }
    }
    if (!document.save()) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("QXlsx 保存结果失败。");
        return false;
    }
    return true;
}

// 将多个界面结果表写入同一个工作簿并复核工作表数量和尺寸
bool WorkbookService::exportResultTables(
    const QVector<WorkbookSheetData>& sheets,
    const QString& outputPath,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (sheets.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("没有可导出的结果表。");
        return false;
    }
    QSet<QString> sheetNames;
    for (const WorkbookSheetData& sheet : sheets) {
        const QString name = sheet.name.trimmed();
        if (name.isEmpty() || sheet.headers.isEmpty()) {
            if (errorMessage) *errorMessage = QStringLiteral("结果表名称或表头为空。");
            return false;
        }
        if (sheetNames.contains(name)) {
            if (errorMessage) *errorMessage = QStringLiteral("结果表名称重复：%1").arg(name);
            return false;
        }
        sheetNames.insert(name);
    }

    const QFileInfo outputInfo(outputPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建输出目录。");
        return false;
    }
    if (QFile::exists(outputPath) && !QFile::remove(outputPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法覆盖输出文件，请先关闭该 Excel。");
        return false;
    }

    QXlsx::Format bodyFormat;
    bodyFormat.setFontName(QStringLiteral("宋体"));
    bodyFormat.setFontSize(10);
    bodyFormat.setHorizontalAlignment(QXlsx::Format::AlignHCenter);
    bodyFormat.setVerticalAlignment(QXlsx::Format::AlignVCenter);
    bodyFormat.setBorderStyle(QXlsx::Format::BorderThin);
    bodyFormat.setBorderColor(QColor(QStringLiteral("#D0D0D0")));

    QXlsx::Format headerFormat = bodyFormat;
    headerFormat.setFontBold(true);
    headerFormat.setFontSize(11);
    headerFormat.setTextWrap(true);
    headerFormat.setFillPattern(QXlsx::Format::PatternSolid);
    headerFormat.setPatternForegroundColor(QColor(QStringLiteral("#D9D9D9")));
    headerFormat.setPatternBackgroundColor(QColor(QStringLiteral("#D9D9D9")));

    QXlsx::Document document;
    const QString initialSheet = document.sheetNames().isEmpty()
        ? QString() : document.sheetNames().first();
    for (int sheetIndex = 0; sheetIndex < sheets.size(); ++sheetIndex) {
        const WorkbookSheetData& source = sheets[sheetIndex];
        const QString sheetName = source.name.trimmed();
        if (sheetIndex == 0 && !initialSheet.isEmpty() && initialSheet != sheetName) {
            document.renameSheet(initialSheet, sheetName);
        }
        if (!document.sheetNames().contains(sheetName)) document.addSheet(sheetName);
        if (!document.selectSheet(sheetName)) {
            QFile::remove(outputPath);
            if (errorMessage) *errorMessage = QStringLiteral("无法创建结果表：%1").arg(sheetName);
            return false;
        }
        QXlsx::Worksheet* worksheet = document.currentWorksheet();
        if (!worksheet) {
            QFile::remove(outputPath);
            if (errorMessage) *errorMessage = QStringLiteral("结果表无效：%1").arg(sheetName);
            return false;
        }
        worksheet->setGridLinesVisible(false);

        QVector<int> columnWidths(source.headers.size(), 0);
        for (int column = 0; column < source.headers.size(); ++column) {
            worksheet->write(1, column + 1, source.headers[column], headerFormat);
            columnWidths[column] = displayTextWidth(source.headers[column]);
        }
        worksheet->setRowHeight(1, 1, 30.0);
        for (int row = 0; row < source.rows.size(); ++row) {
            const QVector<QVariant>& values = source.rows[row];
            for (int column = 0; column < source.headers.size(); ++column) {
                const QVariant value = column < values.size() ? values[column] : QVariant();
                if (!value.isValid() || value.isNull() || value.toString().isEmpty()) {
                    worksheet->writeBlank(row + 2, column + 1, bodyFormat);
                } else {
                    worksheet->write(row + 2, column + 1, value, bodyFormat);
                    columnWidths[column] = std::max(
                        columnWidths[column], displayTextWidth(value.toString()));
                }
            }
            worksheet->setRowHeight(row + 2, row + 2, 22.0);
        }
        for (int column = 0; column < columnWidths.size(); ++column) {
            const double width = std::max(10, std::min(36, columnWidths[column] + 2));
            worksheet->setColumnWidth(column + 1, column + 1, width);
        }
    }

    if (!document.saveAs(outputPath)) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("QXlsx 保存三表结果失败。");
        return false;
    }

    QXlsx::Document check(outputPath);
    if (!check.load()) {
        QFile::remove(outputPath);
        if (errorMessage) *errorMessage = QStringLiteral("导出后无法重新打开结果文件。");
        return false;
    }
    for (const WorkbookSheetData& source : sheets) {
        if (!check.selectSheet(source.name)) {
            QFile::remove(outputPath);
            if (errorMessage) *errorMessage = QStringLiteral("导出后缺少结果表：%1").arg(source.name);
            return false;
        }
        QXlsx::Worksheet* worksheet = check.currentWorksheet();
        const QXlsx::CellRange range = worksheet ? worksheet->dimension() : QXlsx::CellRange();
        if (!worksheet || range.lastRow() != source.rows.size() + 1 ||
            range.lastColumn() != source.headers.size()) {
            QFile::remove(outputPath);
            if (errorMessage) *errorMessage = QStringLiteral("结果表完整性校验失败：%1").arg(source.name);
            return false;
        }
    }
    return true;
}

// 根据输入文件名和配置后缀生成默认输出路径
QString WorkbookService::defaultOutputPath(
    const QString& inputPath,
    const QString& suffix)
{
    const QFileInfo info(inputPath);
    return info.absolutePath() + QDir::separator() + info.completeBaseName() + suffix + QStringLiteral(".xlsx");
}
