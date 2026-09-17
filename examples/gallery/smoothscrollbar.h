#pragma once

#include <qscrollbar.h>
#include <qtimer.h>

class SmoothScrollbar : public QScrollBar {
public:
    explicit SmoothScrollbar(QWidget* parent = nullptr);

protected:
    void wheelEvent(QWheelEvent* e) override;

private:
    QTimer* smoothMoveTimer;
    QPointF lastPosition;
    QPointF lastGlobalPosition;
    Qt::MouseButtons lastButtons;
    Qt::KeyboardModifiers lastModifiers;
    Qt::ScrollPhase lastPhase = Qt::NoScrollPhase;
    bool lastInverted = false;

    int stepsTotal;
    QList<QPair<QPoint, int>> stepsLeftQueue;

private:
    QPointF subDelta(QPoint delta, int stepsLeft) const;

private slots:
    void slotSmoothMove();
};
