#ifndef QVCHECKLISTDELEGATE_H
#define QVCHECKLISTDELEGATE_H

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

// macOS draws item-view checkboxes outside the row, so only the first one stays
// visible. This delegate paints the box itself and handles clicks on it, which
// keeps file lists with per-item check states reliable on every platform.
class QVCheckListDelegate : public QStyledItemDelegate
{
public:
    explicit QVCheckListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const bool selected = opt.state & QStyle::State_Selected;
        const QRect checkRect = indicatorRect(opt);
        const auto state = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());

        painter->save();
        painter->setClipRect(opt.rect);
        if (selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else if (opt.state & QStyle::State_MouseOver)
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Mid));

        const QColor accent(10, 132, 255);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(state == Qt::Checked ? accent : QColor(160, 160, 160), 1.4));
        painter->setBrush(state == Qt::Checked ? QBrush(accent) : QBrush(Qt::NoBrush));
        painter->drawRoundedRect(QRectF(checkRect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (state == Qt::Checked) {
            QPen mark(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(mark);
            const QPointF start(checkRect.left() + 3.5, checkRect.center().y() + 0.5);
            const QPointF mid(checkRect.left() + 6.5, checkRect.bottom() - 4.0);
            const QPointF end(checkRect.right() - 3.0, checkRect.top() + 4.5);
            painter->drawLine(start, mid);
            painter->drawLine(mid, end);
        }

        const QRect textRect(checkRect.right() + 8, opt.rect.top(),
                             qMax(0, opt.rect.right() - 8 - (checkRect.right() + 8)),
                             opt.rect.height());
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setFont(opt.font);
        painter->setPen(selected ? opt.palette.color(QPalette::HighlightedText)
                                 : opt.palette.color(QPalette::Text));
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          opt.fontMetrics.elidedText(opt.text, Qt::ElideMiddle, textRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const int box = style->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, widget);
        hint.setHeight(qMax(hint.height(), box + 8));
        return hint;
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<const QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                QStyleOptionViewItem opt = option;
                initStyleOption(&opt, index);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint pos = mouse->position().toPoint();
#else
                const QPoint pos = mouse->pos();
#endif
                if (indicatorRect(opt).contains(pos)) {
                    const auto state =
                            static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
                    model->setData(index, state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                   Qt::CheckStateRole);
                    return true;
                }
            }
            return false;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    static QRect indicatorRect(const QStyleOptionViewItem &option)
    {
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const int box = qMax(16, style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, widget));
        return QRect(option.rect.left() + 8, option.rect.top() + (option.rect.height() - box) / 2,
                     box, box);
    }
};

#endif // QVCHECKLISTDELEGATE_H
