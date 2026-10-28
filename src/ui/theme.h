#pragma once

#include <QColor>

class QApplication;
class QWidget;

/**
 * @brief 应用统一配色，自绘画布与 theme.qss 共用同一组颜色
 */
namespace Theme
{
    inline const QColor kAccent{37, 99, 235}; ///< 主色，用于选中、强调与主按钮
    inline const QColor kText{31, 41, 55}; ///< 正文文字
    inline const QColor kMutedText{107, 118, 133}; ///< 次要说明文字
    inline const QColor kBorder{227, 231, 237}; ///< 面板与分割线
    inline const QColor kSurface{255, 255, 255}; ///< 卡片与面板背景
    inline const QColor kSurfaceAlt{246, 248, 251}; ///< 浅色画布背景
    inline const QColor kCanvasDark{30, 34, 41}; ///< 图片预览深色背景
    inline const QColor kSuccess{21, 128, 61}; ///< 检测成功
    inline const QColor kWarning{180, 83, 9}; ///< 质量提示文字
    inline const QColor kWarningBackground{255, 244, 220}; ///< 质量提示底色
    inline const QColor kDanger{185, 28, 28}; ///< 检测失败
    inline const QColor kDangerBackground{254, 232, 232}; ///< 高误差行底色

    /**
     * @brief 为应用设置 Fusion 风格、调色板与全局样式表
     */
    void apply(QApplication &app);

    /**
     * @brief 重新应用样式，使 uic 在构造后写入的 role 等动态属性参与选择器匹配
     */
    void repolish(QWidget *root);
}
