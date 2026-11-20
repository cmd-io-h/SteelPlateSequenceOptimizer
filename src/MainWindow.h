#pragma once

#include "OptimizerConfig.h"
#include "SteelPlateModel.h"

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

// 负责参数交互、执行计算并展示立库、排产和时序图
class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi(); // 构建主窗口界面
    void buildParameterDialog(); // 构建参数设置对话框
    void openParameterDialog(); // 打开参数设置对话框
    void browseInput(); // 选择输入Excel文件
    void browseConfig(); // 选择参数配置文件
    void loadConfig(); // 加载参数配置文件
    void saveConfig(); // 保存当前参数配置
    bool analyzeAndOptimize(); // 读取工件数据并执行套料排产
    void rebuildActiveSchedule(); // 按当前管径选择重新生成排产结果
    void exportResult(); // 导出立库排产和时序图表格
    void applyConfigToUi(const OptimizerConfig& config); // 将配置参数显示到界面控件
    OptimizerConfig configFromUi() const; // 从界面控件读取配置参数
    void populatePreview(); // 刷新全部结果预览表格
    void populateWarehouseTable(); // 填充立库汇总表格
    void applyWarehouseFilters(); // 按筛选条件显示立库数据
    void populateScheduleTable(); // 填充工件排产表格
    void populateTimelineTable(); // 填充各工序时序表格
    bool isSmallDiameter(double width) const; // 判断工件是否属于当前支持的小径范围
    void updateParameterSummary(); // 更新主界面的参数摘要
    void appendMetrics(const QString& title, const BalanceMetrics& metrics, int plateCount); // 将均衡指标写入日志
    QString locateDefaultConfig() const; // 查找默认参数配置文件

    // 文件和参数设置控件
    QLineEdit* inputEdit_ = nullptr;
    QLineEdit* configEdit_ = nullptr;
    QDialog* parameterDialog_ = nullptr;
    QComboBox* inputModeCombo_ = nullptr;
    QCheckBox* groupMaterialCheck_ = nullptr;
    QCheckBox* groupSpecificationCheck_ = nullptr;
    QDoubleSpinBox* defaultStockLengthSpin_ = nullptr;
    QDoubleSpinBox* kerfSpin_ = nullptr;
    QDoubleSpinBox* endAllowanceSpin_ = nullptr;
    QLineEdit* lengthThresholdsEdit_ = nullptr;
    QLineEdit* manualWidthsEdit_ = nullptr;
    QLineEdit* robotWidthsEdit_ = nullptr;
    QLineEdit* mediumWidthsEdit_ = nullptr;
    QLineEdit* largeWidthsEdit_ = nullptr;
    QTableWidget* objectiveTable_ = nullptr;
    QDoubleSpinBox* downstreamMinimumLengthSpin_ = nullptr;
    QSpinBox* bufferCapacitySpin_ = nullptr;
    QDoubleSpinBox* sprayIncrementSpin_ = nullptr;
    QDoubleSpinBox* sawFeedSpeedSpin_ = nullptr;
    QDoubleSpinBox* sawFirstFeedSpin_ = nullptr;
    QDoubleSpinBox* sawMaximumFeedSpin_ = nullptr;
    QDoubleSpinBox* sawClampSpin_ = nullptr;
    QDoubleSpinBox* sawReleaseSpin_ = nullptr;
    QDoubleSpinBox* sawUnloadSpin_ = nullptr;
    QDoubleSpinBox* sawPlateResetSpin_ = nullptr;
    QDoubleSpinBox* sawCutUnder35Spin_ = nullptr;
    QDoubleSpinBox* sawCutUnder61Spin_ = nullptr;
    QDoubleSpinBox* sawCutUnder90Spin_ = nullptr;
    QDoubleSpinBox* sawCutOtherSpin_ = nullptr;
    QSpinBox* bevelStationCountSpin_ = nullptr;
    QDoubleSpinBox* bevelCycleSpin_ = nullptr;
    QDoubleSpinBox* tackCycleSpin_ = nullptr;
    QDoubleSpinBox* manualWeldCycleSpin_ = nullptr;
    QDoubleSpinBox* robotWeldFixedSpin_ = nullptr;
    QDoubleSpinBox* robotWeldSpeedSpin_ = nullptr;
    // 结果页面和筛选控件
    QPushButton* exportButton_ = nullptr;
    QCheckBox* smallDiameterCheck_ = nullptr;
    QCheckBox* mediumDiameterCheck_ = nullptr;
    QCheckBox* largeDiameterCheck_ = nullptr;
    QTabWidget* resultTabs_ = nullptr;
    QTableWidget* warehouseTable_ = nullptr;
    QLineEdit* warehouseSearchEdit_ = nullptr;
    QComboBox* warehouseMaterialFilter_ = nullptr;
    QComboBox* warehouseThicknessFilter_ = nullptr;
    QComboBox* warehouseWidthFilter_ = nullptr;
    QTableWidget* scheduleTable_ = nullptr;
    QTableWidget* timelineTable_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* parameterSummaryLabel_ = nullptr;

    // 当前配置、工作簿数据和优化后的钢板索引顺序
    OptimizerConfig config_;
    WorkbookData sourceWorkbookData_;
    WorkbookData workbookData_;
    QVector<int> optimizedOrder_;
    QString calculationSummaryBase_;
};
