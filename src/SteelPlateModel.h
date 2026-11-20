/**
 * @file SteelPlateModel.h
 * @brief 定义导入数据、钢板工件、均衡指标和工作簿快照模型
 * @author cmd-io-h
 */

#pragma once

#include <QVariant>
#include <QVector>
#include <QString>
#include <QStringList>

#include "xlsxcellformula.h"
#include "xlsxformat.h"

// 保存单元格值、公式和格式用于无损导出
struct CellSnapshot
{
    bool exists = false; // 标记原单元格是否存在
    bool hasFormula = false; // 标记原单元格是否包含公式
    QVariant value; // 保存原单元格的值
    QXlsx::Format format; // 保存原单元格的格式
    QXlsx::CellFormula formula; // 保存原单元格的公式
};

// 保存原工作表一整行的内容和行级样式
struct RowSnapshot
{
    QVector<CellSnapshot> cells; // 保存本行全部单元格
    double height = 0.0; // 保存原始行高
    bool hidden = false; // 保存原始隐藏状态
    QXlsx::Format rowFormat; // 保存原始行级格式
};

// 保存单个工件的工序负荷和原表整行数据
struct WorkpieceProcessData
{
    QString workpieceId; // 保存工件号
    double length = 0.0; // 保存工件长度
    double width = 0.0; // 保存工件管径宽度
    double bevelCount = 0.0; // 保存工件坡口负荷
    double lapWeldCount = 0.0; // 保存工件搭焊、对焊和套管合计的组队数量
    QVector<QVariant> sourceValues; // 保存工件原表整行字段
};

// 保存一张钢板及其内部全部工件的数据
struct SteelPlateBlock
{
    int originalIndex = 0; // 保存钢板导入时的原始顺序
    int startRow = 0; // 保存钢板在原工作表中的起始行
    int endRow = 0; // 保存钢板在原工作表中的结束行
    QString plateId; // 保存钢板唯一编号
    int plateNumber = 0; // 保存钢板组内编号
    QString material; // 保存钢板材质
    QString specification; // 保存钢板宽度X厚度规格
    double width = 0.0; // 保存钢板宽度
    double thickness = 0.0; // 保存钢板厚度
    double stockLength = 0.0; // 保存钢板原料长度
    double usedLength = 0.0; // 保存工件占用总长度
    int workpieceCount = 0; // 保存钢板内工件数量
    double bevelCount = 0.0; // 保存钢板坡口总负荷
    double lapWeldCount = 0.0; // 保存钢板组队总数量
    double manualWeldCount = 0.0; // 保存钢板人工接焊总负荷
    double robotWeldCount = 0.0; // 保存钢板机器接焊总负荷
    double unclassifiedWeldCount = 0.0; // 保存未识别焊接路线的负荷
    QVector<WorkpieceProcessData> workpieces; // 保存钢板内全部工件
    QVector<RowSnapshot> rows; // 保存钢板对应的原表行快照
    QVector<QVector<QVariant>> generatedRows; // 保存套料后生成的明细行
};

// 保存一次导入产生的工作簿级数据
struct WorkbookData
{
    QString inputPath; // 保存输入工作簿路径
    QString worksheetName; // 保存导入工作表名称
    int lastColumn = 0; // 保存原工作表最后一列
    int lastRow = 0; // 保存原工作表最后一行
    bool generatedFromRaw = false; // 标记钢板是否由原始工件自动套料生成
    int validWorkpieceCount = 0; // 保存有效工件数量
    int skippedWorkpieceCount = 0; // 保存跳过的无效工件数量
    QStringList warnings; // 保存导入和套料提示信息
    QVector<QString> generatedHeaders; // 保存自动生成表格的表头
    QVector<QString> sourceHeaders; // 保存原工作表表头
    QVector<SteelPlateBlock> plates; // 保存本次导入的全部钢板
};

// 保存钢板顺序的均衡效果指标
struct BalanceMetrics
{
    double objective = 0.0; // 保存当前顺序的综合评价分数
    double totalBevel = 0.0; // 保存全部钢板坡口总负荷
    double totalManualWeld = 0.0; // 保存全部钢板人工接焊总负荷
    double totalRobotWeld = 0.0; // 保存全部钢板机器接焊总负荷
    double totalUnclassifiedWeld = 0.0; // 保存全部未识别焊接路线负荷
    double frontBevel = 0.0; // 保存前半段坡口负荷
    double backBevel = 0.0; // 保存后半段坡口负荷
    double frontManualWeld = 0.0; // 保存前半段人工接焊负荷
    double backManualWeld = 0.0; // 保存后半段人工接焊负荷
    double frontRobotWeld = 0.0; // 保存前半段机器接焊负荷
    double backRobotWeld = 0.0; // 保存后半段机器接焊负荷
    int adjacentHighBevelPairs = 0; // 保存相邻高坡口负荷钢板对数
    int adjacentHighManualWeldPairs = 0; // 保存相邻高人工接焊负荷钢板对数
    int adjacentHighRobotWeldPairs = 0; // 保存相邻高机器接焊负荷钢板对数
    int longestHighBevelRun = 0; // 保存高坡口负荷最长连续张数
    int longestHighManualWeldRun = 0; // 保存高人工接焊负荷最长连续张数
    int longestHighRobotWeldRun = 0; // 保存高机器接焊负荷最长连续张数
    double rollingPeakBevel = 0.0; // 保存连续窗口内坡口负荷峰值
    double rollingPeakManualWeld = 0.0; // 保存连续窗口内人工接焊负荷峰值
    double rollingPeakRobotWeld = 0.0; // 保存连续窗口内机器接焊负荷峰值
};
