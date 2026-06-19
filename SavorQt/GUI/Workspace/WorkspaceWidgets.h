#pragma once

#include <QtCore/QSettings>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPixmap>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <functional>
#include <utility>

namespace savorqt::gui {

namespace {

QIcon createDrawerCloseIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(255, 255, 255), 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(6, 6, 18, 18);
    painter.drawLine(18, 6, 6, 18);
    return QIcon(pixmap);
}

} // namespace

struct UiEntityRef {
    QString workspace;
    QString kind;
    qint64 id = 0;
    QString key;
};

enum class ContextDrawerMode {
    Collapsed,
    Peek,
    Expanded,
    Focused,
};

class WorkspaceSelectorBar final : public QFrame
{
public:
    explicit WorkspaceSelectorBar(QWidget* parent = nullptr)
        : QFrame(parent)
        , labels_{ QStringLiteral("Setup"), QStringLiteral("Running"), QStringLiteral("Analysis") }
    {
        setObjectName("workspaceSelectorBar");
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(8);
        layout->addStretch();

        for (int i = 0; i < labels_.size(); ++i) {
            auto* button = new QPushButton(this);
            button->setCheckable(true);
            button->setMinimumWidth(150);
            button->setObjectName("workspaceSelectorButton");
            buttons_.push_back(button);
            badges_.push_back(QString());
            layout->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, i]() {
                setCurrentIndex(i);
                if (selectionChanged_) {
                    selectionChanged_(i);
                }
            });
        }
        layout->addStretch();
        refreshButtonText();
        setCurrentIndex(0);
    }

    void setSelectionChangedCallback(std::function<void(int)> callback)
    {
        selectionChanged_ = std::move(callback);
    }

    void setBadge(int index, const QString& badge)
    {
        if (index < 0 || index >= badges_.size() || badges_[index] == badge) {
            return;
        }
        badges_[index] = badge;
        refreshButtonText();
    }

    void setCurrentIndex(int index)
    {
        if (index < 0 || index >= buttons_.size()) {
            return;
        }
        currentIndex_ = index;
        for (int i = 0; i < buttons_.size(); ++i) {
            buttons_[i]->setChecked(i == currentIndex_);
            buttons_[i]->setProperty("workspaceActive", i == currentIndex_);
            buttons_[i]->style()->unpolish(buttons_[i]);
            buttons_[i]->style()->polish(buttons_[i]);
        }
    }

private:
    void refreshButtonText()
    {
        for (int i = 0; i < buttons_.size(); ++i) {
            const QString suffix = badges_[i].isEmpty() ? QString() : QStringLiteral("  %1").arg(badges_[i]);
            buttons_[i]->setText(labels_[i] + suffix);
        }
    }

    QVector<QPushButton*> buttons_;
    QStringList labels_;
    QVector<QString> badges_;
    int currentIndex_ = 0;
    std::function<void(int)> selectionChanged_;
};

class DrawerResizeHandle final : public QFrame
{
public:
    explicit DrawerResizeHandle(QWidget* parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("workspaceDrawerResizeHandle");
        setCursor(Qt::SplitHCursor);
        setFixedWidth(12);
    }

    void setCallbacks(
        std::function<int()> currentWidth,
        std::function<void(int, const QPoint&)> widthChanged,
        std::function<void()> dragFinished)
    {
        currentWidth_ = std::move(currentWidth);
        widthChanged_ = std::move(widthChanged);
        dragFinished_ = std::move(dragFinished);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event == nullptr || event->button() != Qt::LeftButton) {
            QFrame::mousePressEvent(event);
            return;
        }
        dragging_ = true;
        dragStartX_ = event->globalPos().x();
        dragStartWidth_ = currentWidth_ ? currentWidth_() : width();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!dragging_ || event == nullptr) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        const int deltaX = event->globalPos().x() - dragStartX_;
        if (widthChanged_) {
            widthChanged_(dragStartWidth_ - deltaX, event->globalPos());
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event != nullptr && event->button() == Qt::LeftButton) {
            dragging_ = false;
            if (dragFinished_) {
                dragFinished_();
            }
            event->accept();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    bool dragging_ = false;
    int dragStartX_ = 0;
    int dragStartWidth_ = 0;
    std::function<int()> currentWidth_;
    std::function<void(int, const QPoint&)> widthChanged_;
    std::function<void()> dragFinished_;
};

class WorkspacePageShell : public QWidget
{
public:
    explicit WorkspacePageShell(const QString& workspaceKey, const QString& title, const QString& description, QWidget* parent = nullptr)
        : QWidget(parent)
        , workspaceKey_(workspaceKey)
    {
        (void)title;
        (void)description;
        setObjectName("workspacePageShell");
        setFocusPolicy(Qt::StrongFocus);
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(16, 10, 16, 10);
        rootLayout->setSpacing(10);

        auto* body = new QFrame(this);
        body->setObjectName("workspaceBody");
        body_ = body;
        auto* bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(10);

        scrollArea_ = new QScrollArea(body);
        scrollArea_->setWidgetResizable(true);
        scrollArea_->setFrameShape(QFrame::NoFrame);
        canvas_ = new QWidget(scrollArea_);
        canvasLayout_ = new QVBoxLayout(canvas_);
        canvasLayout_->setContentsMargins(0, 0, 0, 0);
        canvasLayout_->setSpacing(10);
        scrollArea_->setWidget(canvas_);
        bodyLayout->addWidget(scrollArea_, 1);

        drawer_ = new QFrame(body);
        drawer_->setObjectName("workspaceContextDrawer");
        drawer_->raise();

        drawerHandle_ = new DrawerResizeHandle(body);
        drawerHandle_->setCallbacks(
            [this]() { return drawerWidth_; },
            [this](int requestedWidth, const QPoint& globalPointerPos) {
                if (body_ == nullptr) {
                    return;
                }
                const int localPointerX = body_->mapFromGlobal(globalPointerPos).x();
                if (drawerMode_ == ContextDrawerMode::Collapsed) {
                    if (localPointerX <= body_->width() - 20) {
                        drawerWidth_ = kMinDrawerWidth;
                        setDrawerMode(ContextDrawerMode::Peek);
                    }
                    return;
                }
                if (localPointerX >= body_->width() - 10) {
                    collapseDrawerFromDrag();
                    return;
                }
                setDrawerWidth(requestedWidth);
                persistDrawerState();
            },
            [this]() {
                if (drawerMode_ == ContextDrawerMode::Collapsed && drawerHandle_ != nullptr) {
                    drawerHandle_->hide();
                }
            });
        drawerHandle_->raise();

        auto* drawerLayout = new QVBoxLayout(drawer_);
        drawerLayout->setContentsMargins(14, 14, 14, 14);
        drawerLayout->setSpacing(10);

        auto* drawerHeader = new QHBoxLayout();
        drawerHeader->setContentsMargins(0, 0, 0, 0);
        drawerHeader->setSpacing(8);
        drawerTitle_ = new QLabel(drawer_);
        drawerTitle_->setObjectName("panelTitle");
        closeDrawerButton_ = new QPushButton(drawer_);
        closeDrawerButton_->setObjectName("drawerCloseButton");
        closeDrawerButton_->setIcon(createDrawerCloseIcon());
        closeDrawerButton_->setIconSize(QSize(24, 24));
        closeDrawerButton_->setToolTip(QStringLiteral("Close"));
        closeDrawerButton_->setFixedSize(32, 32);
        drawerHeader->addWidget(drawerTitle_, 1);
        drawerHeader->addWidget(closeDrawerButton_);
        drawerLayout->addLayout(drawerHeader);

        auto* drawerScroll = new QScrollArea(drawer_);
        drawerScroll->setWidgetResizable(true);
        drawerScroll->setFrameShape(QFrame::NoFrame);
        drawerScrollContent_ = new QWidget(drawerScroll);
        drawerContentLayout_ = new QVBoxLayout(drawerScrollContent_);
        drawerContentLayout_->setContentsMargins(0, 0, 0, 0);
        drawerContentLayout_->setSpacing(0);
        drawerBody_ = new QLabel(drawerScrollContent_);
        drawerBody_->setObjectName("sectionDescription");
        drawerBody_->setWordWrap(true);
        drawerBody_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        drawerContentLayout_->addWidget(drawerBody_);
        drawerContentLayout_->addStretch();
        drawerScroll->setWidget(drawerScrollContent_);
        drawerActions_ = new QGridLayout();
        drawerActions_->setSpacing(8);
        drawerActions_->setColumnStretch(0, 1);
        drawerActions_->setColumnStretch(1, 1);

        connect(closeDrawerButton_, &QPushButton::clicked, this, [this]() { closeDrawer(true); });

        drawerLayout->addWidget(drawerScroll, 1);
        drawerLayout->addLayout(drawerActions_);

        rootLayout->addWidget(body, 1);
        loadDrawerState();
        setDrawerMode(ContextDrawerMode::Collapsed);
    }

    QVBoxLayout* canvasLayout() const { return canvasLayout_; }

    void setPageVerticalScrollBarPolicy(Qt::ScrollBarPolicy policy)
    {
        if (scrollArea_ != nullptr) {
            scrollArea_->setVerticalScrollBarPolicy(policy);
        }
    }

    void setContext(
        const UiEntityRef& entity,
        const QString& title,
        const QString& body,
        const QVector<std::pair<QString, std::function<void()>>>& actions,
        ContextDrawerMode preferredMode = ContextDrawerMode::Expanded)
    {
        currentEntity_ = entity;
        drawerTitle_->setText(title);
        clearContextWidget();
        drawerBody_->show();
        drawerBody_->setText(body);
        setDrawerActions(actions);
        setDrawerMode(preferredMode);
    }

    void setContextWidget(
        const UiEntityRef& entity,
        const QString& title,
        QWidget* bodyWidget,
        const QVector<std::pair<QString, std::function<void()>>>& actions,
        ContextDrawerMode preferredMode = ContextDrawerMode::Expanded)
    {
        currentEntity_ = entity;
        drawerTitle_->setText(title);
        drawerBody_->hide();
        if (contextWidget_ != bodyWidget) {
            clearContextWidget();
            contextWidget_ = bodyWidget;
            if (contextWidget_ != nullptr) {
                contextWidget_->setParent(drawerScrollContent_);
                contextWidget_->show();
                const int insertIndex = std::max(0, drawerContentLayout_->count() - 1);
                drawerContentLayout_->insertWidget(insertIndex, contextWidget_, 1);
            }
        } else if (contextWidget_ != nullptr) {
            contextWidget_->show();
        }
        setDrawerActions(actions);
        setDrawerMode(preferredMode);
    }

    bool isContextDrawerOpen() const
    {
        return drawerMode_ != ContextDrawerMode::Collapsed;
    }

    void closeDrawer(bool force = false)
    {
        (void)force;
        setDrawerMode(ContextDrawerMode::Collapsed);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event != nullptr && event->key() == Qt::Key_Escape) {
            closeDrawer(false);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        updateDrawerGeometry();
    }

private:
    void clearContextWidget()
    {
        if (contextWidget_ == nullptr || drawerContentLayout_ == nullptr) {
            return;
        }
        drawerContentLayout_->removeWidget(contextWidget_);
        contextWidget_->hide();
        contextWidget_->setParent(nullptr);
        contextWidget_ = nullptr;
    }

    void setDrawerActions(const QVector<std::pair<QString, std::function<void()>>>& actions)
    {
        while (QLayoutItem* item = drawerActions_->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
        for (int i = 0; i < actions.size(); ++i) {
            const auto& action = actions[i];
            auto* button = new QPushButton(action.first, drawer_);
            button->setObjectName("jobsPrimaryButton");
            connect(button, &QPushButton::clicked, drawer_, [callback = action.second]() {
                if (callback) {
                    callback();
                }
            });
            if (i == actions.size() - 1 && actions.size() % 2 != 0) {
                drawerActions_->addWidget(button, i / 2, 0, 1, 2);
            } else {
                drawerActions_->addWidget(button, i / 2, i % 2);
            }
        }
    }

    void setDrawerMode(ContextDrawerMode mode)
    {
        drawerMode_ = mode;
        switch (mode) {
        case ContextDrawerMode::Collapsed:
            drawer_->hide();
            if (drawerHandle_ != nullptr) {
                drawerHandle_->hide();
            }
            break;
        case ContextDrawerMode::Peek:
            updateDrawerGeometry();
            drawer_->show();
            drawer_->raise();
            if (drawerHandle_ != nullptr) {
                drawerHandle_->show();
                drawerHandle_->raise();
            }
            lastOpenMode_ = mode;
            break;
        case ContextDrawerMode::Expanded:
            updateDrawerGeometry();
            drawer_->show();
            drawer_->raise();
            if (drawerHandle_ != nullptr) {
                drawerHandle_->show();
                drawerHandle_->raise();
            }
            lastOpenMode_ = mode;
            break;
        case ContextDrawerMode::Focused:
            updateDrawerGeometry();
            drawer_->show();
            drawer_->raise();
            if (drawerHandle_ != nullptr) {
                drawerHandle_->show();
                drawerHandle_->raise();
            }
            lastOpenMode_ = mode;
            break;
        }
        persistDrawerState();
    }

    void setDrawerWidth(int requestedWidth)
    {
        if (body_ == nullptr) {
            drawerWidth_ = requestedWidth;
            return;
        }
        constexpr int kDrawerMargin = 10;
        drawerWidth_ = std::max(
            kMinDrawerWidth,
            std::min(requestedWidth, std::max(kMinDrawerWidth, body_->width() - (2 * kDrawerMargin))));
        updateDrawerGeometry();
    }

    void updateDrawerGeometry()
    {
        if (body_ == nullptr || drawer_ == nullptr || drawerMode_ == ContextDrawerMode::Collapsed) {
            return;
        }
        constexpr int kDrawerMargin = 10;
        const int availableWidth = body_->width();
        const int availableHeight = body_->height();
        const int width = std::max(
            kMinDrawerWidth,
            std::min(drawerWidth_, std::max(kMinDrawerWidth, availableWidth - (2 * kDrawerMargin))));
        drawerWidth_ = width;
        const int drawerX = availableWidth - width - kDrawerMargin;
        drawer_->setGeometry(
            drawerX,
            kDrawerMargin,
            width,
            std::max(240, availableHeight - (2 * kDrawerMargin)));
        if (drawerHandle_ != nullptr) {
            drawerHandle_->setGeometry(
                drawerX - (drawerHandle_->width() / 2),
                kDrawerMargin,
                drawerHandle_->width(),
                std::max(240, availableHeight - (2 * kDrawerMargin)));
        }
    }

    void collapseDrawerFromDrag()
    {
        if (body_ == nullptr || drawer_ == nullptr) {
            return;
        }
        drawerMode_ = ContextDrawerMode::Collapsed;
        drawer_->hide();
        if (drawerHandle_ != nullptr) {
            constexpr int kDrawerMargin = 10;
            const int handleHeight = std::max(240, body_->height() - (2 * kDrawerMargin));
            drawerHandle_->setGeometry(
                body_->width() - kDrawerMargin - (drawerHandle_->width() / 2),
                kDrawerMargin,
                drawerHandle_->width(),
                handleHeight);
            drawerHandle_->show();
            drawerHandle_->raise();
        }
        persistDrawerState();
    }

    void loadDrawerState()
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspace/%1/Drawer").arg(workspaceKey_));
        drawerWidth_ = settings.value(QStringLiteral("width"), 520).toInt();
        const int mode = settings.value(QStringLiteral("mode"), static_cast<int>(ContextDrawerMode::Expanded)).toInt();
        if (mode == static_cast<int>(ContextDrawerMode::Peek)) {
            lastOpenMode_ = ContextDrawerMode::Peek;
        } else if (mode == static_cast<int>(ContextDrawerMode::Focused)) {
            lastOpenMode_ = ContextDrawerMode::Focused;
        } else {
            lastOpenMode_ = ContextDrawerMode::Expanded;
        }
        settings.endGroup();
    }

    void persistDrawerState() const
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspace/%1/Drawer").arg(workspaceKey_));
        settings.setValue(QStringLiteral("mode"), static_cast<int>(lastOpenMode_));
        settings.setValue(QStringLiteral("width"), drawerWidth_);
        settings.endGroup();
    }

    QString workspaceKey_;
    QFrame* body_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QWidget* canvas_ = nullptr;
    QVBoxLayout* canvasLayout_ = nullptr;
    QFrame* drawer_ = nullptr;
    DrawerResizeHandle* drawerHandle_ = nullptr;
    QLabel* drawerTitle_ = nullptr;
    QWidget* drawerScrollContent_ = nullptr;
    QVBoxLayout* drawerContentLayout_ = nullptr;
    QLabel* drawerBody_ = nullptr;
    QWidget* contextWidget_ = nullptr;
    QPushButton* closeDrawerButton_ = nullptr;
    QGridLayout* drawerActions_ = nullptr;
    UiEntityRef currentEntity_;
    ContextDrawerMode drawerMode_ = ContextDrawerMode::Collapsed;
    ContextDrawerMode lastOpenMode_ = ContextDrawerMode::Expanded;
    static constexpr int kMinDrawerWidth = 320;
    int drawerWidth_ = 520;
};

} // namespace savorqt::gui
