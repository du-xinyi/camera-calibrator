#pragma once

#include "calibration.h"

#include <QDialog>

#include <vector>

/**
 * @brief 以表格汇总多个标定候选的求解状态、误差和核心内参
 */
class AlgorithmComparisonDialog: public QDialog
{
    Q_OBJECT

public:

    /**
     * @brief 构造标定算法对比窗口
     *
     * @param results 候选结果；成功候选按 RMS 排序，失败项后置
     * @param parent 所属窗口，允许为空
     */
    explicit AlgorithmComparisonDialog(
        std::vector<CalibrationResult> results,
        QWidget *parent = nullptr);

signals:

    /**
     * @brief 用户确认应用一个成功的候选结果
     *
     * @param result 已选候选；接收方应在对话框销毁前保存需要的数据
     */
    void resultSelected(const CalibrationResult &result);

private:

    std::vector<CalibrationResult> results_; ///< 与表格行顺序一致的候选结果
};
