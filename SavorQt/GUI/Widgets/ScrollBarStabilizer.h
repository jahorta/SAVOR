#pragma once

#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QScrollBar>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <algorithm>

struct ScrollBarSnapshot
{
    int value = 0;
    bool wasAtMaximum = false;
};

inline ScrollBarSnapshot captureScrollBarSnapshot(QScrollBar* scrollBar)
{
    if (!scrollBar) {
        return {};
    }

    return ScrollBarSnapshot{
        scrollBar->value(),
        scrollBar->value() >= scrollBar->maximum()
    };
}

inline void restoreScrollBarSnapshot(QScrollBar* scrollBar, const ScrollBarSnapshot& snapshot)
{
    if (!scrollBar) {
        return;
    }

    if (snapshot.wasAtMaximum) {
        scrollBar->setValue(scrollBar->maximum());
        return;
    }

    scrollBar->setValue(std::clamp(snapshot.value, scrollBar->minimum(), scrollBar->maximum()));
}

struct ItemViewScrollSnapshot
{
    ScrollBarSnapshot vertical;
    ScrollBarSnapshot horizontal;
};

inline ItemViewScrollSnapshot captureItemViewScrollSnapshot(const QAbstractItemView* view)
{
    if (!view) {
        return {};
    }

    return ItemViewScrollSnapshot{
        captureScrollBarSnapshot(view->verticalScrollBar()),
        captureScrollBarSnapshot(view->horizontalScrollBar())
    };
}

inline void restoreItemViewScrollSnapshot(QAbstractItemView* view, const ItemViewScrollSnapshot& snapshot)
{
    if (!view) {
        return;
    }

    restoreScrollBarSnapshot(view->verticalScrollBar(), snapshot.vertical);
    restoreScrollBarSnapshot(view->horizontalScrollBar(), snapshot.horizontal);

    QPointer<QAbstractItemView> viewPtr(view);
    QTimer::singleShot(0, view, [viewPtr, snapshot]() {
        if (viewPtr.isNull()) {
            return;
        }
        restoreScrollBarSnapshot(viewPtr->verticalScrollBar(), snapshot.vertical);
        restoreScrollBarSnapshot(viewPtr->horizontalScrollBar(), snapshot.horizontal);
    });
}

using ScrollAreaScrollSnapshot = ItemViewScrollSnapshot;

inline ScrollAreaScrollSnapshot captureScrollAreaScrollSnapshot(const QAbstractScrollArea* scrollArea)
{
    if (!scrollArea) {
        return {};
    }

    return ScrollAreaScrollSnapshot{
        captureScrollBarSnapshot(scrollArea->verticalScrollBar()),
        captureScrollBarSnapshot(scrollArea->horizontalScrollBar())
    };
}

inline void restoreScrollAreaScrollSnapshot(QAbstractScrollArea* scrollArea, const ScrollAreaScrollSnapshot& snapshot)
{
    if (!scrollArea) {
        return;
    }

    restoreScrollBarSnapshot(scrollArea->verticalScrollBar(), snapshot.vertical);
    restoreScrollBarSnapshot(scrollArea->horizontalScrollBar(), snapshot.horizontal);

    QPointer<QAbstractScrollArea> scrollAreaPtr(scrollArea);
    QTimer::singleShot(0, scrollArea, [scrollAreaPtr, snapshot]() {
        if (scrollAreaPtr.isNull()) {
            return;
        }
        restoreScrollBarSnapshot(scrollAreaPtr->verticalScrollBar(), snapshot.vertical);
        restoreScrollBarSnapshot(scrollAreaPtr->horizontalScrollBar(), snapshot.horizontal);
    });
}
