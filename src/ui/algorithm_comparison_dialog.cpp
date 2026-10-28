#include "algorithm_comparison_dialog.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace
{
    QString modelName(CameraModel model)
    {
        switch (model)
        {
            case CameraModel::Pinhole: return QStringLiteral("Pinhole");
            case CameraModel::Fisheye: return QStringLiteral("Fisheye");
        }
        return QStringLiteral("--");
    }

    QString detectorName(const CalibrationOptions &options)
    {
        if (options.boardType == CalibrationBoardType::Charuco)
        {
            return QStringLiteral("ChArUco");
        }
        return options.method == CalibrationMethod::SectorBased ?
                QObject::tr("扇区法") :
                QObject::tr("传统方法");
    }

    QTableWidgetItem *numberItem(double value, int precision)
    {
        auto *item = new QTableWidgetItem(
            std::isfinite(value) ? QString::number(value, 'f', precision) : QStringLiteral("--"));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    }

}

AlgorithmComparisonDialog::AlgorithmComparisonDialog(
    std::vector<CalibrationResult> results, QWidget *parent)
    : QDialog(parent),
      results_(std::move(results))
{
    setObjectName(QStringLiteral("AlgorithmComparisonDialog"));
    setWindowTitle(tr("算法结果对比"));
    resize(900, 360);

    // 成功候选按 RMS 排序，失败项统一后置。
    std::stable_sort(results_.begin(), results_.end(),
        [](const CalibrationResult &lhs,
    const CalibrationResult &rhs)
        {
            if (lhs.success != rhs.success)
            {
                return lhs.success;
            }
            if (!lhs.success)
            {
                return false;
            }
            const double left = lhs.rmsError;
            const double right = rhs.rmsError;
            return (std::isfinite(left) ? left : std::numeric_limits<double>::infinity())
                    < (std::isfinite(right) ? right : std::numeric_limits<double>::infinity());
        });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);
    auto *hint = new QLabel(
        tr("RMS 较低只表示当前图片的拟合误差较小；选择模型还应结合有效图片、角点、去畸变效果、视场与参数稳定性。"),
        this);
    hint->setObjectName(QStringLiteral("algorithmComparisonHint"));
    hint->setWordWrap(true);
    hint->setProperty("role", QStringLiteral("hint"));
    layout->addWidget(hint);

    auto *table = new QTableWidget(static_cast<int>(results_.size()), 9, this);
    table->setObjectName(QStringLiteral("algorithmComparisonTable"));
    table->setHorizontalHeaderLabels(
        {
            tr("相机模型"), tr("检测器"), tr("状态"), tr("RMS (px)"),
            tr("有效图片"), QStringLiteral("fx"), QStringLiteral("fy"),
            QStringLiteral("cx"), QStringLiteral("cy")
        });
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(30);
    for (int row = 0; row < static_cast<int>(results_.size()); ++row)
    {
        const CalibrationResult &result = results_[static_cast<size_t>(row)];
        table->setItem(row, 0,
            new QTableWidgetItem(modelName(result.options.cameraModel)));
        table->setItem(row, 1,
            new QTableWidgetItem(detectorName(result.options)));
        table->setItem(row, 2, new QTableWidgetItem(
            result.success ? tr("成功") : tr("失败")));
        table->setItem(row, 3, numberItem(result.success ? result.rmsError : NAN, 4));
        table->setItem(row, 4, new QTableWidgetItem(
            tr("%1 / %2")
            .arg(result.imagesUsed)
            .arg(result.imagesTotal)));
        for (int col = 5; col < 9; ++col)
        {
            table->setItem(row, col, numberItem(NAN, 2));
        }
        if (result.success && result.cameraMatrix.rows == 3
            && result.cameraMatrix.cols == 3)
        {
            table->setItem(row, 5,
                numberItem(result.cameraMatrix.at<double>(0, 0), 2));
            table->setItem(row, 6,
                numberItem(result.cameraMatrix.at<double>(1, 1), 2));
            table->setItem(row, 7,
                numberItem(result.cameraMatrix.at<double>(0, 2), 2));
            table->setItem(row, 8,
                numberItem(result.cameraMatrix.at<double>(1, 2), 2));
        }
    }
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *applyButton = buttons->addButton(
        tr("应用选中结果"), QDialogButtonBox::ActionRole);
    applyButton->setObjectName(QStringLiteral("applyComparisonResultButton"));
    applyButton->setEnabled(false);
    applyButton->setProperty("role", QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    const auto selectedSuccessfulRow = [this, table]
    {
        const int row = table->currentRow();
        return row >= 0 && row < static_cast<int>(results_.size())
                && !table->selectedItems().isEmpty()
                && results_[static_cast<size_t>(row)].success;
    };
    connect(table, &QTableWidget::itemSelectionChanged, this,
        [applyButton, selectedSuccessfulRow]
        {
            applyButton->setEnabled(selectedSuccessfulRow());
        });
    connect(applyButton, &QPushButton::clicked, this,
        [this, table, selectedSuccessfulRow]
        {
            if (!selectedSuccessfulRow())
            {
                return;
            }
            emit resultSelected(results_[static_cast<size_t>(table->currentRow())]);
            accept();
        });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    if (!results_.empty())
    {
        table->selectRow(0);
    }
}
