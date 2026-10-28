#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

namespace Theme
{
    void repolish(QWidget *root)
    {
        for (QWidget *widget: root->findChildren<QWidget *>())
        {
            if (widget->property("role").isValid())
            {
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
            }
        }
    }

    void apply(QApplication &app)
    {
        // Fusion 在各平台表现一致，样式表只需覆盖差异部分
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        palette.setColor(QPalette::Window, QColor(243, 245, 248));
        palette.setColor(QPalette::WindowText, kText);
        palette.setColor(QPalette::Base, kSurface);
        palette.setColor(QPalette::AlternateBase, QColor(248, 250, 252));
        palette.setColor(QPalette::Text, kText);
        palette.setColor(QPalette::Button, kSurface);
        palette.setColor(QPalette::ButtonText, kText);
        palette.setColor(QPalette::PlaceholderText, kMutedText);
        palette.setColor(QPalette::Highlight, kAccent);
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, kAccent);
        palette.setColor(QPalette::ToolTipBase, QColor(31, 41, 55));
        palette.setColor(QPalette::ToolTipText, QColor(248, 250, 252));
        palette.setColor(QPalette::Mid, kBorder);
        palette.setColor(QPalette::Dark, kCanvasDark);
        for (const auto role: {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        {
            palette.setColor(QPalette::Disabled, role, QColor(163, 172, 185));
        }
        app.setPalette(palette);

        QFile qss(QStringLiteral(":/theme.qss"));
        if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            app.setStyleSheet(QString::fromUtf8(qss.readAll()));
        }
    }
}
