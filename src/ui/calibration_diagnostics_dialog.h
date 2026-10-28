#pragma once

#include "calibration.h"

#include <QDialog>
#include <QString>

/**
 * @brief 显示标定角点的画面覆盖分布与逐点重投影偏差
 */
class CalibrationDiagnosticsDialog final: public QDialog
{
public:

    /**
     * @brief 创建包含覆盖热力图和误差叠加的诊断窗口
     *
     * @param result 含原图角点及对应重投影点的标定结果
     * @param selectedImagePath 误差页优先显示的图片路径，缺省时显示首张图片
     * @param parent Qt 父窗口，允许为空
     */
    explicit CalibrationDiagnosticsDialog(const CalibrationResult &result,
        const QString &selectedImagePath = {}, QWidget *parent = nullptr);

private:

    CalibrationResult result_; ///< 窗口独立持有结果，避免调用方更新结果后引用失效
};
