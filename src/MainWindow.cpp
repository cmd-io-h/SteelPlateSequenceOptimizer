#include "MainWindow.h"

#include "SequenceOptimizer.h"
#include "TimelineSimulator.h"
#include "WorkbookService.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHash>

#include <algorithm>
#include <cmath>

namespace
{
// 创建统一范围和步长的浮点参数输入框
QDoubleSpinBox* createDoubleSpin(
    QWidget* parent,
    double minimum,
    double maximum,
    int decimals = 2,
    double step = 1.0)
{
    QDoubleSpinBox* spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    return spin;
}

// 将数值转换为适合界面显示的简洁文本
QString numberText(double value)
{
    if (std::abs(value - std::round(value)) < 1e-9) {
        return QString::number(static_cast<qlonglong>(std::llround(value)));
    }
    return QString::number(value, 'f', 2);
}

// 将秒数转换为时分秒格式
QString timeText(double seconds)
{
    if (seconds < 0.0) return QString();
    const qlonglong rounded = static_cast<qlonglong>(std::llround(seconds));
    const qlonglong hours = rounded / 3600;
    const qlonglong minutes = (rounded % 3600) / 60;
    const qlonglong remainingSeconds = rounded % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
}

// 返回甲方确认的钢板明细和各工序时序列
QStringList timelineTableHeaders()
{
    return {
        QStringLiteral("钢板明细"), QStringLiteral("规格"), QStringLiteral("材质"),
        QStringLiteral("工件数"), QStringLiteral("已用长度(mm)"), QStringLiteral("余料(mm)"),
        QStringLiteral("利用率"), QStringLiteral("工件名称"), QStringLiteral("船名"),
        QStringLiteral("分段名"), QStringLiteral("管名"), QStringLiteral("需求日期"),
        QStringLiteral("规格"), QStringLiteral("材质"), QStringLiteral("加工系列"),
        QStringLiteral("区域"), QStringLiteral("长度"), QStringLiteral("坡口"),
        QStringLiteral("线上装配信息(简写)"), QStringLiteral("搭焊"),
        QStringLiteral("喷码进入时间"), QStringLiteral("喷码完成时间"), QStringLiteral("喷码用时"),
        QStringLiteral("堵塞用时"), QStringLiteral("进入切割时间"), QStringLiteral("完成切割时间"),
        QStringLiteral("切割时间"), QStringLiteral("堵塞用时"), QStringLiteral("进入坡口时间"),
        QStringLiteral("完成坡口时间"), QStringLiteral("坡口用时"), QStringLiteral("堵塞时间"),
        QStringLiteral("进入人工组对时间"), QStringLiteral("完成人工组对时间"),
        QStringLiteral("组对用时"), QStringLiteral("堵塞用时"), QStringLiteral("接焊方式"),
        QStringLiteral("接焊工位"), QStringLiteral("进入接焊时间"), QStringLiteral("完成接焊时间"),
        QStringLiteral("接焊用时"), QStringLiteral("堵塞用时"), QStringLiteral("进入分拣时间"),
        QStringLiteral("完成分拣时间")
    };
}

// 读取界面表格的完整表头和数据，筛选隐藏行仍保留在导出结果中
WorkbookSheetData resultSheetData(const QString& name, const QTableWidget* table)
{
    WorkbookSheetData result;
    result.name = name;
    if (!table) return result;
    result.headers.reserve(table->columnCount());
    for (int column = 0; column < table->columnCount(); ++column) {
        const QTableWidgetItem* item = table->horizontalHeaderItem(column);
        result.headers.append(item ? item->text() : QString());
    }
    result.rows.reserve(table->rowCount());
    for (int row = 0; row < table->rowCount(); ++row) {
        QVector<QVariant> values;
        values.reserve(table->columnCount());
        for (int column = 0; column < table->columnCount(); ++column) {
            const QTableWidgetItem* item = table->item(row, column);
            values.append(item ? QVariant(item->text()) : QVariant());
        }
        result.rows.append(values);
    }
    return result;
}

// 按候选列名读取工件在原表中的字段
QString sourceValueText(
    const WorkbookData& data,
    const WorkpieceProcessData& workpiece,
    const QStringList& candidateHeaders,
    bool preferLast = false)
{
    auto compactText = [](QString text) {
        text = text.trimmed();
        text.remove(QLatin1Char(' '));
        return text;
    };
    QStringList compactCandidates;
    for (const QString& header : candidateHeaders) {
        compactCandidates.append(compactText(header));
    }
    const int first = preferLast ? data.sourceHeaders.size() - 1 : 0;
    const int end = preferLast ? -1 : data.sourceHeaders.size();
    const int step = preferLast ? -1 : 1;
    for (int column = first; column != end; column += step) {
        if (!compactCandidates.contains(compactText(data.sourceHeaders[column]))) continue;
        if (column < workpiece.sourceValues.size()) {
            return workpiece.sourceValues[column].toString().trimmed();
        }
    }
    return QString();
}

// 将数值列表转换为逗号分隔文本
QString listText(const QVector<double>& values)
{
    QStringList result;
    for (double value : values) result << numberText(value);
    return result.join(QStringLiteral(","));
}

// 解析用户输入的数值列表并完成排序去重
QVector<double> parseDoubleList(const QString& text)
{
    QString normalized = text;
    normalized.replace(QChar(0xFF0C), QLatin1Char(','));
    normalized.replace(QLatin1Char(';'), QLatin1Char(','));
    QVector<double> values;
    const QStringList parts = normalized.split(QLatin1Char(','), QString::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const double value = part.trimmed().toDouble(&ok);
        if (ok && value >= 0.0) values.append(value);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), [](double left, double right) {
        return std::abs(left - right) <= 1e-7;
    }), values.end());
    return values;
}

// 将宽度范围列表转换为界面文本
QString rangeListText(const QVector<WidthRange>& ranges)
{
    QStringList result;
    for (const WidthRange& range : ranges) {
        if (std::abs(range.minimum - range.maximum) <= 1e-7) {
            result << numberText(range.minimum);
        } else {
            result << numberText(range.minimum) + QStringLiteral("-") + numberText(range.maximum);
        }
    }
    return result.join(QStringLiteral(","));
}

// 解析人工接焊和机器接焊的宽度范围
QVector<WidthRange> parseRangeList(const QString& text)
{
    QString normalized = text;
    normalized.replace(QChar(0xFF0C), QLatin1Char(','));
    normalized.replace(QLatin1Char(';'), QLatin1Char(','));
    normalized.replace(QChar(0xFF1B), QLatin1Char(','));
    normalized.replace(QChar(0x2013), QLatin1Char('-'));
    normalized.replace(QChar(0x2014), QLatin1Char('-'));
    normalized.replace(QChar(0xFF5E), QLatin1Char('-'));
    normalized.replace(QLatin1Char('~'), QLatin1Char('-'));
    normalized.replace(QStringLiteral("至"), QStringLiteral("-"));
    normalized.replace(QStringLiteral("到"), QStringLiteral("-"));
    const QRegularExpression expression(
        QStringLiteral("^\\s*(\\d+(?:\\.\\d+)?)\\s*(?:-\\s*(\\d+(?:\\.\\d+)?))?\\s*$"));
    QVector<WidthRange> ranges;
    const QStringList parts = normalized.split(QLatin1Char(','), QString::SkipEmptyParts);
    for (const QString& part : parts) {
        const QRegularExpressionMatch match = expression.match(part);
        if (!match.hasMatch()) continue;
        WidthRange range;
        range.minimum = match.captured(1).toDouble();
        range.maximum = match.captured(2).isEmpty()
            ? range.minimum : match.captured(2).toDouble();
        if (range.minimum > range.maximum) std::swap(range.minimum, range.maximum);
        ranges.append(range);
    }
    return ranges;
}

// 统一配置结果表格的交互方式和显示样式
void configureResultTable(QTableWidget* table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(true);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(70);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setWordWrap(false);
}

// 从优先级表格读取单个均衡目标参数
BalanceObjectiveSetting objectiveFromRow(
    const QTableWidget* table,
    int row,
    const BalanceObjectiveSetting& fallback)
{
    BalanceObjectiveSetting result = fallback;
    if (QCheckBox* enabled = qobject_cast<QCheckBox*>(table->cellWidget(row, 0))) {
        result.enabled = enabled->isChecked();
    }
    if (QSpinBox* priority = qobject_cast<QSpinBox*>(table->cellWidget(row, 2))) {
        result.priority = priority->value();
    }
    if (QDoubleSpinBox* weight = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 3))) {
        result.weight = weight->value();
    }
    if (QDoubleSpinBox* tolerance = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 4))) {
        result.tolerance = tolerance->value();
    }
    return result;
}

// 将单个均衡目标参数写入优先级表格
void applyObjectiveToRow(
    QTableWidget* table,
    int row,
    const BalanceObjectiveSetting& setting)
{
    qobject_cast<QCheckBox*>(table->cellWidget(row, 0))->setChecked(setting.enabled);
    qobject_cast<QSpinBox*>(table->cellWidget(row, 2))->setValue(setting.priority);
    qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 3))->setValue(setting.weight);
    qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 4))->setValue(setting.tolerance);
}
}

// 初始化主窗口并加载默认参数
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , config_(OptimizerConfig::defaults())
{
    buildUi();
    const QString defaultConfig = locateDefaultConfig();
    if (!defaultConfig.isEmpty()) {
        configEdit_->setText(defaultConfig);
        QString error;
        OptimizerConfig loaded;
        if (OptimizerConfig::loadFromFile(defaultConfig, &loaded, &error)) config_ = loaded;
    }
    applyConfigToUi(config_);
}

// 创建文件导入区和三张结果表
void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("钢板套料与流水线均衡工具"));
    resize(1480, 900);

    QWidget* central = new QWidget(this);
    QVBoxLayout* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    QGroupBox* fileGroup = new QGroupBox(QStringLiteral("文件导入"), central);
    QGridLayout* fileLayout = new QGridLayout(fileGroup);
    inputEdit_ = new QLineEdit(fileGroup);
    QPushButton* inputButton = new QPushButton(QStringLiteral("选择 Excel"), fileGroup);
    QPushButton* parameterButton = new QPushButton(QStringLiteral("参数设置..."), fileGroup);
    QPushButton* analyzeButton = new QPushButton(QStringLiteral("开始计算"), fileGroup);
    exportButton_ = new QPushButton(QStringLiteral("导出结果 Excel"), fileGroup);
    exportButton_->setEnabled(false);
    summaryLabel_ = new QLabel(QStringLiteral("尚未读取数据"), fileGroup);
    parameterSummaryLabel_ = new QLabel(fileGroup);
    parameterSummaryLabel_->setStyleSheet(QStringLiteral("color:#555555;"));
    fileLayout->addWidget(new QLabel(QStringLiteral("输入 Excel："), fileGroup), 0, 0);
    fileLayout->addWidget(inputEdit_, 0, 1, 1, 4);
    fileLayout->addWidget(inputButton, 0, 5);
    fileLayout->addWidget(parameterButton, 1, 1);
    fileLayout->addWidget(analyzeButton, 1, 2);
    fileLayout->addWidget(exportButton_, 1, 3);
    fileLayout->addWidget(parameterSummaryLabel_, 1, 4, 1, 2);
    fileLayout->addWidget(summaryLabel_, 2, 1, 1, 5);
    fileLayout->setColumnStretch(1, 1);
    fileLayout->setColumnStretch(4, 1);

    QGroupBox* diameterSelectionGroup = new QGroupBox(
        QStringLiteral("参与排产的管径"), central);
    QHBoxLayout* diameterSelectionLayout = new QHBoxLayout(diameterSelectionGroup);
    smallDiameterCheck_ = new QCheckBox(QStringLiteral("小径"), diameterSelectionGroup);
    mediumDiameterCheck_ = new QCheckBox(QStringLiteral("中径"), diameterSelectionGroup);
    largeDiameterCheck_ = new QCheckBox(QStringLiteral("大径"), diameterSelectionGroup);
    smallDiameterCheck_->setChecked(true);
    mediumDiameterCheck_->setChecked(false);
    largeDiameterCheck_->setChecked(false);
    diameterSelectionLayout->addWidget(new QLabel(
        QStringLiteral("勾选后进入当前排产和时序图："), diameterSelectionGroup));
    diameterSelectionLayout->addWidget(smallDiameterCheck_);
    diameterSelectionLayout->addWidget(mediumDiameterCheck_);
    diameterSelectionLayout->addWidget(largeDiameterCheck_);
    diameterSelectionLayout->addStretch();

    resultTabs_ = new QTabWidget(central);
    resultTabs_->setDocumentMode(true);
    resultTabs_->setMovable(false);
    resultTabs_->setStyleSheet(QStringLiteral(
        "QTabBar::tab { min-width:130px; min-height:30px; font-size:14px; }"));

    QWidget* warehousePage = new QWidget(resultTabs_);
    QVBoxLayout* warehouseLayout = new QVBoxLayout(warehousePage);
    warehouseLayout->setContentsMargins(0, 6, 0, 0);
    QHBoxLayout* warehouseFilterLayout = new QHBoxLayout();
    warehouseSearchEdit_ = new QLineEdit(warehousePage);
    warehouseSearchEdit_->setPlaceholderText(QStringLiteral("输入钢板号、规格或其他关键字"));
    warehouseMaterialFilter_ = new QComboBox(warehousePage);
    warehouseThicknessFilter_ = new QComboBox(warehousePage);
    warehouseWidthFilter_ = new QComboBox(warehousePage);
    warehouseMaterialFilter_->addItem(QStringLiteral("全部材质"));
    warehouseThicknessFilter_->addItem(QStringLiteral("全部厚度"));
    warehouseWidthFilter_->addItem(QStringLiteral("全部宽度"));
    QPushButton* clearWarehouseFilterButton = new QPushButton(QStringLiteral("清空筛选"), warehousePage);
    warehouseFilterLayout->addWidget(new QLabel(QStringLiteral("筛选："), warehousePage));
    warehouseFilterLayout->addWidget(warehouseSearchEdit_, 1);
    warehouseFilterLayout->addWidget(warehouseMaterialFilter_);
    warehouseFilterLayout->addWidget(warehouseThicknessFilter_);
    warehouseFilterLayout->addWidget(warehouseWidthFilter_);
    warehouseFilterLayout->addWidget(clearWarehouseFilterButton);

    warehouseTable_ = new QTableWidget(warehousePage);
    warehouseTable_->setColumnCount(9);
    warehouseTable_->setHorizontalHeaderLabels({
        QStringLiteral("钢板组"), QStringLiteral("材质"),
        QStringLiteral("厚度(mm)"), QStringLiteral("宽度(mm)"), QStringLiteral("钢板长度(mm)"),
        QStringLiteral("钢板数量"), QStringLiteral("工件数量"), QStringLiteral("已用长度(mm)"),
        QStringLiteral("平均利用率")
    });
    configureResultTable(warehouseTable_);
    warehouseLayout->addLayout(warehouseFilterLayout);
    warehouseLayout->addWidget(warehouseTable_, 1);

    scheduleTable_ = new QTableWidget(resultTabs_);
    scheduleTable_->setColumnCount(7);
    scheduleTable_->setHorizontalHeaderLabels({
        QStringLiteral("钢板组"), QStringLiteral("材质"), QStringLiteral("规格(厚×宽)"),
        QStringLiteral("钢板长度"), QStringLiteral("工件数"), QStringLiteral("已用长度"),
        QStringLiteral("当前钢板组内的工件")
    });
    configureResultTable(scheduleTable_);

    timelineTable_ = new QTableWidget(resultTabs_);
    const QStringList timelineHeaders = timelineTableHeaders();
    timelineTable_->setColumnCount(timelineHeaders.size());
    timelineTable_->setHorizontalHeaderLabels(timelineHeaders);
    configureResultTable(timelineTable_);

    resultTabs_->addTab(warehousePage, QStringLiteral("立库"));
    resultTabs_->addTab(scheduleTable_, QStringLiteral("排产"));
    resultTabs_->addTab(timelineTable_, QStringLiteral("时序图"));

    connect(warehouseSearchEdit_, &QLineEdit::textChanged,
        this, [this]() { applyWarehouseFilters(); });
    connect(warehouseMaterialFilter_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this]() { applyWarehouseFilters(); });
    connect(warehouseThicknessFilter_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this]() { applyWarehouseFilters(); });
    connect(warehouseWidthFilter_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this]() { applyWarehouseFilters(); });
    connect(clearWarehouseFilterButton, &QPushButton::clicked, this, [this]() {
        warehouseSearchEdit_->clear();
        warehouseMaterialFilter_->setCurrentIndex(0);
        warehouseThicknessFilter_->setCurrentIndex(0);
        warehouseWidthFilter_->setCurrentIndex(0);
        applyWarehouseFilters();
    });
    const auto refreshActiveSchedule = [this]() {
        if (sourceWorkbookData_.inputPath.isEmpty()) {
            populatePreview();
            return;
        }
        analyzeAndOptimize();
    };
    connect(smallDiameterCheck_, &QCheckBox::toggled, this, refreshActiveSchedule);
    connect(mediumDiameterCheck_, &QCheckBox::toggled, this, refreshActiveSchedule);
    connect(largeDiameterCheck_, &QCheckBox::toggled, this, refreshActiveSchedule);

    logEdit_ = new QPlainTextEdit(central);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(2000);
    logEdit_->setVisible(false);

    rootLayout->addWidget(fileGroup);
    rootLayout->addWidget(diameterSelectionGroup);
    rootLayout->addWidget(resultTabs_, 1);
    setCentralWidget(central);

    buildParameterDialog();
    populateTimelineTable();

    connect(inputButton, &QPushButton::clicked, this, [this]() { browseInput(); });
    connect(parameterButton, &QPushButton::clicked, this, [this]() { openParameterDialog(); });
    connect(analyzeButton, &QPushButton::clicked, this, [this]() { analyzeAndOptimize(); });
    connect(exportButton_, &QPushButton::clicked, this, [this]() { exportResult(); });
}

// 创建套料参数、均衡优先级和时序参数页面
void MainWindow::buildParameterDialog()
{
    parameterDialog_ = new QDialog(this);
    parameterDialog_->setWindowTitle(QStringLiteral("参数设置"));
    parameterDialog_->resize(920, 610);
    QVBoxLayout* dialogLayout = new QVBoxLayout(parameterDialog_);

    configEdit_ = new QLineEdit(parameterDialog_);
    configEdit_->setVisible(false);

    QTabWidget* parameterTabs = new QTabWidget(parameterDialog_);
    QWidget* nestingTab = new QWidget(parameterTabs);
    QVBoxLayout* nestingLayout = new QVBoxLayout(nestingTab);
    QGroupBox* groupingGroup = new QGroupBox(QStringLiteral("分组与一维套料硬条件"), nestingTab);
    QGridLayout* groupingLayout = new QGridLayout(groupingGroup);
    inputModeCombo_ = new QComboBox(groupingGroup);
    inputModeCombo_->addItem(QStringLiteral("自动识别"), QStringLiteral("auto"));
    inputModeCombo_->addItem(QStringLiteral("原始工件表（先分组套料）"), QStringLiteral("raw"));
    inputModeCombo_->addItem(QStringLiteral("已有钢板明细（只调顺序）"), QStringLiteral("detail"));
    groupMaterialCheck_ = new QCheckBox(QStringLiteral("按材质分组"), groupingGroup);
    groupSpecificationCheck_ = new QCheckBox(QStringLiteral("按规格分组"), groupingGroup);
    defaultStockLengthSpin_ = createDoubleSpin(groupingGroup, 1.0, 100000.0, 1, 100.0);
    kerfSpin_ = createDoubleSpin(groupingGroup, 0.0, 1000.0, 2, 0.5);
    endAllowanceSpin_ = createDoubleSpin(groupingGroup, 0.0, 10000.0, 2, 1.0);
    lengthThresholdsEdit_ = new QLineEdit(groupingGroup);
    lengthThresholdsEdit_->setPlaceholderText(QStringLiteral("0表示不启用；例如：600,900"));
    groupingLayout->addWidget(new QLabel(QStringLiteral("输入模式"), groupingGroup), 0, 0);
    groupingLayout->addWidget(inputModeCombo_, 0, 1, 1, 3);
    groupingLayout->addWidget(groupMaterialCheck_, 1, 0);
    groupingLayout->addWidget(groupSpecificationCheck_, 1, 1);
    groupingLayout->addWidget(new QLabel(QStringLiteral("钢板长度(mm)"), groupingGroup), 2, 0);
    groupingLayout->addWidget(defaultStockLengthSpin_, 2, 1);
    groupingLayout->addWidget(new QLabel(QStringLiteral("切缝(mm)"), groupingGroup), 2, 2);
    groupingLayout->addWidget(kerfSpin_, 2, 3);
    groupingLayout->addWidget(new QLabel(QStringLiteral("首尾余量合计(mm)"), groupingGroup), 3, 0);
    groupingLayout->addWidget(endAllowanceSpin_, 3, 1);
    groupingLayout->addWidget(new QLabel(QStringLiteral("套料长度分界值(mm)"), groupingGroup), 3, 2);
    groupingLayout->addWidget(lengthThresholdsEdit_, 3, 3);
    QLabel* stockLengthHint = new QLabel(
        QStringLiteral("同一次计算中的所有钢板统一使用上面的钢板长度。"), groupingGroup);
    stockLengthHint->setStyleSheet(QStringLiteral("color:#666666;"));
    groupingLayout->addWidget(stockLengthHint, 4, 0, 1, 4);

    QGroupBox* routingGroup = new QGroupBox(QStringLiteral("接焊路线（按宽度）"), nestingTab);
    QGridLayout* routingLayout = new QGridLayout(routingGroup);
    manualWidthsEdit_ = new QLineEdit(routingGroup);
    robotWidthsEdit_ = new QLineEdit(routingGroup);
    mediumWidthsEdit_ = new QLineEdit(routingGroup);
    largeWidthsEdit_ = new QLineEdit(routingGroup);
    manualWidthsEdit_->setPlaceholderText(QStringLiteral("例如：22-34"));
    robotWidthsEdit_->setPlaceholderText(QStringLiteral("例如：42-60"));
    mediumWidthsEdit_->setPlaceholderText(QStringLiteral("例如：76-114"));
    largeWidthsEdit_->setPlaceholderText(QStringLiteral("例如：140-377"));
    routingLayout->addWidget(new QLabel(QStringLiteral("小径人工接焊宽度范围"), routingGroup), 0, 0);
    routingLayout->addWidget(manualWidthsEdit_, 0, 1);
    routingLayout->addWidget(new QLabel(QStringLiteral("小径机器接焊宽度范围"), routingGroup), 0, 2);
    routingLayout->addWidget(robotWidthsEdit_, 0, 3);
    routingLayout->addWidget(new QLabel(QStringLiteral("中径机器接焊宽度范围"), routingGroup), 1, 0);
    routingLayout->addWidget(mediumWidthsEdit_, 1, 1, 1, 3);
    routingLayout->addWidget(new QLabel(QStringLiteral("大径机器接焊宽度范围"), routingGroup), 2, 0);
    routingLayout->addWidget(largeWidthsEdit_, 2, 1, 1, 3);
    QLabel* routingHint = new QLabel(
        QStringLiteral("中径和大径当前计入机器接焊负荷，时序节拍沿用机器接焊参数。"),
        routingGroup);
    routingHint->setStyleSheet(QStringLiteral("color:#666666;"));
    routingLayout->addWidget(routingHint, 3, 0, 1, 4);
    nestingLayout->addWidget(groupingGroup);
    nestingLayout->addWidget(routingGroup);
    nestingLayout->addStretch();
    parameterTabs->addTab(nestingTab, QStringLiteral("套料与规格参数"));

    QWidget* balanceTab = new QWidget(parameterTabs);
    QVBoxLayout* balanceLayout = new QVBoxLayout(balanceTab);
    objectiveTable_ = new QTableWidget(2, 5, balanceTab);
    objectiveTable_->setHorizontalHeaderLabels({
        QStringLiteral("启用"), QStringLiteral("均衡目标"), QStringLiteral("优先级"),
        QStringLiteral("权重"), QStringLiteral("允许偏差")
    });
    objectiveTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    objectiveTable_->verticalHeader()->setVisible(false);
    objectiveTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QStringList names = {
        QStringLiteral("坡口"), QStringLiteral("焊接")
    };
    for (int row = 0; row < 2; ++row) {
        QCheckBox* enabled = new QCheckBox(objectiveTable_);
        enabled->setStyleSheet(QStringLiteral("margin-left:45%;"));
        QSpinBox* priority = new QSpinBox(objectiveTable_);
        priority->setRange(1, 20);
        QDoubleSpinBox* weight = createDoubleSpin(objectiveTable_, 0.0, 100.0, 2, 0.25);
        QDoubleSpinBox* tolerance = createDoubleSpin(objectiveTable_, 0.0, 1.0, 3, 0.01);
        objectiveTable_->setCellWidget(row, 0, enabled);
        objectiveTable_->setItem(row, 1, new QTableWidgetItem(names[row]));
        objectiveTable_->setCellWidget(row, 2, priority);
        objectiveTable_->setCellWidget(row, 3, weight);
        objectiveTable_->setCellWidget(row, 4, tolerance);
    }

    balanceLayout->addWidget(objectiveTable_);
    QLabel* balanceHint = new QLabel(
        QStringLiteral("“焊接”使用同一组参数，但人工接焊和机器接焊仍分别统计、分别均衡。"),
        balanceTab);
    balanceHint->setStyleSheet(QStringLiteral("color:#666666;"));
    balanceLayout->addWidget(balanceHint);
    balanceLayout->addStretch();
    parameterTabs->addTab(balanceTab, QStringLiteral("流水线优先级"));

    QWidget* timingTab = new QWidget(parameterTabs);
    QVBoxLayout* timingLayout = new QVBoxLayout(timingTab);
    QGroupBox* flowTimingGroup = new QGroupBox(QStringLiteral("后续工序节拍"), timingTab);
    QGridLayout* flowTimingLayout = new QGridLayout(flowTimingGroup);
    downstreamMinimumLengthSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 1, 50.0);
    bufferCapacitySpin_ = new QSpinBox(flowTimingGroup);
    bufferCapacitySpin_->setRange(0, 100);
    sprayIncrementSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 2, 1.0);
    bevelStationCountSpin_ = new QSpinBox(flowTimingGroup);
    bevelStationCountSpin_->setRange(1, 20);
    bevelCycleSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 2, 1.0);
    tackCycleSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 2, 1.0);
    manualWeldStationCountSpin_ = new QSpinBox(flowTimingGroup);
    manualWeldStationCountSpin_->setRange(1, 20);
    manualWeldCycleSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 2, 1.0);
    robotWeldStationCountSpin_ = new QSpinBox(flowTimingGroup);
    robotWeldStationCountSpin_->setRange(1, 20);
    robotWeldFixedSpin_ = createDoubleSpin(flowTimingGroup, 0.0, 100000.0, 2, 1.0);
    robotWeldSpeedSpin_ = createDoubleSpin(flowTimingGroup, 0.01, 10000.0, 2, 0.1);

    flowTimingLayout->addWidget(new QLabel(QStringLiteral("进入后续最小长度(mm)"), flowTimingGroup), 0, 0);
    flowTimingLayout->addWidget(downstreamMinimumLengthSpin_, 0, 1);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("各工序缓冲位"), flowTimingGroup), 0, 2);
    flowTimingLayout->addWidget(bufferCapacitySpin_, 0, 3);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("喷码单件增量(s)"), flowTimingGroup), 1, 0);
    flowTimingLayout->addWidget(sprayIncrementSpin_, 1, 1);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("坡口工位数"), flowTimingGroup), 1, 2);
    flowTimingLayout->addWidget(bevelStationCountSpin_, 1, 3);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("坡口节拍(s)"), flowTimingGroup), 2, 0);
    flowTimingLayout->addWidget(bevelCycleSpin_, 2, 1);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("人工搭焊(s)"), flowTimingGroup), 2, 2);
    flowTimingLayout->addWidget(tackCycleSpin_, 2, 3);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("人工接焊工位数"), flowTimingGroup), 3, 0);
    flowTimingLayout->addWidget(manualWeldStationCountSpin_, 3, 1);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("人工接焊节拍(s)"), flowTimingGroup), 3, 2);
    flowTimingLayout->addWidget(manualWeldCycleSpin_, 3, 3);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("机器接焊工位数"), flowTimingGroup), 4, 0);
    flowTimingLayout->addWidget(robotWeldStationCountSpin_, 4, 1);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("机器接焊固定动作(s)"), flowTimingGroup), 4, 2);
    flowTimingLayout->addWidget(robotWeldFixedSpin_, 4, 3);
    flowTimingLayout->addWidget(new QLabel(QStringLiteral("机器接焊速度(mm/s)"), flowTimingGroup), 5, 0);
    flowTimingLayout->addWidget(robotWeldSpeedSpin_, 5, 1);

    QGroupBox* sawTimingGroup = new QGroupBox(QStringLiteral("圆锯切割节拍"), timingTab);
    QGridLayout* sawTimingLayout = new QGridLayout(sawTimingGroup);
    sawFeedSpeedSpin_ = createDoubleSpin(sawTimingGroup, 0.01, 100000.0, 2, 10.0);
    sawFirstFeedSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawMaximumFeedSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawClampSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawReleaseSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawUnloadSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawPlateResetSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawCutUnder35Spin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawCutUnder61Spin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawCutUnder90Spin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);
    sawCutOtherSpin_ = createDoubleSpin(sawTimingGroup, 0.0, 100000.0, 2, 1.0);

    sawTimingLayout->addWidget(new QLabel(QStringLiteral("送料速度(mm/s)"), sawTimingGroup), 0, 0);
    sawTimingLayout->addWidget(sawFeedSpeedSpin_, 0, 1);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("第一次送料(s)"), sawTimingGroup), 0, 2);
    sawTimingLayout->addWidget(sawFirstFeedSpin_, 0, 3);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("单次送料上限(s)"), sawTimingGroup), 1, 0);
    sawTimingLayout->addWidget(sawMaximumFeedSpin_, 1, 1);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("夹紧/松开(s)"), sawTimingGroup), 1, 2);
    QHBoxLayout* clampReleaseLayout = new QHBoxLayout();
    clampReleaseLayout->addWidget(sawClampSpin_);
    clampReleaseLayout->addWidget(sawReleaseSpin_);
    sawTimingLayout->addLayout(clampReleaseLayout, 1, 3);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("工件下料(s)"), sawTimingGroup), 2, 0);
    sawTimingLayout->addWidget(sawUnloadSpin_, 2, 1);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("钢板切完回位(s)"), sawTimingGroup), 2, 2);
    sawTimingLayout->addWidget(sawPlateResetSpin_, 2, 3);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("切割<35 / <61(s)"), sawTimingGroup), 3, 0);
    QHBoxLayout* firstCutLayout = new QHBoxLayout();
    firstCutLayout->addWidget(sawCutUnder35Spin_);
    firstCutLayout->addWidget(sawCutUnder61Spin_);
    sawTimingLayout->addLayout(firstCutLayout, 3, 1);
    sawTimingLayout->addWidget(new QLabel(QStringLiteral("切割<90 / 其他(s)"), sawTimingGroup), 3, 2);
    QHBoxLayout* secondCutLayout = new QHBoxLayout();
    secondCutLayout->addWidget(sawCutUnder90Spin_);
    secondCutLayout->addWidget(sawCutOtherSpin_);
    sawTimingLayout->addLayout(secondCutLayout, 3, 3);

    timingLayout->addWidget(flowTimingGroup);
    timingLayout->addWidget(sawTimingGroup);
    timingLayout->addStretch();
    parameterTabs->addTab(timingTab, QStringLiteral("时序参数"));

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, parameterDialog_);
    buttonBox->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));

    dialogLayout->addWidget(parameterTabs, 1);
    dialogLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        OptimizerConfig candidate = configFromUi();
        QString error;
        if (!candidate.validate(&error)) {
            QMessageBox::warning(parameterDialog_, QStringLiteral("参数错误"), error);
            return;
        }
        config_ = candidate;
        updateParameterSummary();
        parameterDialog_->accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, parameterDialog_, &QDialog::reject);
}

// 打开参数设置窗口并在确认后保存界面参数
void MainWindow::openParameterDialog()
{
    applyConfigToUi(config_);
    if (parameterDialog_->exec() != QDialog::Accepted) applyConfigToUi(config_);
}

// 选择需要处理的 Excel 文件
void MainWindow::browseInput()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择原始工件表或钢板明细"),
        QFileInfo(inputEdit_->text()).absolutePath(), QStringLiteral("Excel 工作簿 (*.xlsx)"));
    if (!path.isEmpty()) {
        inputEdit_->setText(path);
        sourceWorkbookData_ = WorkbookData();
        workbookData_ = WorkbookData();
        optimizedOrder_.clear();
        calculationSummaryBase_.clear();
        summaryLabel_->clear();
        exportButton_->setEnabled(false);
        populatePreview();
    }
}

// 选择外部 JSON 参数文件
void MainWindow::browseConfig()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择参数文件"), QFileInfo(configEdit_->text()).absolutePath(),
        QStringLiteral("JSON 参数 (*.json)"));
    if (!path.isEmpty()) configEdit_->setText(path);
}

// 加载 JSON 参数并同步到参数界面
void MainWindow::loadConfig()
{
    QString error;
    OptimizerConfig loaded;
    if (!OptimizerConfig::loadFromFile(configEdit_->text().trimmed(), &loaded, &error)) {
        QMessageBox::warning(this, QStringLiteral("参数加载失败"), error);
        return;
    }
    config_ = loaded;
    applyConfigToUi(config_);
    logEdit_->appendPlainText(QStringLiteral("已加载参数：%1").arg(configEdit_->text()));
}

// 将当前参数保存为 JSON 文件
void MainWindow::saveConfig()
{
    QString path = configEdit_->text().trimmed();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存参数文件"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/config/default.json"),
            QStringLiteral("JSON 参数 (*.json)"));
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) path += QStringLiteral(".json");
    config_ = configFromUi();
    QString error;
    if (!config_.saveToFile(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("参数保存失败"), error);
        return;
    }
    configEdit_->setText(path);
    logEdit_->appendPlainText(QStringLiteral("已保存参数：%1").arg(path));
}

// 读取工件数据、执行套料排序并刷新三张结果表
bool MainWindow::analyzeAndOptimize()
{
    const QString inputPath = inputEdit_->text().trimmed();
    if (inputPath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择文件"), QStringLiteral("请先选择输入 Excel。"));
        return false;
    }
    config_ = configFromUi();
    QString error;
    if (!config_.validate(&error)) {
        QMessageBox::warning(this, QStringLiteral("参数错误"), error);
        return false;
    }
    if (!smallDiameterCheck_->isChecked() &&
        !mediumDiameterCheck_->isChecked() &&
        !largeDiameterCheck_->isChecked()) {
        sourceWorkbookData_ = WorkbookData();
        sourceWorkbookData_.inputPath = QFileInfo(inputPath).absoluteFilePath();
        workbookData_ = WorkbookData();
        optimizedOrder_.clear();
        calculationSummaryBase_ = QStringLiteral("请至少勾选一种参与排产的管径");
        summaryLabel_->setText(calculationSummaryBase_);
        exportButton_->setEnabled(false);
        populatePreview();
        return false;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    WorkbookData loadedData;
    const bool loaded = WorkbookService::load(
        inputPath, config_,
        smallDiameterCheck_->isChecked(),
        mediumDiameterCheck_->isChecked(),
        largeDiameterCheck_->isChecked(),
        &loadedData, &error);
    QApplication::restoreOverrideCursor();
    if (!loaded) {
        sourceWorkbookData_ = WorkbookData();
        sourceWorkbookData_.inputPath = QFileInfo(inputPath).absoluteFilePath();
        workbookData_ = WorkbookData();
        optimizedOrder_.clear();
        calculationSummaryBase_ = error;
        summaryLabel_->setText(calculationSummaryBase_);
        exportButton_->setEnabled(false);
        populatePreview();
        QMessageBox::warning(this, QStringLiteral("处理失败"), error);
        return false;
    }

    sourceWorkbookData_ = loadedData;
    rebuildActiveSchedule();
    if (optimizedOrder_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("当前范围没有数据"),
            QStringLiteral("当前勾选的管径没有可排产工件"));
        return false;
    }
    return true;
}

// 按当前管径选择生成本次计算数据，并重新完成均衡排序和结果展示
void MainWindow::rebuildActiveSchedule()
{
    workbookData_ = sourceWorkbookData_;
    workbookData_.plates.clear();
    workbookData_.validWorkpieceCount = 0;

    // 未勾选管径在优化前剔除，不进入立库、排产、时序图和导出
    for (const SteelPlateBlock& sourcePlate : sourceWorkbookData_.plates) {
        if (!isDiameterSelected(sourcePlate.width)) continue;
        workbookData_.plates.append(sourcePlate);
        workbookData_.validWorkpieceCount += sourcePlate.workpieceCount;
    }
    if (!workbookData_.generatedFromRaw && !workbookData_.plates.isEmpty() &&
        !sourceWorkbookData_.plates.isEmpty()) {
        // 明细表导出仍从原数据区第一行开始，避免保留未勾选管径的旧行
        workbookData_.plates.first().startRow = sourceWorkbookData_.plates.first().startRow;
    }

    optimizedOrder_ = SequenceOptimizer::optimize(workbookData_.plates, config_);
    QVector<int> originalOrder;
    for (int i = 0; i < workbookData_.plates.size(); ++i) originalOrder.append(i);
    const BalanceMetrics before = SequenceOptimizer::calculateMetrics(
        workbookData_.plates, originalOrder, config_);
    const BalanceMetrics after = SequenceOptimizer::calculateMetrics(
        workbookData_.plates, optimizedOrder_, config_);

    logEdit_->clear();
    logEdit_->appendPlainText(workbookData_.generatedFromRaw
        ? QStringLiteral("原始工件已完成分组和一维套料。")
        : QStringLiteral("已识别现有钢板明细，只调整整张钢板顺序。"));
    logEdit_->appendPlainText(QStringLiteral(
        "有效工件 %1 个，跳过 %2 行；钢板 %3 张；坡口 %4，人工焊 %5，机器焊 %6，未分类焊 %7。")
        .arg(workbookData_.validWorkpieceCount)
        .arg(workbookData_.skippedWorkpieceCount)
        .arg(workbookData_.plates.size())
        .arg(numberText(after.totalBevel))
        .arg(numberText(after.totalManualWeld))
        .arg(numberText(after.totalRobotWeld))
        .arg(numberText(after.totalUnclassifiedWeld)));
    appendMetrics(QStringLiteral("调整前"), before, workbookData_.plates.size());
    appendMetrics(QStringLiteral("调整后"), after, workbookData_.plates.size());
    for (int index = 0; index < std::min(10, workbookData_.warnings.size()); ++index) {
        logEdit_->appendPlainText(QStringLiteral("提示：") + workbookData_.warnings[index]);
    }
    if (workbookData_.warnings.size() > 10) {
        logEdit_->appendPlainText(QStringLiteral("另有 %1 条提示未展开。")
            .arg(workbookData_.warnings.size() - 10));
    }

    calculationSummaryBase_ = QStringLiteral("钢板 %1 张")
        .arg(workbookData_.plates.size());
    summaryLabel_->setText(calculationSummaryBase_);
    populatePreview();
    exportButton_->setEnabled(!optimizedOrder_.isEmpty());
}

// 将立库、排产和时序图一次性导出到同一个 Excel 文件
void MainWindow::exportResult()
{
    if (optimizedOrder_.isEmpty() && !analyzeAndOptimize()) return;
    const QString suggested = WorkbookService::defaultOutputPath(
        workbookData_.inputPath, config_.outputSuffix);
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出最终 Excel"), suggested, QStringLiteral("Excel 工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".xlsx"), Qt::CaseInsensitive)) path += QStringLiteral(".xlsx");
    if (QFileInfo::exists(path)) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("覆盖文件"), QStringLiteral("输出文件已存在，是否覆盖？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    QString error;
    const QVector<WorkbookSheetData> sheets = {
        resultSheetData(QStringLiteral("立库"), warehouseTable_),
        resultSheetData(QStringLiteral("排产"), scheduleTable_),
        resultSheetData(QStringLiteral("时序图"), timelineTable_)
    };
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = WorkbookService::exportResultTables(sheets, path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    logEdit_->appendPlainText(QStringLiteral("已导出：%1").arg(path));
    QMessageBox::information(
        this, QStringLiteral("完成"),
        QStringLiteral("立库、排产和时序图已导出到同一个 Excel 文件。"));
}

// 将配置对象中的参数写入各个界面控件
void MainWindow::applyConfigToUi(const OptimizerConfig& config)
{
    const int modeIndex = inputModeCombo_->findData(config.inputMode);
    inputModeCombo_->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    groupMaterialCheck_->setChecked(config.groupByMaterial);
    groupSpecificationCheck_->setChecked(config.groupByThickness || config.groupByWidth);
    defaultStockLengthSpin_->setValue(config.defaultStockLength);
    kerfSpin_->setValue(config.kerf);
    endAllowanceSpin_->setValue(config.endAllowance);
    lengthThresholdsEdit_->setText(listText(config.lengthThresholds));
    manualWidthsEdit_->setText(rangeListText(config.manualWeldRanges));
    robotWidthsEdit_->setText(rangeListText(config.robotWeldRanges));
    mediumWidthsEdit_->setText(rangeListText(config.mediumWeldRanges));
    largeWidthsEdit_->setText(rangeListText(config.largeWeldRanges));
    applyObjectiveToRow(objectiveTable_, 0, config.bevelObjective);
    applyObjectiveToRow(objectiveTable_, 1, config.manualWeldObjective);
    downstreamMinimumLengthSpin_->setValue(config.downstreamMinimumLength);
    bufferCapacitySpin_->setValue(config.bufferCapacity);
    sprayIncrementSpin_->setValue(config.sprayIncrementSeconds);
    sawFeedSpeedSpin_->setValue(config.sawFeedSpeedMmPerSecond);
    sawFirstFeedSpin_->setValue(config.sawFirstFeedSeconds);
    sawMaximumFeedSpin_->setValue(config.sawMaximumFeedSeconds);
    sawClampSpin_->setValue(config.sawClampSeconds);
    sawReleaseSpin_->setValue(config.sawReleaseSeconds);
    sawUnloadSpin_->setValue(config.sawUnloadSeconds);
    sawPlateResetSpin_->setValue(config.sawPlateResetSeconds);
    sawCutUnder35Spin_->setValue(config.sawCutSecondsUnder35);
    sawCutUnder61Spin_->setValue(config.sawCutSecondsUnder61);
    sawCutUnder90Spin_->setValue(config.sawCutSecondsUnder90);
    sawCutOtherSpin_->setValue(config.sawCutSecondsOther);
    bevelStationCountSpin_->setValue(config.bevelStationCount);
    bevelCycleSpin_->setValue(config.bevelCycleSeconds);
    tackCycleSpin_->setValue(config.tackCycleSeconds);
    manualWeldStationCountSpin_->setValue(config.manualWeldStationCount);
    manualWeldCycleSpin_->setValue(config.manualWeldCycleSeconds);
    robotWeldStationCountSpin_->setValue(config.robotWeldStationCount);
    robotWeldFixedSpin_->setValue(config.robotWeldFixedSeconds);
    robotWeldSpeedSpin_->setValue(config.robotWeldSpeedMmPerSecond);
    updateParameterSummary();
}

// 读取界面控件并生成完整配置对象
OptimizerConfig MainWindow::configFromUi() const
{
    OptimizerConfig result = config_;
    result.inputMode = inputModeCombo_->currentData().toString();
    result.groupByMaterial = groupMaterialCheck_->isChecked();
    result.groupByThickness = groupSpecificationCheck_->isChecked();
    result.groupByWidth = groupSpecificationCheck_->isChecked();
    result.defaultStockLength = defaultStockLengthSpin_->value();
    result.kerf = kerfSpin_->value();
    result.endAllowance = endAllowanceSpin_->value();
    result.lengthThresholds = parseDoubleList(lengthThresholdsEdit_->text());
    result.manualWeldRanges = parseRangeList(manualWidthsEdit_->text());
    result.robotWeldRanges = parseRangeList(robotWidthsEdit_->text());
    result.mediumWeldRanges = parseRangeList(mediumWidthsEdit_->text());
    result.largeWeldRanges = parseRangeList(largeWidthsEdit_->text());
    result.stockLengthRules.clear();
    result.bevelObjective = objectiveFromRow(objectiveTable_, 0, result.bevelObjective);
    result.manualWeldObjective = objectiveFromRow(objectiveTable_, 1, result.manualWeldObjective);
    result.robotWeldObjective = result.manualWeldObjective;
    result.downstreamMinimumLength = downstreamMinimumLengthSpin_->value();
    result.bufferCapacity = bufferCapacitySpin_->value();
    result.sprayIncrementSeconds = sprayIncrementSpin_->value();
    result.sawFeedSpeedMmPerSecond = sawFeedSpeedSpin_->value();
    result.sawFirstFeedSeconds = sawFirstFeedSpin_->value();
    result.sawMaximumFeedSeconds = sawMaximumFeedSpin_->value();
    result.sawClampSeconds = sawClampSpin_->value();
    result.sawReleaseSeconds = sawReleaseSpin_->value();
    result.sawUnloadSeconds = sawUnloadSpin_->value();
    result.sawPlateResetSeconds = sawPlateResetSpin_->value();
    result.sawCutSecondsUnder35 = sawCutUnder35Spin_->value();
    result.sawCutSecondsUnder61 = sawCutUnder61Spin_->value();
    result.sawCutSecondsUnder90 = sawCutUnder90Spin_->value();
    result.sawCutSecondsOther = sawCutOtherSpin_->value();
    result.bevelStationCount = bevelStationCountSpin_->value();
    result.bevelCycleSeconds = bevelCycleSpin_->value();
    result.tackCycleSeconds = tackCycleSpin_->value();
    result.manualWeldStationCount = manualWeldStationCountSpin_->value();
    result.manualWeldCycleSeconds = manualWeldCycleSpin_->value();
    result.robotWeldStationCount = robotWeldStationCountSpin_->value();
    result.robotWeldFixedSeconds = robotWeldFixedSpin_->value();
    result.robotWeldSpeedMmPerSecond = robotWeldSpeedSpin_->value();
    return result;
}

// 统一刷新立库、排产和时序图页面
void MainWindow::populatePreview()
{
    populateWarehouseTable();
    populateScheduleTable();
    populateTimelineTable();
    resultTabs_->setCurrentIndex(0);
}

// 按材质和规格汇总钢板库存信息
void MainWindow::populateWarehouseTable()
{
    struct WarehouseSummary
    {
        QString name;
        QString material;
        double thickness = 0.0;
        double width = 0.0;
        double stockLength = 0.0;
        int plateCount = 0;
        int workpieceCount = 0;
        double usedLength = 0.0;
    };

    QVector<WarehouseSummary> summaries;
    QHash<QString, int> summaryIndex;
    for (const SteelPlateBlock& plate : workbookData_.plates) {
        const QString key = QStringLiteral("%1|%2|%3|%4")
            .arg(plate.material)
            .arg(plate.thickness, 0, 'g', 15)
            .arg(plate.width, 0, 'g', 15)
            .arg(plate.stockLength, 0, 'g', 15);
        int index = summaryIndex.value(key, -1);
        if (index < 0) {
            WarehouseSummary summary;
            summary.name = plate.specification.trimmed().isEmpty()
                ? plate.plateId : plate.specification;
            summary.material = plate.material;
            summary.thickness = plate.thickness;
            summary.width = plate.width;
            summary.stockLength = plate.stockLength;
            summaries.append(summary);
            index = summaries.size() - 1;
            summaryIndex.insert(key, index);
        }
        WarehouseSummary& summary = summaries[index];
        ++summary.plateCount;
        summary.workpieceCount += plate.workpieceCount;
        summary.usedLength += plate.usedLength;
    }

    warehouseTable_->clearContents();
    warehouseTable_->setRowCount(summaries.size());
    QStringList materials;
    QStringList thicknesses;
    QStringList widths;
    for (int row = 0; row < summaries.size(); ++row) {
        const WarehouseSummary& summary = summaries[row];
        const double utilization = summary.plateCount > 0 && summary.stockLength > 0.0
            ? summary.usedLength / (summary.plateCount * summary.stockLength) : 0.0;
        const QStringList values = {
            summary.name, summary.material,
            numberText(summary.thickness), numberText(summary.width),
            numberText(summary.stockLength), QString::number(summary.plateCount),
            QString::number(summary.workpieceCount), numberText(summary.usedLength),
            QString::number(utilization * 100.0, 'f', 1) + QStringLiteral("%")
        };
        for (int column = 0; column < values.size(); ++column) {
            QTableWidgetItem* item = new QTableWidgetItem(values[column]);
            if (column != 0 && column != 1) item->setTextAlignment(Qt::AlignCenter);
            warehouseTable_->setItem(row, column, item);
        }
        if (!materials.contains(summary.material)) materials.append(summary.material);
        const QString thickness = numberText(summary.thickness);
        const QString width = numberText(summary.width);
        if (!thicknesses.contains(thickness)) thicknesses.append(thickness);
        if (!widths.contains(width)) widths.append(width);
    }
    materials.sort(Qt::CaseInsensitive);
    const auto numericTextLess = [](const QString& left, const QString& right) {
        return left.toDouble() < right.toDouble();
    };
    std::sort(thicknesses.begin(), thicknesses.end(), numericTextLess);
    std::sort(widths.begin(), widths.end(), numericTextLess);
    auto refillFilter = [](QComboBox* combo, const QString& allText, const QStringList& values) {
        const QString previous = combo->currentText();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(allText);
        combo->addItems(values);
        const int previousIndex = combo->findText(previous);
        combo->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    };
    refillFilter(warehouseMaterialFilter_, QStringLiteral("全部材质"), materials);
    refillFilter(warehouseThicknessFilter_, QStringLiteral("全部厚度"), thicknesses);
    refillFilter(warehouseWidthFilter_, QStringLiteral("全部宽度"), widths);
    warehouseTable_->resizeColumnsToContents();
    applyWarehouseFilters();
}

// 按照关键字、材质、厚度和宽度筛选立库数据
void MainWindow::applyWarehouseFilters()
{
    if (!warehouseTable_) return;
    const QString keyword = warehouseSearchEdit_->text().trimmed();
    const QString material = warehouseMaterialFilter_->currentIndex() > 0
        ? warehouseMaterialFilter_->currentText() : QString();
    const QString thickness = warehouseThicknessFilter_->currentIndex() > 0
        ? warehouseThicknessFilter_->currentText() : QString();
    const QString width = warehouseWidthFilter_->currentIndex() > 0
        ? warehouseWidthFilter_->currentText() : QString();
    for (int row = 0; row < warehouseTable_->rowCount(); ++row) {
        const auto textAt = [this, row](int column) {
            const QTableWidgetItem* item = warehouseTable_->item(row, column);
            return item ? item->text() : QString();
        };
        bool matches = material.isEmpty() || textAt(1) == material;
        matches = matches && (thickness.isEmpty() || textAt(2) == thickness);
        matches = matches && (width.isEmpty() || textAt(3) == width);
        if (matches && !keyword.isEmpty()) {
            matches = false;
            for (int column = 0; column < warehouseTable_->columnCount(); ++column) {
                if (textAt(column).contains(keyword, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
        }
        warehouseTable_->setRowHidden(row, !matches);
    }
}

// 按排产顺序逐工件展示钢板信息和原表整行数据
void MainWindow::populateScheduleTable()
{
    scheduleTable_->clearContents();
    QStringList headers = {
        QStringLiteral("钢板组"), QStringLiteral("材质"), QStringLiteral("规格(厚×宽)"),
        QStringLiteral("钢板长度"), QStringLiteral("工件数"), QStringLiteral("已用长度"),
        QStringLiteral("当前工件")
    };
    const QStringList mergedHeaders = {
        QStringLiteral("钢板明细"), QStringLiteral("钢板组"),
        QStringLiteral("材质"), QStringLiteral("规格"), QStringLiteral("规格(厚×宽)"),
        QStringLiteral("宽度"), QStringLiteral("宽度(mm)"),
        QStringLiteral("厚度"), QStringLiteral("厚度(mm)"),
        QStringLiteral("钢板编号"), QStringLiteral("钢板长度"), QStringLiteral("钢板长度(mm)"),
        QStringLiteral("工件数"), QStringLiteral("工件数量"),
        QStringLiteral("已用长度"), QStringLiteral("已用长度(mm)"),
        QStringLiteral("余料"), QStringLiteral("余料(mm)"), QStringLiteral("利用率"),
        QStringLiteral("工件名称"), QStringLiteral("管名")
    };
    QVector<int> sourceColumns;
    for (int column = 0; column < workbookData_.sourceHeaders.size(); ++column) {
        QString header = workbookData_.sourceHeaders[column].trimmed();
        QString compact = header;
        compact.remove(QLatin1Char(' '));
        if (mergedHeaders.contains(compact)) continue;
        sourceColumns.append(column);
        headers.append(header);
    }
    scheduleTable_->setColumnCount(headers.size());
    scheduleTable_->setHorizontalHeaderLabels(headers);
    const QVector<int>& activeOrder = optimizedOrder_;
    int rowCount = 0;
    for (int plateIndex : activeOrder) {
        if (plateIndex >= 0 && plateIndex < workbookData_.plates.size()) {
            rowCount += workbookData_.plates[plateIndex].workpieces.size();
        }
    }
    scheduleTable_->setRowCount(rowCount);
    int row = 0;
    for (int plateIndex : activeOrder) {
        if (plateIndex < 0 || plateIndex >= workbookData_.plates.size()) continue;
        const SteelPlateBlock& plate = workbookData_.plates[plateIndex];
        for (int itemIndex = 0; itemIndex < plate.workpieces.size(); ++itemIndex, ++row) {
            const WorkpieceProcessData& workpiece = plate.workpieces[itemIndex];
            const QString workpieceId = workpiece.workpieceId.trimmed().isEmpty()
                ? QStringLiteral("工件%1").arg(itemIndex + 1) : workpiece.workpieceId;
            QStringList values = {
                itemIndex == 0 ? plate.plateId : QString(),
                plate.material, plate.specification,
                numberText(plate.stockLength), QString::number(plate.workpieceCount),
                numberText(plate.usedLength), workpieceId
            };
            for (int sourceColumn : sourceColumns) {
                values.append(sourceColumn < workpiece.sourceValues.size()
                    ? workpiece.sourceValues[sourceColumn].toString().trimmed()
                    : QString());
            }
            for (int column = 0; column < values.size(); ++column) {
                QTableWidgetItem* item = new QTableWidgetItem(values[column]);
                if (column >= 3 && column <= 5) {
                    item->setTextAlignment(Qt::AlignCenter);
                }
                scheduleTable_->setItem(row, column, item);
            }
        }
    }
    scheduleTable_->resizeColumnsToContents();
    for (int column = 6; column < scheduleTable_->columnCount(); ++column) {
        if (scheduleTable_->columnWidth(column) > 360) {
            scheduleTable_->setColumnWidth(column, 360);
        }
    }
}

// 按甲方表格格式展示钢板明细、原始字段和各工序时间
void MainWindow::populateTimelineTable()
{
    timelineTable_->clearSpans();
    timelineTable_->clearContents();
    const QStringList headers = timelineTableHeaders();
    timelineTable_->setColumnCount(headers.size());
    timelineTable_->setHorizontalHeaderLabels(headers);

    const QVector<int>& activeOrder = optimizedOrder_;
    TimelineSummary timelineSummary;
    const QVector<TimelineEntry> entries = TimelineSimulator::simulate(
        workbookData_.plates, activeOrder, config_, &timelineSummary);
    if (entries.isEmpty()) {
        timelineTable_->setRowCount(1);
        timelineTable_->setSpan(0, 0, 1, timelineTable_->columnCount());
        const bool anyDiameterSelected = smallDiameterCheck_->isChecked() ||
            mediumDiameterCheck_->isChecked() || largeDiameterCheck_->isChecked();
        QTableWidgetItem* item = new QTableWidgetItem(anyDiameterSelected
            ? QStringLiteral("当前勾选的管径没有可排产工件。")
            : QStringLiteral("请至少勾选一种参与排产的管径。"));
        item->setTextAlignment(Qt::AlignCenter);
        item->setForeground(QColor(QStringLiteral("#666666")));
        timelineTable_->setItem(0, 0, item);
        timelineTable_->setRowHeight(0, 42);
        return;
    }

    timelineTable_->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) {
        const TimelineEntry& entry = entries[row];
        if (entry.plateIndex < 0 || entry.plateIndex >= workbookData_.plates.size()) continue;
        const SteelPlateBlock& plate = workbookData_.plates[entry.plateIndex];
        if (entry.workpieceIndex < 0 || entry.workpieceIndex >= plate.workpieces.size()) continue;
        const WorkpieceProcessData& workpiece = plate.workpieces[entry.workpieceIndex];
        QVector<QString> values(headers.size());

        // 同一张钢板的汇总字段只在首个工件行显示
        if (entry.workpieceIndex == 0) {
            const double residual = std::max(0.0, plate.stockLength - plate.usedLength);
            const double utilization = plate.stockLength > 0.0
                ? plate.usedLength * 100.0 / plate.stockLength : 0.0;
            values[0] = entry.plateNumber;
            values[1] = plate.specification;
            values[2] = plate.material;
            values[3] = QString::number(plate.workpieceCount);
            values[4] = numberText(plate.usedLength);
            values[5] = numberText(residual);
            values[6] = numberText(utilization) + QLatin1Char('%');
        }

        values[7] = sourceValueText(
            workbookData_, workpiece, {QStringLiteral("工件名称"), QStringLiteral("工件号"),
                                      QStringLiteral("管名")});
        if (values[7].isEmpty()) values[7] = entry.workpieceId;
        values[8] = sourceValueText(workbookData_, workpiece, {QStringLiteral("船名")});
        values[9] = sourceValueText(
            workbookData_, workpiece, {QStringLiteral("分段名"), QStringLiteral("分段名称")});
        values[10] = sourceValueText(workbookData_, workpiece, {QStringLiteral("管名")});
        values[11] = sourceValueText(workbookData_, workpiece, {QStringLiteral("需求日期")});
        values[12] = sourceValueText(workbookData_, workpiece, {QStringLiteral("规格")}, true);
        values[13] = sourceValueText(workbookData_, workpiece, {QStringLiteral("材质")}, true);
        values[14] = sourceValueText(workbookData_, workpiece, {QStringLiteral("加工系列")});
        values[15] = sourceValueText(workbookData_, workpiece, {QStringLiteral("区域")});
        values[16] = sourceValueText(
            workbookData_, workpiece, {QStringLiteral("长度"), QStringLiteral("长度(mm)")}, true);
        if (values[16].isEmpty()) values[16] = numberText(workpiece.length);
        values[17] = sourceValueText(workbookData_, workpiece, {QStringLiteral("坡口")});
        if (values[17].isEmpty() && workpiece.bevelCount > 0.0) {
            values[17] = numberText(workpiece.bevelCount);
        }
        values[18] = sourceValueText(
            workbookData_, workpiece, {QStringLiteral("线上装配信息(简写)"),
                                      QStringLiteral("线上装配信息（简写）")});
        values[19] = sourceValueText(workbookData_, workpiece, {QStringLiteral("搭焊")});
        if (values[19].isEmpty() && workpiece.lapWeldCount > 0.0) {
            values[19] = numberText(workpiece.lapWeldCount);
        }

        values[20] = timeText(entry.sprayArrival);
        values[21] = timeText(entry.sprayFinish);
        values[22] = numberText(entry.sprayDuration);
        values[23] = numberText(entry.sprayBlockedSeconds);
        values[24] = timeText(entry.cuttingArrival);
        values[25] = timeText(entry.cuttingFinish);
        values[26] = numberText(entry.cuttingDuration);
        values[27] = numberText(entry.cuttingBlockedSeconds);
        values[28] = timeText(entry.bevelArrival);
        values[29] = timeText(entry.bevelFinish);
        if (entry.bevelFinish >= 0.0) values[30] = numberText(entry.bevelDuration);
        values[31] = numberText(entry.bevelBlockedSeconds);
        values[32] = timeText(entry.tackArrival);
        values[33] = timeText(entry.tackFinish);
        if (entry.tackFinish >= 0.0) values[34] = numberText(entry.tackDuration);
        values[35] = numberText(entry.tackBlockedSeconds);

        // 人工接焊和机器接焊共用一组时间列并标记实际工位
        if (entry.weldFinish >= 0.0) {
            values[36] = entry.weldRoute.contains(QStringLiteral("机器"))
                ? QStringLiteral("机器接焊") : QStringLiteral("人工接焊");
            if (entry.station.endsWith(QLatin1Char('1'))) values[37] = QStringLiteral("1");
            if (entry.station.endsWith(QLatin1Char('2'))) values[37] = QStringLiteral("2");
            values[38] = timeText(entry.weldArrival);
            values[39] = timeText(entry.weldFinish);
            values[40] = numberText(entry.weldDuration);
        }
        values[41] = numberText(entry.weldBlockedSeconds);
        values[42] = timeText(entry.sortingArrival);
        values[43] = timeText(entry.sortingFinish);
        for (int column = 0; column < values.size(); ++column) {
            QTableWidgetItem* item = new QTableWidgetItem(values[column]);
            item->setTextAlignment(column <= 19
                ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
            timelineTable_->setItem(row, column, item);
        }
    }
    timelineTable_->resizeColumnsToContents();
    for (int column = 0; column < timelineTable_->columnCount(); ++column) {
        if (timelineTable_->columnWidth(column) > 260) {
            timelineTable_->setColumnWidth(column, 260);
        }
    }
    int blockedWorkpieceCount = 0;
    for (const TimelineEntry& entry : entries) {
        if (entry.blockedSeconds > 1e-7) ++blockedWorkpieceCount;
    }
    summaryLabel_->setText(calculationSummaryBase_ +
        QStringLiteral("｜当前排产 %1 张｜完工 %2｜堵塞工件 %3｜累计堵塞 %4 秒")
            .arg(activeOrder.size())
            .arg(timeText(timelineSummary.finishTime))
            .arg(blockedWorkpieceCount)
            .arg(numberText(timelineSummary.totalBufferBlockedSeconds)));
}

// 按照小于76、中径76至114和大于114判断钢板是否参与当前排产
bool MainWindow::isDiameterSelected(double width) const
{
    if (width < 76.0) return smallDiameterCheck_->isChecked();
    if (width <= 114.0) return mediumDiameterCheck_->isChecked();
    return largeDiameterCheck_->isChecked();
}

// 更新主界面顶部的关键参数摘要
void MainWindow::updateParameterSummary()
{
    if (!parameterSummaryLabel_) return;
    QStringList grouping;
    if (groupMaterialCheck_->isChecked()) grouping.append(QStringLiteral("材质"));
    if (groupSpecificationCheck_->isChecked()) grouping.append(QStringLiteral("规格"));
    if (grouping.isEmpty()) grouping.append(QStringLiteral("不分组"));
    parameterSummaryLabel_->setText(QStringLiteral("参数：钢板长度 %1 mm｜分组 %2｜切缝 %3 mm")
        .arg(numberText(defaultStockLengthSpin_->value()))
        .arg(grouping.join(QStringLiteral("/")))
        .arg(numberText(kerfSpin_->value())));
}

// 将调整前后的均衡指标写入内部日志
void MainWindow::appendMetrics(
    const QString& title,
    const BalanceMetrics& metrics,
    int plateCount)
{
    const int frontCount = plateCount / 2;
    const int backCount = plateCount - frontCount;
    logEdit_->appendPlainText(QStringLiteral(
        "%1：前半段(%2张) 坡口%3/人工焊%4/机器焊%5，后半段(%6张) 坡口%7/人工焊%8/机器焊%9；"
        "相邻高负荷 %10/%11/%12 对；最长连续高负荷 %13/%14/%15 张；"
        "连续%16张峰值 %17/%18/%19；目标值 %20。")
        .arg(title).arg(frontCount)
        .arg(numberText(metrics.frontBevel))
        .arg(numberText(metrics.frontManualWeld))
        .arg(numberText(metrics.frontRobotWeld))
        .arg(backCount)
        .arg(numberText(metrics.backBevel))
        .arg(numberText(metrics.backManualWeld))
        .arg(numberText(metrics.backRobotWeld))
        .arg(metrics.adjacentHighBevelPairs)
        .arg(metrics.adjacentHighManualWeldPairs)
        .arg(metrics.adjacentHighRobotWeldPairs)
        .arg(metrics.longestHighBevelRun)
        .arg(metrics.longestHighManualWeldRun)
        .arg(metrics.longestHighRobotWeldRun)
        .arg(config_.rollingWindow)
        .arg(numberText(metrics.rollingPeakBevel))
        .arg(numberText(metrics.rollingPeakManualWeld))
        .arg(numberText(metrics.rollingPeakRobotWeld))
        .arg(metrics.objective, 0, 'f', 4));
}

// 在程序目录和开发目录中查找默认参数文件
QString MainWindow::locateDefaultConfig() const
{
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/config/default.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../../config/default.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../config/default.json")
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}
