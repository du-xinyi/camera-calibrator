#pragma once

#include "calibration.h"

#include <QString>
#include <QStringList>

#include <optional>

/**
 * @brief 可写入项目文件的完整标定会话快照
 */
struct CalibrationProject
{
    CalibrationOptions options; ///< 保存时的完整标定选项
    QStringList imagePaths; ///< 参与项目的图片路径
    QStringList disabledImagePaths; ///< 暂不参与标定的图片，必须属于 imagePaths
    std::optional<CalibrationResult> result; ///< 可选的完整标定结果或导入参数
    bool resultImported = false; ///< 结果是否来自外部参数文件
    QString resultWarning; ///< 加载时因输入变化而丢弃结果的说明，不写入项目
};

/**
 * @brief 负责标定项目 JSON 的版本校验、路径转换和原子保存
 */
class CalibrationProjectIo
{
public:

    /**
     * @brief 将项目序列化到 JSON 文件
     *
     * @details 保存 v3 会话、相对图片路径和图片指纹，目标文件仅在数据完整写入后提交
     *
     * @param filePath 目标项目文件
     * @param project 待序列化的项目快照
     * @param error 接收失败原因，可为 nullptr
     *
     * @return 成功校验并提交文件时返回 true
     */
    static bool save(const QString &filePath,
        const CalibrationProject &project,
        QString *error = nullptr);

    /**
     * @brief 从 JSON 文件恢复标定项目
     *
     * @details 仅读取 v3 会话并解析相对路径。图片变化或缺失时仅丢弃结果，
     * 通过 resultWarning 提示
     *
     * @param filePath 待读取的项目文件
     * @param error 接收失败原因，可为 nullptr
     *
     * @return 有效项目；文件格式或参数无效时返回 std::nullopt
     */
    static std::optional<CalibrationProject> load(
        const QString &filePath, QString *error = nullptr);
};
