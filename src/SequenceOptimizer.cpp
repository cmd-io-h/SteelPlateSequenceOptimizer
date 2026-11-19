#include "SequenceOptimizer.h"
#include "TimelineSimulator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
enum class LoadStream
{
    Bevel,
    ManualWeld,
    RobotWeld
};

struct PriorityScore
{
    int priority = 1;
    double value = 0.0;
    double tolerance = 0.0;
};

// 读取钢板在指定流水线负荷上的数量
double loadOf(const SteelPlateBlock& plate, LoadStream stream)
{
    switch (stream) {
    case LoadStream::Bevel: return plate.bevelCount;
    case LoadStream::ManualWeld: return plate.manualWeldCount;
    case LoadStream::RobotWeld: return plate.robotWeldCount;
    }
    return 0.0;
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

// 统计全部钢板在指定负荷上的总量
double totalLoad(const QVector<SteelPlateBlock>& plates, LoadStream stream)
{
    double total = 0.0;
    for (const SteelPlateBlock& plate : plates) total += loadOf(plate, stream);
    return total;
}

// 计算累计负荷相对目标值的归一化误差
double normalizedError(double actual, double target, double total)
{
    return std::abs(actual - target) / std::max(1.0, total);
}

// 用实际工序时间评估给定顺序产生的队列和缓冲区压力
double queuePressureScore(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config)
{
    if (order.isEmpty()) return 0.0;
    TimelineSummary summary;
    TimelineSimulator::simulate(plates, order, config, &summary);
    if (summary.workpieceCount <= 0) return 0.0;
    double sprayReferenceTime = 0.0;
    for (int plateIndex : order) {
        if (plateIndex < 0 || plateIndex >= plates.size()) continue;
        sprayReferenceTime += config.sprayPlateSeconds(
            plates[plateIndex].workpieces.size());
    }
    const double referenceTime = std::max(1.0, sprayReferenceTime);
    const double waitRatio = summary.totalQueueWaitSeconds / referenceTime;
    const double blockedRatio = summary.totalBufferBlockedSeconds / referenceTime;
    const double fullEventRatio = static_cast<double>(summary.bufferFullEvents) /
        summary.workpieceCount;
    const double bufferRatio = config.bufferCapacity > 0
        ? static_cast<double>(summary.maximumBufferedWorkpieces) / config.bufferCapacity
        : static_cast<double>(summary.maximumBufferedWorkpieces);
    return waitRatio + 5.0 * blockedRatio +
        20.0 * fullEventRatio + 0.25 * bufferRatio;
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
    const int count = order.size();
    if (count <= 1) return 0.0;
    const double total = totalLoad(plates, stream);
    if (total <= 0.0) return 0.0;
    const double average = total / count;

    double prefix = 0.0;
    double adjacent = 0.0;
    double rolling = 0.0;
    double cumulative = 0.0;
    const int window = std::max(1, std::min(config.rollingWindow, count));
    for (int position = 0; position < count; ++position) {
        cumulative += loadOf(plates[order[position]], stream);
        const double progress = static_cast<double>(position + 1) / count;
        prefix += normalizedError(cumulative, total * progress, total);
        if (position > 0) {
            adjacent += adjacentPenalty(
                loadOf(plates[order[position - 1]], stream),
                loadOf(plates[order[position]], stream), average, config.highLoadFactor);
        }
        if (position + 1 >= window) {
            double windowLoad = 0.0;
            for (int i = position + 1 - window; i <= position; ++i) {
                windowLoad += loadOf(plates[order[i]], stream);
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
    addPriorityScore(&result, 0,
        queuePressureScore(plates, order, config), 1e-9);
    for (LoadStream stream : streams()) {
        const BalanceObjectiveSetting& setting = settingOf(config, stream);
        if (!setting.enabled) continue;
        addPriorityScore(&result, setting.priority,
            setting.weight * streamFullScore(plates, order, stream, config),
            setting.tolerance);
    }
    sortScores(&result);
    return result;
}

// 计算候选钢板放到当前末尾后的增量评分
QVector<PriorityScore> candidateScores(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& currentOrder,
    int candidateIndex,
    const OptimizerConfig& config)
{
    QVector<PriorityScore> result;
    const int fullCount = plates.size();
    const int position = currentOrder.size();
    for (LoadStream stream : streams()) {
        const BalanceObjectiveSetting& setting = settingOf(config, stream);
        if (!setting.enabled) continue;
        const double total = totalLoad(plates, stream);
        const double average = fullCount > 0 ? total / fullCount : 0.0;
        double cumulative = 0.0;
        for (int index : currentOrder) cumulative += loadOf(plates[index], stream);
        const double candidateLoad = loadOf(plates[candidateIndex], stream);
        const double progress = static_cast<double>(position + 1) / std::max(1, fullCount);
        double value = config.prefixBalanceWeight *
            normalizedError(cumulative + candidateLoad, total * progress, total);

        if (!currentOrder.isEmpty()) {
            value += config.adjacentLoadWeight * adjacentPenalty(
                loadOf(plates[currentOrder.last()], stream), candidateLoad,
                average, config.highLoadFactor);
        }

        const int window = std::max(1, config.rollingWindow);
        if (position + 1 >= window && total > 0.0) {
            double windowLoad = candidateLoad;
            const int first = std::max(0, position + 1 - window);
            for (int i = first; i < position; ++i) {
                windowLoad += loadOf(plates[currentOrder[i]], stream);
            }
            value += config.rollingWindowWeight *
                std::max(0.0, windowLoad - average * window) / std::max(1.0, total);
        }
        addPriorityScore(&result, setting.priority, setting.weight * value, setting.tolerance);
    }
    sortScores(&result);
    return result;
}

// 统计指定负荷连续高于阈值的最长钢板数量
int longestHighRun(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    double threshold,
    LoadStream stream)
{
    int longest = 0;
    int current = 0;
    for (int index : order) {
        if (loadOf(plates[index], stream) > threshold) {
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
    LoadStream stream)
{
    if (order.isEmpty()) return 0.0;
    window = std::max(1, std::min(window, order.size()));
    double sum = 0.0;
    double peak = 0.0;
    for (int i = 0; i < order.size(); ++i) {
        sum += loadOf(plates[order[i]], stream);
        if (i >= window) sum -= loadOf(plates[order[i - window]], stream);
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
            candidate.scores = candidateScores(
                plates, order, remaining[candidatePosition], config);
            ranked.append(candidate);
        }
        std::sort(ranked.begin(), ranked.end(), [&](
            const RankedCandidate& left, const RankedCandidate& right) {
            const int comparison = compareScores(left.scores, right.scores);
            if (comparison != 0) return comparison < 0;
            return plates[remaining[left.remainingPosition]].originalIndex <
                plates[remaining[right.remainingPosition]].originalIndex;
        });

        // 先按负荷筛选候选钢板，再用实际时序和缓冲区压力选择最终位置
        const int shortlistSize = std::min(12, ranked.size());
        int bestRemaining = ranked.first().remainingPosition;
        QVector<PriorityScore> best = ranked.first().scores;
        QVector<int> trialOrder = order;
        trialOrder.append(remaining[bestRemaining]);
        addPriorityScore(&best, 0,
            queuePressureScore(plates, trialOrder, config), 1e-9);
        sortScores(&best);
        for (int candidateIndex = 1; candidateIndex < shortlistSize; ++candidateIndex) {
            const int remainingPosition = ranked[candidateIndex].remainingPosition;
            QVector<PriorityScore> candidate = ranked[candidateIndex].scores;
            trialOrder = order;
            trialOrder.append(remaining[remainingPosition]);
            addPriorityScore(&candidate, 0,
                queuePressureScore(plates, trialOrder, config), 1e-9);
            sortScores(&candidate);
            const int comparison = compareScores(candidate, best);
            if (comparison < 0 ||
                (comparison == 0 &&
                 plates[remaining[remainingPosition]].originalIndex <
                    plates[remaining[bestRemaining]].originalIndex)) {
                bestRemaining = remainingPosition;
                best = candidate;
            }
        }
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

    if (compareScores(currentScore, fullScores(plates, original, config)) > 0) return original;
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
        metrics.totalBevel += plates[index].bevelCount;
        metrics.totalManualWeld += plates[index].manualWeldCount;
        metrics.totalRobotWeld += plates[index].robotWeldCount;
        metrics.totalUnclassifiedWeld += plates[index].unclassifiedWeldCount;
    }
    const int frontCount = count / 2;
    for (int position = 0; position < count; ++position) {
        const SteelPlateBlock& plate = plates[order[position]];
        if (position < frontCount) {
            metrics.frontBevel += plate.bevelCount;
            metrics.frontManualWeld += plate.manualWeldCount;
            metrics.frontRobotWeld += plate.robotWeldCount;
        } else {
            metrics.backBevel += plate.bevelCount;
            metrics.backManualWeld += plate.manualWeldCount;
            metrics.backRobotWeld += plate.robotWeldCount;
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
        if (previous.bevelCount > bevelThreshold && current.bevelCount > bevelThreshold) {
            ++metrics.adjacentHighBevelPairs;
        }
        if (previous.manualWeldCount > manualThreshold && current.manualWeldCount > manualThreshold) {
            ++metrics.adjacentHighManualWeldPairs;
        }
        if (previous.robotWeldCount > robotThreshold && current.robotWeldCount > robotThreshold) {
            ++metrics.adjacentHighRobotWeldPairs;
        }
    }
    metrics.longestHighBevelRun = longestHighRun(
        plates, order, bevelThreshold, LoadStream::Bevel);
    metrics.longestHighManualWeldRun = longestHighRun(
        plates, order, manualThreshold, LoadStream::ManualWeld);
    metrics.longestHighRobotWeldRun = longestHighRun(
        plates, order, robotThreshold, LoadStream::RobotWeld);
    metrics.rollingPeakBevel = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::Bevel);
    metrics.rollingPeakManualWeld = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::ManualWeld);
    metrics.rollingPeakRobotWeld = rollingPeak(
        plates, order, config.rollingWindow, LoadStream::RobotWeld);
    metrics.objective = calculateScore(plates, order, config);
    return metrics;
}
