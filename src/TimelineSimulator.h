#pragma once

#include "OptimizerConfig.h"
#include "SteelPlateModel.h"

#include <QString>
#include <QVector>

// 保存单个工件进入各工序的时间和路线
struct TimelineEntry
{
    int sequence = 0; // 保存工件排产序号
    int plateIndex = -1; // 保存工件所属钢板索引
    int workpieceIndex = -1; // 保存工件在钢板内的索引
    QString plateNumber; // 保存工件所属钢板号
    QString workpieceId; // 保存工件号
    double sprayArrival = 0.0; // 保存进入喷码工序的时间
    double sprayFinish = 0.0; // 保存完成喷码工序的时间
    double sprayDuration = 0.0; // 保存喷码加工时长
    double sprayBlockedSeconds = 0.0; // 保存喷码缓冲区堵塞时间
    double cuttingArrival = 0.0; // 保存进入切割工序的时间
    double cuttingFinish = 0.0; // 保存完成切割工序的时间
    double cuttingDuration = 0.0; // 保存切割加工时长
    double cuttingBlockedSeconds = 0.0; // 保存切割缓冲区堵塞时间
    double bevelArrival = -1.0; // 保存进入坡口工序的时间
    double bevelFinish = -1.0; // 保存完成坡口工序的时间
    double bevelDuration = 0.0; // 保存坡口加工时长
    double bevelBlockedSeconds = 0.0; // 保存坡口缓冲区堵塞时间
    double tackArrival = -1.0; // 保存进入人工组对工序的时间
    double tackFinish = -1.0; // 保存完成人工组对工序的时间
    double tackDuration = 0.0; // 保存人工组对加工时长
    double tackBlockedSeconds = 0.0; // 保存人工组对缓冲区堵塞时间
    double weldArrival = -1.0; // 保存进入接焊工序的时间
    double weldFinish = -1.0; // 保存完成接焊工序的时间
    double weldDuration = 0.0; // 保存接焊加工时长
    double weldBlockedSeconds = 0.0; // 保存接焊工位缓冲区堵塞时间
    double sortingArrival = -1.0; // 保存进入分拣工序的时间
    double sortingFinish = -1.0; // 保存完成分拣工序的时间
    double finishTime = 0.0; // 保存工件全部工序完成时间
    QString weldRoute; // 保存工件接焊路线
    QString station; // 保存工件分配的接焊工位
    double blockedSeconds = 0.0; // 保存工件累计堵塞时间，暂时统一为零
};

// 保存整批排产完成后的时序汇总结果
struct TimelineSummary
{
    int workpieceCount = 0; // 保存本批排产工件总数
    double finishTime = 0.0; // 保存本批排产最终完成时间
    double totalQueueWaitSeconds = 0.0; // 保存全部工件累计排队等待时间
    double totalBufferBlockedSeconds = 0.0; // 保存全部工件累计缓冲区堵塞时间
    int bufferFullEvents = 0; // 保存缓冲区填满事件次数
    int maximumBufferedWorkpieces = 0; // 保存缓冲区内工件数量峰值
};

// 按固定排产顺序模拟工件通过整条流水线的过程
class TimelineSimulator
{
public:
    static QVector<TimelineEntry> simulate(
        const QVector<SteelPlateBlock>& plates,
        const QVector<int>& order,
        const OptimizerConfig& config,
        TimelineSummary* summary = nullptr);
};
