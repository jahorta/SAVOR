#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QtTypes>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Widgets/PersistentToolWindow.h"

class QLabel;
class QListWidget;
class QPushButton;
class QSplitter;
class QVBoxLayout;

enum class AuthoringLibraryKey {
    SeedProbe,
    BattlePlan,
    Predicates,
    PredicateGroups,
};

struct SpecLibraryRow {
    qint64 id = 0;
    QString text;
    QString details;
    bool enabled = true;
};

struct SpecLibraryCallbacks {
    std::function<void(const QString&, StatusToast::Severity)> postStatus;
    std::function<void()> refreshLibrary;
    std::function<void(qint64)> openPredicateGroup;
};

struct SpecLibraryOperationResult {
    bool ok = false;
    QString message;
    StatusToast::Severity severity = StatusToast::Severity::Info;
};

class ISpecLibraryAdapter {
public:
    virtual ~ISpecLibraryAdapter() = default;

    virtual AuthoringLibraryKey key() const = 0;
    virtual QString title() const = 0;
    virtual QString placeholderText() const = 0;
    virtual QString savedItemsLabel() const;
    virtual QString newButtonText() const;
    virtual QString editButtonText() const;
    virtual QStringList newActionLabels() const;
    virtual bool supportsDelete() const;
    virtual bool editCreatesCopy() const;
    virtual std::vector<SpecLibraryRow> refreshRows(QString* errorText) = 0;
    virtual QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) = 0;
    virtual QWidget* createNewEditorForAction(int action, QWidget* parent,
                                               SpecLibraryCallbacks callbacks);
    virtual QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) = 0;
    virtual SpecLibraryOperationResult deleteRow(int row, QWidget* parent);
};

class AuthoringLibraryWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit AuthoringLibraryWidget(QWidget* parent = nullptr);

    void selectLibrary(AuthoringLibraryKey key);
    AuthoringLibraryKey currentLibrary() const;

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct LibraryRefreshRequest {
        int adapterIndex = -1;
        ISpecLibraryAdapter* adapter = nullptr;
    };
    struct LibraryRefreshData {
        int adapterIndex = -1;
        std::vector<SpecLibraryRow> rows;
        QString errorText;
    };

    void createAdapters();
    void createWidgets();
    void selectLibraryIndex(int index, bool forceRefresh);
    void refreshLibrary();
    void clearRightPane();
    void installEditorWidget(QWidget* editor);
    void showEditorForSelectedRow();
    void deleteSelectedRow();
    void handleSavedRowChanged();
    void openPredicateGroup(qint64 bindingRevisionId);
    void installNewActionMenu();
    void updateActionState();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);
    int selectedSavedRow() const;
    ISpecLibraryAdapter* currentAdapter() const;

    std::vector<std::unique_ptr<ISpecLibraryAdapter>> adapters_;
    int currentAdapterIndex_ = -1;
    QListWidget* librarySelector_ = nullptr;
    QLabel* savedItemsLabel_ = nullptr;
    QListWidget* savedItemsList_ = nullptr;
    QLabel* libraryStatusLabel_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QWidget* rightPane_ = nullptr;
    QVBoxLayout* rightPaneLayout_ = nullptr;
    QLabel* placeholderLabel_ = nullptr;
    QWidget* activeEditor_ = nullptr;
    QSplitter* contentSplitter_ = nullptr;
    savorqt::gui::AsyncRefreshPipeline<LibraryRefreshRequest, LibraryRefreshData>* refreshPipeline_ = nullptr;
};

class AuthoringLibraryWindow : public PersistentToolWindow
{
    Q_OBJECT

public:
    explicit AuthoringLibraryWindow(QWidget* parent = nullptr);

    void selectLibrary(AuthoringLibraryKey key);
    AuthoringLibraryKey currentLibrary() const;

signals:
    void statusToastRequested(StatusToast toast);

private:
    AuthoringLibraryWidget* widget_ = nullptr;
};
