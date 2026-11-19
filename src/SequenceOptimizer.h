#pragma once

#include "OptimizerConfig.h"
#include "SteelPlateModel.h"

// 保持钢板内部工件归属不变并在全部钢板范围内优化顺序
class SequenceOptimizer
{
public:
    static QVector<int> optimize(
        const QVector<SteelPlateBlock>& plates,
        const OptimizerConfig& config); // 按流水线均衡条件优化整张钢板顺序

    static BalanceMetrics calculateMetrics(
        const QVector<SteelPlateBlock>& plates,
        const QVector<int>& order,
        const OptimizerConfig& config); // 统计指定钢板顺序的均衡指标

    static double calculateScore(
        const QVector<SteelPlateBlock>& plates,
        const QVector<int>& order,
        const OptimizerConfig& config); // 计算指定钢板顺序的综合评价分数
};
