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
    double requestedArrival = 0.0;
    double arrival = 0.0;
    double start = 0.0;
    double finish = 0.0;
    int station = 0;
    int bufferedWorkpieces = 0;
    bool bufferWasFull = false;
};

class BufferedStage
{
public:
    // 初始化工位数量和允许等待的缓冲位数量
    BufferedStage(int stationCount, int bufferCapacity)
        : stationAvailable_(std::max(1, stationCount), 0.0),
          waitingStartTimes_(std::max(1, stationCount)),
          bufferCapacity_(std::max(0, bufferCapacity))
    {
    }

    // 优先选择缓冲区有空位并且预计最早完成的工位
    StageResult schedule(double requestedArrival, double duration)
    {
        StageResult result;
        result.requestedArrival = requestedArrival;
        result.arrival = requestedArrival;
        int selectedStation = -1;
        while (selectedStation < 0) {
            removeStarted(result.arrival);
            selectedStation = selectStation(result.arrival);
            if (selectedStation >= 0) break;
            result.bufferWasFull = true;
            double releasedAt = std::numeric_limits<double>::max();
            for (const QVector<double>& stationQueue : waitingStartTimes_) {
                for (double startTime : stationQueue) {
                    releasedAt = std::min(releasedAt, startTime);
                }
            }
            if (releasedAt == std::numeric_limits<double>::max()) {
                releasedAt = *std::min_element(
                    stationAvailable_.constBegin(), stationAvailable_.constEnd());
            }
            result.arrival = std::max(result.arrival, releasedAt);
        }

        result.station = selectedStation;
        nextStation_ = (selectedStation + 1) % stationAvailable_.size();
        result.start = std::max(result.arrival, stationAvailable_[result.station]);
        if (result.start > result.arrival + Epsilon) {
            waitingStartTimes_[result.station].append(result.start);
        }
        result.bufferedWorkpieces = waitingStartTimes_[result.station].size();
        result.finish = result.start + std::max(0.0, duration);
        stationAvailable_[result.station] = result.finish;
        return result;
    }

    // 下游缓冲区满时延长当前工位的占用时间
    void holdStation(int station, double releaseTime)
    {
        if (station < 0 || station >= stationAvailable_.size()) return;
        stationAvailable_[station] = std::max(stationAvailable_[station], releaseTime);
    }

private:
    // 清除已经从各自缓冲区进入工位的等待记录
    void removeStarted(double time)
    {
        for (QVector<double>& stationQueue : waitingStartTimes_) {
            for (int index = stationQueue.size() - 1; index >= 0; --index) {
                if (stationQueue[index] <= time + Epsilon) {
                    stationQueue.removeAt(index);
                }
            }
        }
    }

    // 在缓冲区未满的工位中选择预计最早开始的一个
    int selectStation(double arrival) const
    {
        int selected = -1;
        double selectedStart = std::numeric_limits<double>::max();
        for (int offset = 0; offset < stationAvailable_.size(); ++offset) {
            const int station = (nextStation_ + offset) % stationAvailable_.size();
            const bool hasBufferSpace = bufferCapacity_ > 0
                ? waitingStartTimes_[station].size() < bufferCapacity_
                : stationAvailable_[station] <= arrival + Epsilon;
            if (!hasBufferSpace) continue;
            const double expectedStart = std::max(arrival, stationAvailable_[station]);
            if (selected < 0 || expectedStart < selectedStart - Epsilon) {
                selected = station;
                selectedStart = expectedStart;
            }
        }
        return selected;
    }

    QVector<double> stationAvailable_;
    QVector<QVector<double>> waitingStartTimes_;
    int bufferCapacity_ = 0;
    int nextStation_ = 0;
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

// 按照管径周长计算机器接焊总时间
double machineWeldSeconds(double width, const OptimizerConfig& config)
{
    const double circumferenceTime = width * 3.14 /
        std::max(0.01, config.robotWeldSpeedMmPerSecond);
    return config.robotWeldFixedSeconds + circumferenceTime;
}
}

// 按照排产顺序模拟喷码、切割、坡口、搭焊和接焊时间
QVector<TimelineEntry> TimelineSimulator::simulate(
    const QVector<SteelPlateBlock>& plates,
    const QVector<int>& order,
    const OptimizerConfig& config,
    TimelineSummary* summary)
{
    TimelineSummary calculatedSummary;
    auto recordQueuePressure = [&calculatedSummary](const StageResult& result) {
        calculatedSummary.totalQueueWaitSeconds +=
            std::max(0.0, result.start - result.requestedArrival);
        calculatedSummary.totalBufferBlockedSeconds +=
            std::max(0.0, result.arrival - result.requestedArrival);
        if (result.bufferWasFull) ++calculatedSummary.bufferFullEvents;
        calculatedSummary.maximumBufferedWorkpieces = std::max(
            calculatedSummary.maximumBufferedWorkpieces, result.bufferedWorkpieces);
    };
    QVector<TimelineEntry> entries;
    BufferedStage bevelStage(config.bevelStationCount, config.bufferCapacity);
    BufferedStage tackStage(1, config.bufferCapacity);
    BufferedStage manualWeldStage(config.manualWeldStationCount, config.bufferCapacity);
    BufferedStage robotWeldStage(config.robotWeldStationCount, config.bufferCapacity);
    BufferedStage unclassifiedWeldStage(1, config.bufferCapacity);

    double sprayAvailable = 0.0;
    double sawAvailable = 0.0;
    int sequence = 0;
    for (int orderPosition = 0; orderPosition < order.size(); ++orderPosition) {
        const int plateIndex = order[orderPosition];
        if (plateIndex < 0 || plateIndex >= plates.size()) continue;
        const SteelPlateBlock& plate = plates[plateIndex];
        // 喷码和切割分别推进，下一张钢板喷码只等待上一张钢板完成喷码
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
            // 加工资料整板公式化简为102加单件增量乘工件数
            // 首件计入62秒固定前置动作，末件计入40秒回位和下料
            entry.sprayArrival = sprayAvailable;
            double sprayDuration = config.sprayIncrementSeconds;
            if (itemIndex == 0) sprayDuration += SprayPlateStartSeconds;
            if (itemIndex + 1 == plate.workpieces.size()) {
                sprayDuration += SprayPlateFinishSeconds;
            }
            const double sprayFinish = entry.sprayArrival + sprayDuration;
            entry.sprayFinish = sprayFinish;
            entry.sprayDuration = sprayDuration;
            sprayAvailable = sprayFinish;
            entry.cuttingArrival = std::max(cuttingCursor, sprayFinish);

            const double cuttingDuration =
                feedSeconds(item.length, itemIndex == 0, config) +
                config.sawClampSeconds + sawCutSeconds(item.width, config) +
                config.sawReleaseSeconds + config.sawUnloadSeconds;
            const double cuttingFinish = entry.cuttingArrival + cuttingDuration;
            entry.cuttingFinish = cuttingFinish;
            entry.cuttingDuration = cuttingDuration;
            cuttingCursor = cuttingFinish;
            entry.finishTime = cuttingFinish;

            if (item.length + Epsilon >= config.downstreamMinimumLength) {
                double downstreamReady = cuttingFinish;
                StageResult bevel;
                bool hasBevelStage = false;
                if (item.bevelCount > Epsilon) {
                    // 只有存在坡口的工件才占用坡口工位
                    bevel = bevelStage.schedule(
                        downstreamReady, config.bevelCycleSeconds);
                    hasBevelStage = true;
                    recordQueuePressure(bevel);
                    entry.cuttingBlockedSeconds = std::max(
                        0.0, bevel.arrival - downstreamReady);
                    cuttingCursor = std::max(cuttingCursor, bevel.arrival);
                    entry.bevelArrival = bevel.start;
                    entry.bevelFinish = bevel.finish;
                    entry.bevelDuration = config.bevelCycleSeconds;
                    downstreamReady = bevel.finish;
                    entry.finishTime = bevel.finish;
                }

                if (item.lapWeldCount > Epsilon) {
                    // 人工搭焊是人工接焊和机器接焊共同经过的前置工序
                    const StageResult tack = tackStage.schedule(
                        downstreamReady, config.tackCycleSeconds);
                    recordQueuePressure(tack);
                    if (hasBevelStage) {
                        entry.bevelBlockedSeconds = std::max(
                            0.0, tack.arrival - downstreamReady);
                        bevelStage.holdStation(bevel.station, tack.arrival);
                    } else {
                        entry.cuttingBlockedSeconds = std::max(
                            0.0, tack.arrival - downstreamReady);
                        cuttingCursor = std::max(cuttingCursor, tack.arrival);
                    }
                    entry.tackArrival = tack.start;
                    entry.tackFinish = tack.finish;
                    entry.tackDuration = config.tackCycleSeconds;
                    const double weldRequestedArrival = tack.finish;
                    StageResult weld;
                    if (config.isManualWeldWidth(item.width)) {
                        entry.weldRoute = QStringLiteral("人工接焊");
                        weld = manualWeldStage.schedule(
                            weldRequestedArrival, config.manualWeldCycleSeconds);
                        entry.station = QStringLiteral("人工%1").arg(weld.station + 1);
                    } else if (config.isRobotWeldWidth(item.width)) {
                        if (config.isMediumWeldWidth(item.width)) {
                            entry.weldRoute = QStringLiteral("中径机器接焊");
                        } else if (config.isLargeWeldWidth(item.width)) {
                            entry.weldRoute = QStringLiteral("大径机器接焊");
                        } else {
                            entry.weldRoute = QStringLiteral("小径机器接焊");
                        }
                        weld = robotWeldStage.schedule(
                            weldRequestedArrival, machineWeldSeconds(item.width, config));
                        entry.station = QStringLiteral("机器%1").arg(weld.station + 1);
                    } else {
                        // 宽度未命中路线时保留提示，避免把工件错误分配到某个工位
                        entry.weldRoute = QStringLiteral("未配置焊接路线");
                        weld = unclassifiedWeldStage.schedule(
                            weldRequestedArrival, config.manualWeldCycleSeconds);
                        entry.station = QStringLiteral("未配置");
                    }
                    recordQueuePressure(weld);
                    entry.tackBlockedSeconds = std::max(
                        0.0, weld.arrival - weldRequestedArrival);
                    tackStage.holdStation(tack.station, weld.arrival);
                    entry.weldArrival = weld.start;
                    entry.weldFinish = weld.finish;
                    entry.weldDuration = weld.finish - weld.start;
                    entry.finishTime = weld.finish;
                } else {
                    entry.weldRoute = QStringLiteral("直接离线");
                }
            } else {
                entry.weldRoute = QStringLiteral("短件离线");
            }

            // 分拣汇总区间从工件开始喷码起算，便于比较完整流程用时
            entry.sortingArrival = entry.sprayArrival;
            entry.sortingFinish = entry.finishTime;
            entry.blockedSeconds = entry.sprayBlockedSeconds +
                entry.cuttingBlockedSeconds + entry.bevelBlockedSeconds +
                entry.tackBlockedSeconds + entry.weldBlockedSeconds;
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
