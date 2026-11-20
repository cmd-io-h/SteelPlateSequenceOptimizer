#include "SequenceOptimizer.h"
#include "TimelineSimulator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Epsilon = 1e-7;

enum class LoadStream
{
    Bevel,
    ManualWeld,
    RobotWeld
};

enum class WeldRouteKind
{
    None,
    Manual,
    Robot
};

struct PriorityScore
{
    int priority = 1;
    double value = 0.0;
    double tolerance = 0.0;
};

// 按单个工件的实际路线计算指定工序的时间负荷
double workpieceLoadOf(
    const WorkpieceProcessData& item,
    LoadStream stream,
    const OptimizerConfig& config)
{
    if (item.length + Epsilon < config.downstreamMinimumLength) return 0.0;
    if (stream == LoadStream::Bevel && item.bevelCount > Epsilon) {
        return config.bevelCycleSeconds;
    }
    if (stream == LoadStream::ManualWeld &&
        item.lapWeldCount > Epsilon && config.isManualWeldWidth(item.width)) {
        return 2.0 * config.manualWeldCycleSeconds;
    }
    if (stream == LoadStream::RobotWeld &&
        item.lapWeldCount > Epsilon && config.isRobotWeldWidth(item.width)) {
        const double singlePass = config.robotWeldFixedSeconds + item.width * 3.14 /
            std::max(0.01, config.robotWeldSpeedMmPerSecond);
        return 2.0 * singlePass;
    }
    return 0.0;
}

// 按实际工序节拍计算钢板在指定流水线上的时间负荷
double loadOf(
    const SteelPlateBlock& plate,
    LoadStream stream,
    const OptimizerConfig& config)
{
    double load = 0.0;
    for (const WorkpieceProcessData& item : plate.workpieces) {
        load += workpieceLoadOf(item, stream, config);
    }
    return load;
}

// 返回指定流水线负荷对应的均衡参数
const BalanceObjectiveSetting& settingOf(const OptimizerConfig& config, LoadStream stream)
{
    switch (stream) {
    case LoadStream::Bevel: return config.bevelObjective;
    case LoadStream::ManualWeld: return config.manualWeldObjective;
    case LoadStream::RobotWeld: return config.robotWeldObjective;
    }
    return config.bevelObjective;
}

// 定义参与排序评分的流水线负荷类型
QVector<LoadStream> streams()
{
    return { LoadStream::Bevel, LoadStream::ManualWeld, LoadStream::RobotWeld };
}

// 按钢板上的主要接焊负荷判断人工或机器路线
WeldRouteKind weldRouteOf(
    const SteelPlateBlock& plate,
    const OptimizerConfig& config)
{
    double manualLoad = 0.0;
    double robotLoad = 0.0;
    for (const WorkpieceProcessData& item : plate.workpieces) {
        manualLoad += workpieceLoadOf(item, LoadStream::ManualWeld, config);
        robotLoad += workpieceLoadOf(item, LoadStream::RobotWeld, config);
    }
    if (manualLoad <= Epsilon && robotLoad <= Epsilon) return WeldRouteKind::None;
    return manualLoad >= robotLoad
        ? WeldRouteKind::Manual : WeldRouteKind::Robot;
}

// 按本批人工和机器钢板数量生成等比例穿插的目标路线
QVector<WeldRouteKind> targetWeldRouteSequence(
    const QVector<SteelPlateBlock>& plates,
    const OptimizerConfig& config)
{
    int manualCount = 0;
    int robotCount = 0;
    int noneCount = 0;
    for (const SteelPlateBlock& plate : plates) {
        const WeldRouteKind route = weldRouteOf(plate, config);
        if (route == WeldRouteKind::Manual) ++manualCount;
        if (route == WeldRouteKind::Robot) ++robotCount;
        if (route == WeldRouteKind::None) ++noneCount;
    }

    QVector<WeldRouteKind> weldSequence;
    weldSequence.reserve(manualCount + robotCount);
    const WeldRouteKind reliefRoute = robotCount >= manualCount
        ? WeldRouteKind::Robot : WeldRouteKind::Manual;
    if (manualCount == 0 || robotCount == 0) {
        const WeldRouteKind onlyRoute = manualCount > 0
            ? WeldRouteKind::Manual : WeldRouteKind::Robot;
        for (int index = 0; index < manualCount + robotCount; ++index) {
            weldSequence.append(onlyRoute);
        }
    } else {
        const bool robotDominant = robotCount >= manualCount;
        const int dominantCount = robotDominant ? robotCount : manualCount;
        const int minorCount = robotDominant ? manualCount : robotCount;
        const WeldRouteKind dominantRoute = robotDominant
            ? WeldRouteKind::Robot : WeldRouteKind::Manual;
        const WeldRouteKind minorRoute = robotDominant
            ? WeldRouteKind::Manual : WeldRouteKind::Robot;
        const int baseGroupSize = dominantCount / minorCount;
        const int extraDominant = dominantCount % minorCount;
        int remainderAccumulator = 0;

        for (int group = 0; group < minorCount; ++group) {
            int groupSize = baseGroupSize;
            remainderAccumulator += extraDominant;
            if (remainderAccumulator >= minorCount) {
                ++groupSize;
                remainderAccumulator -= minorCount;
            }
            for (int index = 0; index < groupSize; ++index) {
                weldSequence.append(dominantRoute);
            }
            weldSequence.append(minorRoute);
        }
    }

    QVector<WeldRouteKind> result = weldSequence;
    result.reserve(plates.size());
    const int finalCount = weldSequence.size() + noneCount;
    for (int noneIndex = 0; noneIndex < noneCount; ++noneIndex) {
        if (result.isEmpty()) {
            result.append(WeldRouteKind::None);
            continue;
        }
        const double desiredPosition = static_cast<double>(noneIndex + 1) *
            finalCount / (noneCount + 1.0);
        int bestInsertPosition = -1;
        int bestRunLength = -1;
        bool bestUsesReliefRoute = false;
        double bestDistance = std::numeric_limits<double>::max();
        int position = 0;
        while (position < result.size()) {
            if (result[position] == WeldRouteKind::None) {
                ++position;
                continue;
            }
            const int runStart = position;
            const WeldRouteKind route = result[position];
            while (position < result.size() && result[position] == route) ++position;
            const int runLength = position - runStart;
            const int insertPosition = runStart + (runLength + 1) / 2;
            const bool usesReliefRoute = route == reliefRoute;
            const double distance = std::abs(insertPosition - desiredPosition);
            if ((usesReliefRoute && !bestUsesReliefRoute) ||
                (usesReliefRoute == bestUsesReliefRoute &&
                 runLength > bestRunLength) ||
                (usesReliefRoute == bestUsesReliefRoute &&
                 runLength == bestRunLength && distance < bestDistance)) {
                bestInsertPosition = insertPosition;
                bestRunLength = runLength;
                bestUsesReliefRoute = usesReliefRoute;
                bestDistance = distance;
            }
        }
        if (bestInsertPosition < 0) bestInsertPosition = result.size();
        result.insert(bestInsertPosition, WeldRouteKind::None);
    }
    return result;
}

// 比较当前焊接钢板顺序与本批等比例目标路线的偏差
double weldRouteProportionPenalty(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    const QVector<WeldRouteKind> target = targetWeldRouteSequence(plates, config);
    int routePosition = 0;
    int mismatchCount = 0;
    for (int plateIndex : order) {
        const WeldRouteKind route = weldRouteOf(plates[plateIndex], config);
        if (routePosition >= target.size() || route != target[routePosition]) {
            ++mismatchCount;
        }
        ++routePosition;
    }
    return routePosition > 0
        ? static_cast<double>(mismatchCount) / routePosition : 0.0;
}

// 连续同路线越长惩罚越高，促使人工和机器接焊钢板交替进入流水线
double weldRouteRunPenalty(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    WeldRouteKind previous = WeldRouteKind::None;
    int runLength = 0;
    double penalty = 0.0;
    auto closeRun = [&]() {
        if (runLength > 1) {
            const double repeated = runLength - 1.0;
            penalty += repeated * repeated;
        }
        runLength = 0;
        previous = WeldRouteKind::None;
    };
    for (int plateIndex : order) {
        const WeldRouteKind route = weldRouteOf(plates[plateIndex], config);
        if (route == WeldRouteKind::None) {
            closeRun();
        } else if (route == previous) {
            ++runLength;
        } else {
            closeRun();
            previous = route;
            runLength = 1;
        }
    }
    closeRun();
    return penalty;
}

// 统计全部钢板在指定负荷上的总量
double totalLoad(
    const QVector<SteelPlateBlock>& plates,
    LoadStream stream,
    const OptimizerConfig& config)
{
    double total = 0.0;
    for (const SteelPlateBlock& plate : plates) total += loadOf(plate, stream, config);
    return total;
}

// 计算累计负荷相对目标值的归一化误差
double normalizedError(double actual, double target, double total)
{
    return std::abs(actual - target) / std::max(1.0, total);
}

// 用实际工序时间评估给定顺序产生的队列和缓冲区压力
double queuePressureScore(
    const TimelineSummary& summary,
    const OptimizerConfig& config)
{
    if (summary.workpieceCount <= 0) return 0.0;
    const double referenceTime = std::max(1.0, summary.finishTime);
    const double blockedRatio = summary.totalBufferBlockedSeconds / referenceTime;
    const double fullEventRatio = static_cast<double>(summary.bufferFullEvents) /
        summary.workpieceCount;
    const double bufferRatio = config.bufferCapacity > 0
        ? static_cast<double>(summary.maximumBufferedWorkpieces) / config.bufferCapacity
        : static_cast<double>(summary.maximumBufferedWorkpieces);
    // 只有缓冲区满后无法释放工件的时间计入最高优先级堵塞评分
    const double blockageTier = summary.bufferFullEvents > 0 ||
        summary.totalBufferBlockedSeconds > Epsilon
        ? 1000000.0 + 100.0 * blockedRatio + 20.0 * fullEventRatio : 0.0;
    return blockageTier + 0.001 * bufferRatio;
}

// 将指定工序的累计等待时间按当前时序长度归一化
double stageWaitScore(double waitSeconds, const TimelineSummary& summary)
{
    return waitSeconds / std::max(1.0, summary.finishTime);
}

// 计算相邻两张高负荷钢板造成的拥挤惩罚
double adjacentPenalty(double left, double right, double average, double highFactor)
{
    if (average <= 0.0) return 0.0;
    double penalty = (left / average) * (right / average);
    if (left > average * highFactor && right > average * highFactor) penalty += 1.0;
    return penalty;
}

// 将同一优先级的评分合并到一个层级
void addPriorityScore(
    QVector<PriorityScore>* scores,
    int priority,
    double value,
    double tolerance)
{
    for (PriorityScore& score : *scores) {
        if (score.priority == priority) {
            score.value += value;
            score.tolerance = std::max(score.tolerance, tolerance);
            return;
        }
    }
    PriorityScore score;
    score.priority = priority;
    score.value = value;
    score.tolerance = tolerance;
    scores->append(score);
}

// 将人工和机器接焊交替评分放入焊接优先级
void addWeldRouteScore(
    QVector<PriorityScore>* scores,
    double routePenalty,
    const OptimizerConfig& config)
{
    int priority = std::numeric_limits<int>::max();
    double weight = 0.0;
    double tolerance = 0.0;
    int enabledCount = 0;
    const BalanceObjectiveSetting settings[] = {
        config.manualWeldObjective, config.robotWeldObjective
    };
    for (const BalanceObjectiveSetting& setting : settings) {
        if (!setting.enabled) continue;
        priority = priority == std::numeric_limits<int>::max()
            ? setting.priority : std::max(priority, setting.priority);
        weight += setting.weight;
        tolerance = std::max(tolerance, setting.tolerance);
        ++enabledCount;
    }
    if (enabledCount == 0) return;
    const double averageWeight = weight / enabledCount;
    addPriorityScore(scores, priority + 2,
        averageWeight * config.adjacentLoadWeight * routePenalty, tolerance);
}

// 按照优先级从高到低排列评分层级
void sortScores(QVector<PriorityScore>* scores)
{
    std::sort(scores->begin(), scores->end(), [](const PriorityScore& left, const PriorityScore& right) {
        return left.priority < right.priority;
    });
}

// 逐优先级比较两组评分，负数表示左侧更优，正数表示右侧更优
int compareScores(const QVector<PriorityScore>& left, const QVector<PriorityScore>& right)
{
    const int size = std::min(left.size(), right.size());
    for (int index = 0; index < size; ++index) {
        const double tolerance = std::max(left[index].tolerance, right[index].tolerance);
        const double difference = left[index].value - right[index].value;
        if (difference < -tolerance) return -1;
        if (difference > tolerance) return 1;
    }
    double leftTotal = 0.0;
    double rightTotal = 0.0;
    for (const PriorityScore& score : left) leftTotal += score.value;
    for (const PriorityScore& score : right) rightTotal += score.value;
    if (leftTotal < rightTotal - 1e-12) return -1;
    if (leftTotal > rightTotal + 1e-12) return 1;
    return 0;
}

// 计算一种负荷在完整钢板顺序上的均衡评分
double streamFullScore(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    LoadStream stream,
    const OptimizerConfig& config)
{
    int fullWorkpieceCount = 0;
    for (const SteelPlateBlock& plate : plates) {
        fullWorkpieceCount += plate.workpieces.size();
    }
    if (fullWorkpieceCount <= 1) return 0.0;
    const double total = totalLoad(plates, stream, config);
    if (total <= 0.0) return 0.0;
    const double average = total / fullWorkpieceCount;

    QVector<double> workpieceLoads;
    for (int plateIndex : order) {
        if (plateIndex < 0 || plateIndex >= plates.size()) continue;
        for (const WorkpieceProcessData& item : plates[plateIndex].workpieces) {
            workpieceLoads.append(workpieceLoadOf(item, stream, config));
        }
    }
    const int count = workpieceLoads.size();
    if (count <= 1) return 0.0;

    double prefix = 0.0;
    double adjacent = 0.0;
    double rolling = 0.0;
    double cumulative = 0.0;
    const int window = std::max(1, std::min(config.rollingWindow, count));
    for (int position = 0; position < count; ++position) {
        cumulative += workpieceLoads[position];
        const double progress = static_cast<double>(position + 1) /
            fullWorkpieceCount;
        prefix += normalizedError(cumulative, total * progress, total);
        if (position > 0) {
            adjacent += adjacentPenalty(
                workpieceLoads[position - 1], workpieceLoads[position],
                average, config.highLoadFactor);
        }
        if (position + 1 >= window) {
            double windowLoad = 0.0;
            for (int i = position + 1 - window; i <= position; ++i) {
                windowLoad += workpieceLoads[i];
            }
            const double expected = average * window;
            rolling += std::max(0.0, windowLoad - expected) / std::max(1.0, total);
        }
    }
    prefix /= count;
    adjacent /= std::max(1, count - 1);
    rolling /= std::max(1, count - window + 1);
    return config.prefixBalanceWeight * prefix +
        config.adjacentLoadWeight * adjacent +
        config.rollingWindowWeight * rolling;
}

// 计算全部启用目标在完整顺序上的分层评分
QVector<PriorityScore> fullScores(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    QVector<PriorityScore> result;
    TimelineSummary summary;
    TimelineSimulator::simulate(plates, order, config, &summary);
    addPriorityScore(&result, 0, queuePressureScore(summary, config), 1e-9);
    const int weldPriority = std::min(
        config.manualWeldObjective.priority, config.robotWeldObjective.priority);
    addPriorityScore(&result, weldPriority,
        weldRouteProportionPenalty(plates, order, config), 1e-9);
    for (LoadStream stream : streams()) {
        const BalanceObjectiveSetting& setting = settingOf(config, stream);
        if (!setting.enabled) continue;
        double stageScore = streamFullScore(plates, order, stream, config);
        if (stream == LoadStream::Bevel) {
            stageScore += stageWaitScore(summary.bevelQueueWaitSeconds, summary);
        } else if (stream == LoadStream::ManualWeld) {
            stageScore += stageWaitScore(
                summary.manualOuterQueueWaitSeconds +
                summary.manualInnerQueueWaitSeconds, summary);
        } else if (stream == LoadStream::RobotWeld) {
            stageScore += stageWaitScore(
                summary.robotOuterQueueWaitSeconds +
                summary.robotInnerQueueWaitSeconds, summary);
        }
        const int scorePriority = stream == LoadStream::Bevel
            ? setting.priority : std::max(setting.priority, weldPriority + 1);
        addPriorityScore(&result, scorePriority,
            setting.weight * stageScore, setting.tolerance);
    }
    const double weldTolerance = std::max(
        config.manualWeldObjective.tolerance, config.robotWeldObjective.tolerance);
    addPriorityScore(&result, weldPriority + 1,
        stageWaitScore(summary.tackQueueWaitSeconds, summary), weldTolerance);
    addWeldRouteScore(&result,
        weldRouteRunPenalty(plates, order, config), config);
    sortScores(&result);
    return result;
}

// 统计指定负荷连续高于阈值的最长钢板数量
int longestHighRun(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    double threshold,
    LoadStream stream,
    const OptimizerConfig& config)
{
    int longest = 0;
    int current = 0;
    for (int index : order) {
        if (loadOf(plates[index], stream, config) > threshold) {
            ++current;
            longest = std::max(longest, current);
        } else {
            current = 0;
        }
    }
    return longest;
}

// 统计连续窗口内指定负荷的最大累计值
double rollingPeak(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    int window,
    LoadStream stream,
    const OptimizerConfig& config)
{
    if (order.isEmpty()) return 0.0;
    window = std::max(1, std::min(window, order.size()));
    double sum = 0.0;
    double peak = 0.0;
    for (int i = 0; i < order.size(); ++i) {
        sum += loadOf(plates[order[i]], stream, config);
        if (i >= window) sum -= loadOf(plates[order[i - window]], stream, config);
        if (i + 1 >= window) peak = std::max(peak, sum);
    }
    return peak;
}
}

// 汇总所有优先级评分并返回单一目标值
double SequenceOptimizer::calculateScore(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    double total = 0.0;
    const QVector<PriorityScore> scores = fullScores(plates, order, config);
    for (const PriorityScore& score : scores) total += score.value;
    return total;
}

namespace
{
// 在全部钢板范围内生成均衡顺序并执行局部交换优化
QVector<int> optimizePlateSequence(
    const QVector<SteelPlateBlock>& plates,
    const OptimizerConfig& config)
{
    const int count = plates.size();
    QVector<int> original;
    original.reserve(count);
    for (int i = 0; i < count; ++i) original.append(i);
    if (count <= 1) return original;

    QVector<int> remaining = original;
    QVector<int> order;
    order.reserve(count);
    for (int position = 0; position < count; ++position) {
        struct RankedCandidate
        {
            int remainingPosition = 0;
            QVector<PriorityScore> scores;
        };
        QVector<RankedCandidate> ranked;
        ranked.reserve(remaining.size());
        for (int candidatePosition = 0;
             candidatePosition < remaining.size(); ++candidatePosition) {
            RankedCandidate candidate;
            candidate.remainingPosition = candidatePosition;
            QVector<int> trialOrder = order;
            trialOrder.append(remaining[candidatePosition]);
            // 每张候选钢板都把内部工件逐个接入完整时序后再比较各工序压力
            candidate.scores = fullScores(plates, trialOrder, config);
            ranked.append(candidate);
        }
        std::sort(ranked.begin(), ranked.end(), [&](
            const RankedCandidate& left, const RankedCandidate& right) {
            const int comparison = compareScores(left.scores, right.scores);
            if (comparison != 0) return comparison < 0;
            return plates[remaining[left.remainingPosition]].originalIndex <
                plates[remaining[right.remainingPosition]].originalIndex;
        });
        int bestRemaining = ranked.first().remainingPosition;
        order.append(remaining.takeAt(bestRemaining));
    }

    QVector<PriorityScore> currentScore = fullScores(plates, order, config);
    quint32 state = 0x9E3779B9u;
    auto nextRandom = [&state]() -> quint32 {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (int iteration = 0; iteration < config.localSearchIterations; ++iteration) {
        int left = static_cast<int>(nextRandom() % static_cast<quint32>(count));
        int right = iteration % 3 == 0
            ? (left + 1 + iteration % std::min(12, count - 1)) % count
            : static_cast<int>(nextRandom() % static_cast<quint32>(count));
        if (left == right) continue;
        std::swap(order[left], order[right]);
        const QVector<PriorityScore> candidate = fullScores(plates, order, config);
        if (compareScores(candidate, currentScore) < 0) {
            currentScore = candidate;
        } else {
            std::swap(order[left], order[right]);
        }
    }

    if (compareScores(currentScore, fullScores(plates, original, config)) > 0) {
        return original;
    }
    return order;
}
}

// 在钢板内部工件归属不变的前提下允许不同钢板组交叉排产
QVector<int> SequenceOptimizer::optimize(
    const QVector<SteelPlateBlock>& plates,
    const OptimizerConfig& config)
{
    return optimizePlateSequence(plates, config);
}

// 统计顺序调整前后的负荷分布和连续拥挤指标
BalanceMetrics SequenceOptimizer::calculateMetrics(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    BalanceMetrics metrics;
    const int count = order.size();
    if (count == 0) return metrics;

    for (int index : order) {
        metrics.totalBevel += loadOf(plates[index], LoadStream::Bevel, config);
        metrics.totalManualWeld += loadOf(plates[index], LoadStream::ManualWeld, config);
        metrics.totalRobotWeld += loadOf(plates[index], LoadStream::RobotWeld, config);
        metrics.totalUnclassifiedWeld += plates[index].unclassifiedWeldCount;
    }
    const int frontCount = count / 2;
    for (int position = 0; position < count; ++position) {
        const SteelPlateBlock& plate = plates[order[position]];
        if (position < frontCount) {
            metrics.frontBevel += loadOf(plate, LoadStream::Bevel, config);
            metrics.frontManualWeld += loadOf(plate, LoadStream::ManualWeld, config);
            metrics.frontRobotWeld += loadOf(plate, LoadStream::RobotWeld, config);
        } else {
            metrics.backBevel += loadOf(plate, LoadStream::Bevel, config);
            metrics.backManualWeld += loadOf(plate, LoadStream::ManualWeld, config);
            metrics.backRobotWeld += loadOf(plate, LoadStream::RobotWeld, config);
        }
    }

    const double averageBevel = metrics.totalBevel / count;
    const double averageManual = metrics.totalManualWeld / count;
    const double averageRobot = metrics.totalRobotWeld / count;
    const double bevelThreshold = averageBevel * config.highLoadFactor;
    const double manualThreshold = averageManual * config.highLoadFactor;
    const double robotThreshold = averageRobot * config.highLoadFactor;
    for (int position = 1; position < count; ++position) {
        const SteelPlateBlock& previous = plates[order[position - 1]];
        const SteelPlateBlock& current = plates[order[position]];
        if (loadOf(previous, LoadStream::Bevel, config) > bevelThreshold &&
            loadOf(current, LoadStream::Bevel, config) > bevelThreshold) {
            ++metrics.adjacentHighBevelPairs;
        }
        if (loadOf(previous, LoadStream::ManualWeld, config) > manualThreshold &&
            loadOf(current, LoadStream::ManualWeld, config) > manualThreshold) {
            ++metrics.adjacentHighManualWeldPairs;
        }
        if (loadOf(previous, LoadStream::RobotWeld, config) > robotThreshold &&
            loadOf(current, LoadStream::RobotWeld, config) > robotThreshold) {
            ++metrics.adjacentHighRobotWeldPairs;
        }
    }
    metrics.longestHighBevelRun = longestHighRun(
        plates, order, bevelThreshold, LoadStream::Bevel, config);
    metrics.longestHighManualWeldRun = longestHighRun(
        plates, order, manualThreshold, LoadStream::ManualWeld, config);
    metrics.longestHighRobotWeldRun = longestHighRun(
        plates, order, robotThreshold, LoadStream::RobotWeld, config);
    metrics.rollingPeakBevel = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::Bevel, config);
    metrics.rollingPeakManualWeld = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::ManualWeld, config);
    metrics.rollingPeakRobotWeld = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::RobotWeld, config);
    metrics.objective = calculateScore(plates, order, config);
    return metrics;
}
