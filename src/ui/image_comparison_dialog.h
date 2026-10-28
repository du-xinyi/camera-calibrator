#pragma once

#include <QDialog>

class QImage;

/**
 * @brief 联动缩放和平移原图与去畸变图，支持并排和分割线对比
 */
class ImageComparisonDialog: public QDialog
{
public:

    /**
     * @brief 创建去畸变前后对比窗口
     *
     * @param original 未叠加角点的原始图片
     * @param undistorted 与原图尺寸相同的去畸变图片
     * @param parent 父窗口，允许为空
     * @details 空图片或不同尺寸的输入只显示错误提示，不允许进行误导性对比
     */
    ImageComparisonDialog(const QImage &original,
        const QImage &undistorted, QWidget *parent = nullptr);
};
