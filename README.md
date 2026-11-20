# 钢板套料与流水线均衡工具

基于 Qt Widgets 和 QXlsx 的 Windows 桌面程序，用于从管件加工 Excel 数据生成小径钢板套料结果，优化整张钢板的上线顺序，并模拟喷码、切割、坡口、组队、外圈接焊和内圈接焊时序

## 当前处理范围

- 小径人工接焊默认范围为 `22–34 mm`
- 小径机器接焊默认范围为 `42–60 mm`
- 小径 `22–60 mm`
- 中径 `76–114 mm`
- 大径 `140–377 mm`
- 同一张钢板内的工件归属固定，顺序只在生成套料数据时按坡口、焊接需求交替整理
- 排产阶段只调整整张钢板，同一钢板组内允许交叉，不跨钢板组调整

## 输入与输出

程序自动识别两种输入

1. 原始工件表，先按材质和规格分组，再执行一维套料
2. 已有钢板明细，保留原钢板内工件及原始行数据，只优化钢板顺序

一次导出生成一个 Excel 工作簿，包含以下三张表

- `立库`，按材质和规格汇总钢板数量、工件数量、已用长度和平均利用率
- `排产`，按最终钢板顺序列出工件，并保留原始业务字段，搭焊、对焊和套管合并为数值型 `组队`
- `时序图`，列出各工件进入和完成喷码、切割、坡口、组队、外圈接焊、内圈接焊和分拣的时间

利用率口径如下

- 排产单板利用率 = 单板已用长度 ÷ 单板长度
- 立库分组平均利用率 = 分组已用长度合计 ÷ 分组钢板总长度

## 处理流程

1. 读取 Excel 和 `config/default.json`
2. 解析材质、规格、长度、坡口、搭焊、对焊和套管数据
3. 按照勾选过滤工件(小径,中径,大径)
4. 按材质、厚度和宽度等硬条件分组
5. 使用 Best Fit Decreasing 确定钢板数量
6. 在钢板数量不增加的前提下重新分配工件，兼顾长度利用率、工件数量、坡口、人工接焊和机器接焊负荷
7. 保持钢板内工件固定，按工序优先级和实际时序模拟结果优化钢板顺序
8. 生成立库、排产和时序图三张结果表

完整的计算规则见 [docs/排产规则.md](docs/排产规则.md)

## 默认节拍

- 整板喷码总时间 = `102 + 18 × 工件数` 秒，程序将固定动作分配到首件和末件
- 人工接焊单道时间为 `178` 秒，外圈和内圈各执行一次
- 机器接焊单道时间 = `109 + 宽度 × 3.14 ÷ 5` 秒，外圈和内圈各执行一次
- 组队时间为 `160` 秒
- 外圈接焊与内圈接焊之间设置容量为 `3` 的缓冲区，只有缓冲区已满且外圈工位无法释放工件的等待才计为堵塞时间

全部节拍均可在参数界面或 `config/default.json` 中调整

## 使用方法

1. 运行 `bin/x64/Release/SteelPlateSequenceOptimizer.exe`，或双击 `运行钢板套料与流水线均衡.cmd`
2. 选择输入 Excel
3. 检查套料、焊接路线和时序参数
4. 点击开始计算
5. 在立库、排产和时序图中检查结果
6. 点击导出结果 Excel，一次生成三张结果表

命令行调用

```text
SteelPlateSequenceOptimizer.exe --input 输入.xlsx --output 输出.xlsx --config config/default.json
```

返回码为 `0` 表示成功，失败时会在输出路径旁生成 `.error.txt`

## 编译环境

- Windows 10 或更高版本
- Visual Studio 2022，平台工具集 `v143`
- Qt 5.9.1，MSVC 2017 64 位套件
- C++17
- QXlsx 1.4.6

编译步骤

1. 使用 Visual Studio 打开 `SteelPlateSequenceOptimizer.sln`
2. 选择 `Release | x64`
3. 生成解决方案
4. 可执行文件输出到 `bin/x64/Release`

构建后项目会复制 QXlsx DLL、默认参数和 Qt 运行库到输出目录

## 项目目录

```text
SteelPlateSequenceOptimizer/
├─ config/                         默认参数
├─ docs/                           排产规则和项目说明
├─ licences/                       第三方许可
├─ src/                            自有 C++ 源码
├─ third_party/                    第三方头文件、库和 DLL
├─ SteelPlateSequenceOptimizer.sln Visual Studio 解决方案
└─ README.md                       项目入口说明
```

`bin`、`obj`、本地构建目录、Excel 数据和 Visual Studio 用户文件均由 `.gitignore` 排除，不提交到 GitHub

## 第三方依赖

项目随仓库保存 QXlsx 1.4.6 的头文件、64 位动态库和导入库，来源为 [QtExcel/QXlsx](https://github.com/QtExcel/QXlsx/tree/v1.4.6)，使用 MIT License，许可文本见 [licences/QXlsx-LICENSE.txt](licences/QXlsx-LICENSE.txt)
