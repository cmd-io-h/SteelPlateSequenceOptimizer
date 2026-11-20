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
    double tackArrival = -1.0; // 保存进入人工组队工序的时间
    double tackFinish = -1.0; // 保存完成人工组队工序的时间
    double tackDuration = 0.0; // 保存人工组队加工时长
    double tackBlockedSeconds = 0.0; // 保存人工组队缓冲区堵塞时间
    double outerWeldArrival = -1.0; // 保存进入外圈接焊工序的时间
    double outerWeldFinish = -1.0; // 保存完成外圈接焊工序的时间
    double outerWeldDuration = 0.0; // 保存外圈接焊加工时长
    double weldBlockedSeconds = 0.0; // 保存外圈完成后因中间缓冲区满产生的堵塞时间
    double innerWeldArrival = -1.0; // 保存进入内圈接焊工序的时间
    double innerWeldFinish = -1.0; // 保存完成内圈接焊工序的时间
    double innerWeldDuration = 0.0; // 保存内圈接焊加工时长
    double sortingArrival = -1.0; // 保存进入分拣工序的时间
    double sortingFinish = -1.0; // 保存完成分拣工序的时间
    double finishTime = 0.0; // 保存工件全部工序完成时间
    QString weldRoute; // 保存工件接焊路线
    QString station; // 保存工件采用的接焊路线
    double blockedSeconds = 0.0; // 保存工件累计堵塞时间
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
    double bevelQueueWaitSeconds = 0.0; // 保存坡口工序累计排队等待时间
    double tackQueueWaitSeconds = 0.0; // 保存组队工序累计排队等待时间
    double manualOuterQueueWaitSeconds = 0.0; // 保存人工外圈接焊累计排队等待时间
    double manualInnerQueueWaitSeconds = 0.0; // 保存人工内圈接焊累计排队等待时间
    double robotOuterQueueWaitSeconds = 0.0; // 保存机器外圈接焊累计排队等待时间
    double robotInnerQueueWaitSeconds = 0.0; // 保存机器内圈接焊累计排队等待时间
    double manualBufferBlockedSeconds = 0.0; // 保存人工焊中间缓冲区累计堵塞时间
    double robotBufferBlockedSeconds = 0.0; // 保存机器焊中间缓冲区累计堵塞时间
    int manualBufferFullEvents = 0; // 保存人工焊中间缓冲区填满次数
    int robotBufferFullEvents = 0; // 保存机器焊中间缓冲区填满次数
    int manualMaximumBufferedWorkpieces = 0; // 保存人工焊中间缓冲区工件数量峰值
    int robotMaximumBufferedWorkpieces = 0; // 保存机器焊中间缓冲区工件数量峰值
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
