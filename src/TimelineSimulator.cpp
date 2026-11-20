#include "TimelineSimulator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Epsilon = 1e-7;
constexpr double SprayPlateStartSeconds = 62.0;
constexpr double SprayPlateFinishSeconds = 40.0;

struct StageResult
{
    double requestedArrival = 0.0; // 保存工件到达工序的原始时间
    double start = 0.0; // 保存工件实际开始加工的时间
    double finish = 0.0; // 保存工件完成加工的时间
    int station = 0; // 保存工件使用的并行工位索引
};

// 在并行工位中选择最早可用工位，普通排队不计为堵塞
class ParallelStage
{
public:
    explicit ParallelStage(int stationCount)
        : stationAvailable_(std::max(1, stationCount), 0.0)
    {
    }

    StageResult schedule(double requestedArrival, double duration)
    {
        StageResult result;
        result.requestedArrival = requestedArrival;
        result.station = 0;
        for (int station = 1; station < stationAvailable_.size(); ++station) {
            if (stationAvailable_[station] < stationAvailable_[result.station] - Epsilon) {
                result.station = station;
            }
        }
        result.start = std::max(requestedArrival, stationAvailable_[result.station]);
        result.finish = result.start + std::max(0.0, duration);
        stationAvailable_[result.station] = result.finish;
        return result;
    }

private:
    QVector<double> stationAvailable_; // 保存各并行工位最早可用时间
};

struct SerialWeldResult
{
    double requestedArrival = 0.0; // 保存工件到达外圈工位的原始时间
    double outerStart = 0.0; // 保存外圈实际开始时间
    double outerFinish = 0.0; // 保存外圈焊接完成时间
    double outerRelease = 0.0; // 保存外圈工位释放工件的时间
    double innerStart = 0.0; // 保存内圈实际开始时间
    double innerFinish = 0.0; // 保存内圈焊接完成时间
    double blockedSeconds = 0.0; // 保存缓冲区满后外圈工位堵塞时间
    int bufferedWorkpieces = 0; // 保存本次排入后的缓冲区工件数
    bool bufferWasFull = false; // 标记本次是否发生缓冲区满
};

// 模拟外圈接焊、中间缓冲区和内圈接焊三段串行关系
class SerialWeldRoute
{
public:
    explicit SerialWeldRoute(int bufferCapacity)
        : bufferCapacity_(std::max(0, bufferCapacity))
    {
    }

    SerialWeldResult schedule(double requestedArrival, double duration)
    {
        SerialWeldResult result;
        result.requestedArrival = requestedArrival;
        result.outerStart = std::max(requestedArrival, outerAvailable_);
        result.outerFinish = result.outerStart + std::max(0.0, duration);
        removeReleased(result.outerFinish);

        result.outerRelease = result.outerFinish;
        if (bufferCapacity_ == 0) {
            result.outerRelease = std::max(result.outerRelease, innerAvailable_);
        } else if (bufferedUntil_.size() >= bufferCapacity_) {
            result.outerRelease = std::max(result.outerRelease, earliestRelease());
        }
        if (result.outerRelease > result.outerFinish + Epsilon) {
            result.bufferWasFull = true;
            result.blockedSeconds = result.outerRelease - result.outerFinish;
        }
        removeReleased(result.outerRelease);

        result.innerStart = std::max(result.outerRelease, innerAvailable_);
        result.innerFinish = result.innerStart + std::max(0.0, duration);
        if (result.innerStart > result.outerRelease + Epsilon) {
            bufferedUntil_.append(result.innerStart);
        }
        result.bufferedWorkpieces = bufferedUntil_.size();
        outerAvailable_ = result.outerRelease;
        innerAvailable_ = result.innerFinish;
        return result;
    }

private:
    void removeReleased(double time)
    {
        for (int index = bufferedUntil_.size() - 1; index >= 0; --index) {
            if (bufferedUntil_[index] <= time + Epsilon) bufferedUntil_.removeAt(index);
        }
    }

    double earliestRelease() const
    {
        if (bufferedUntil_.isEmpty()) return innerAvailable_;
        return *std::min_element(bufferedUntil_.constBegin(), bufferedUntil_.constEnd());
    }

    double outerAvailable_ = 0.0; // 保存外圈工位最早可用时间
    double innerAvailable_ = 0.0; // 保存内圈工位最早可用时间
    QVector<double> bufferedUntil_; // 保存缓冲工件进入内圈工位的时间
    int bufferCapacity_ = 0; // 保存中间缓冲区容量
};

// 按照工件宽度选择圆锯切割节拍
double sawCutSeconds(double width, const OptimizerConfig& config)
{
    if (width < 35.0) return config.sawCutSecondsUnder35;
    if (width < 61.0) return config.sawCutSecondsUnder61;
    if (width < 90.0) return config.sawCutSecondsUnder90;
    return config.sawCutSecondsOther;
}

// 计算第一次送料或后续定长送料时间
double feedSeconds(double length, bool first, const OptimizerConfig& config)
{
    if (first) return config.sawFirstFeedSeconds;
    const double calculated = length / std::max(1.0, config.sawFeedSpeedMmPerSecond);
    return std::min(config.sawMaximumFeedSeconds, std::max(0.0, calculated));
}

// 按实际管径和表格公式计算单道机器接焊时间
double machineWeldSeconds(double width, const OptimizerConfig& config)
{
    const double circumferenceTime = width * 3.14 /
        std::max(0.01, config.robotWeldSpeedMmPerSecond);
    return config.robotWeldFixedSeconds + circumferenceTime;
}
}

// 按固定钢板顺序模拟喷码、切割、坡口、组队和内外圈接焊
QVector<TimelineEntry> TimelineSimulator::simulate(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config,
    TimelineSummary* summary)
{
    TimelineSummary calculatedSummary;
    auto recordStageWait = [&calculatedSummary](
        const StageResult& result,
        double* stageQueueWait) {
        const double wait = std::max(0.0, result.start - result.requestedArrival);
        calculatedSummary.totalQueueWaitSeconds += wait;
        if (stageQueueWait) *stageQueueWait += wait;
    };
    auto recordWeldPressure = [&calculatedSummary](
        const SerialWeldResult& result,
        bool manualRoute) {
        const double outerWait = std::max(
            0.0, result.outerStart - result.requestedArrival);
        const double innerWait = std::max(
            0.0, result.innerStart - result.outerRelease);
        calculatedSummary.totalQueueWaitSeconds += outerWait + innerWait;
        calculatedSummary.totalBufferBlockedSeconds += result.blockedSeconds;
        if (result.bufferWasFull) ++calculatedSummary.bufferFullEvents;
        calculatedSummary.maximumBufferedWorkpieces = std::max(
            calculatedSummary.maximumBufferedWorkpieces, result.bufferedWorkpieces);
        if (manualRoute) {
            calculatedSummary.manualOuterQueueWaitSeconds += outerWait;
            calculatedSummary.manualInnerQueueWaitSeconds += innerWait;
            calculatedSummary.manualBufferBlockedSeconds += result.blockedSeconds;
            if (result.bufferWasFull) ++calculatedSummary.manualBufferFullEvents;
            calculatedSummary.manualMaximumBufferedWorkpieces = std::max(
                calculatedSummary.manualMaximumBufferedWorkpieces,
                result.bufferedWorkpieces);
        } else {
            calculatedSummary.robotOuterQueueWaitSeconds += outerWait;
            calculatedSummary.robotInnerQueueWaitSeconds += innerWait;
            calculatedSummary.robotBufferBlockedSeconds += result.blockedSeconds;
            if (result.bufferWasFull) ++calculatedSummary.robotBufferFullEvents;
            calculatedSummary.robotMaximumBufferedWorkpieces = std::max(
                calculatedSummary.robotMaximumBufferedWorkpieces,
                result.bufferedWorkpieces);
        }
    };

    QVector<TimelineEntry> entries;
    ParallelStage bevelStage(config.bevelStationCount);
    ParallelStage tackStage(1);
    SerialWeldRoute manualWeldRoute(config.bufferCapacity);
    SerialWeldRoute robotWeldRoute(config.bufferCapacity);

    double sprayAvailable = 0.0;
    double sawAvailable = 0.0;
    int sequence = 0;
    for (int orderPosition = 0; orderPosition < order.size(); ++orderPosition) {
        const int plateIndex = order[orderPosition];
        if (plateIndex < 0 || plateIndex >= plates.size()) continue;
        const SteelPlateBlock& plate = plates[plateIndex];
        // 下一张钢板喷码只等待上一张喷码完成，切割等待上一张钢板全部切完并回位
        double cuttingCursor = sawAvailable;

        for (int itemIndex = 0; itemIndex < plate.workpieces.size(); ++itemIndex) {
            const WorkpieceProcessData& item = plate.workpieces[itemIndex];
            TimelineEntry entry;
            entry.sequence = ++sequence;
            entry.plateIndex = plateIndex;
            entry.workpieceIndex = itemIndex;
            entry.plateNumber = plate.plateId + QStringLiteral("-") +
                QString::number(plate.plateNumber);
            entry.workpieceId = item.workpieceId.trimmed().isEmpty()
                ? QStringLiteral("工件%1").arg(itemIndex + 1) : item.workpieceId;

            // 整板喷码公式化简为102加18乘工件数，首件和末件分别计入固定动作
            entry.sprayArrival = sprayAvailable;
            double sprayDuration = config.sprayIncrementSeconds;
            if (itemIndex == 0) sprayDuration += SprayPlateStartSeconds;
            if (itemIndex + 1 == plate.workpieces.size()) sprayDuration += SprayPlateFinishSeconds;
            entry.sprayFinish = entry.sprayArrival + sprayDuration;
            entry.sprayDuration = sprayDuration;
            sprayAvailable = entry.sprayFinish;

            entry.cuttingArrival = std::max(cuttingCursor, entry.sprayFinish);
            entry.cuttingDuration =
                feedSeconds(item.length, itemIndex == 0, config) +
                config.sawClampSeconds + sawCutSeconds(item.width, config) +
                config.sawReleaseSeconds + config.sawUnloadSeconds;
            entry.cuttingFinish = entry.cuttingArrival + entry.cuttingDuration;
            cuttingCursor = entry.cuttingFinish;
            entry.finishTime = entry.cuttingFinish;

            if (item.length + Epsilon >= config.downstreamMinimumLength) {
                double downstreamReady = entry.cuttingFinish;
                if (item.bevelCount > Epsilon) {
                    const StageResult bevel = bevelStage.schedule(
                        downstreamReady, config.bevelCycleSeconds);
                    recordStageWait(
                        bevel, &calculatedSummary.bevelQueueWaitSeconds);
                    entry.bevelArrival = bevel.start;
                    entry.bevelFinish = bevel.finish;
                    entry.bevelDuration = config.bevelCycleSeconds;
                    downstreamReady = bevel.finish;
                    entry.finishTime = bevel.finish;
                }

                if (item.lapWeldCount > Epsilon) {
                    // 组队数量大于零时统一执行一次160秒组队
                    const StageResult tack = tackStage.schedule(
                        downstreamReady, config.tackCycleSeconds);
                    recordStageWait(
                        tack, &calculatedSummary.tackQueueWaitSeconds);
                    entry.tackArrival = tack.start;
                    entry.tackFinish = tack.finish;
                    entry.tackDuration = config.tackCycleSeconds;
                    entry.finishTime = tack.finish;

                    SerialWeldResult weld;
                    double weldDuration = 0.0;
                    bool hasWeldRoute = false;
                    bool manualRoute = false;
                    if (config.isManualWeldWidth(item.width)) {
                        entry.weldRoute = QStringLiteral("人工接焊");
                        entry.station = QStringLiteral("人工");
                        weldDuration = config.manualWeldCycleSeconds;
                        weld = manualWeldRoute.schedule(tack.finish, weldDuration);
                        hasWeldRoute = true;
                        manualRoute = true;
                    } else if (config.isRobotWeldWidth(item.width)) {
                        entry.weldRoute = QStringLiteral("机器接焊");
                        entry.station = QStringLiteral("机器");
                        weldDuration = machineWeldSeconds(item.width, config);
                        weld = robotWeldRoute.schedule(tack.finish, weldDuration);
                        hasWeldRoute = true;
                    } else {
                        entry.weldRoute = QStringLiteral("仅组队");
                    }

                    if (hasWeldRoute) {
                        recordWeldPressure(weld, manualRoute);
                        entry.outerWeldArrival = weld.outerStart;
                        entry.outerWeldFinish = weld.outerFinish;
                        entry.outerWeldDuration = weldDuration;
                        entry.weldBlockedSeconds = weld.blockedSeconds;
                        entry.innerWeldArrival = weld.innerStart;
                        entry.innerWeldFinish = weld.innerFinish;
                        entry.innerWeldDuration = weldDuration;
                        entry.finishTime = weld.innerFinish;
                    }
                } else {
                    entry.weldRoute = QStringLiteral("直接离线");
                }
            } else {
                entry.weldRoute = QStringLiteral("短件离线");
            }

            // 分拣汇总区间从开始喷码起算到全部工序完成
            entry.sortingArrival = entry.sprayArrival;
            entry.sortingFinish = entry.finishTime;
            entry.blockedSeconds = entry.weldBlockedSeconds;
            calculatedSummary.finishTime = std::max(
                calculatedSummary.finishTime, entry.finishTime);
            entries.append(entry);
        }
        sawAvailable = cuttingCursor + config.sawPlateResetSeconds;
    }
    calculatedSummary.workpieceCount = entries.size();
    if (summary) *summary = calculatedSummary;
    return entries;
}
