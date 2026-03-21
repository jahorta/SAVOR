#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

#include "DB/DBCore/DbResult.h"
#include "DB/Querying/PagedQuery.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeView;
class QDialogButtonBox;
class LedgerPickerDialogModel;
class LedgerPickerTreeView;

class LedgerPickerDialogBase : public QDialog
{
    Q_OBJECT

public:
    struct Column {
        QString title;
        int stretch = 1;
    };

    struct PageData {
        QVector<QVector<QString>> cells;
        QVector<qint64> ids;
        QStringList summaries;
        std::optional<KeysetCursor> prev;
        std::optional<KeysetCursor> next;
    };

    using FetchResult = simcore::db::DbResult<PageData>;
    using FetchFn = std::function<FetchResult(const PagedQuery<>& query, const QString& search)>;

    LedgerPickerDialogBase(const QString& title,
        QVector<Column> columns,
        FetchFn fetchFn,
        QWidget* parent = nullptr);
    ~LedgerPickerDialogBase() override;

    qint64 selectedId() const;

protected:
    virtual void handlePageLoadedSuccess();

private slots:
    void onFetchFinished();

private:
    void fetchPage();
    void rebuildView();
    void refreshStatus();
    void refreshSelectionSummary();
    void updateSelectionFromView();

protected:
    QVector<Column> columns_;
    FetchFn fetchFn_;
    QFutureWatcher<FetchResult> watcher_;
    QLineEdit* searchEdit_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    LedgerPickerTreeView* treeView_ = nullptr;
    LedgerPickerDialogModel* model_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* newerButton_ = nullptr;
    QPushButton* olderButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* selectionSummaryLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    PageData pageData_;
    std::optional<qint64> selectedId_;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    QString errorMessage_;
    bool loading_ = false;
};

template <typename RowT>
class LedgerPickerDialog final : public LedgerPickerDialogBase
{
public:
    using TypedFetchResult = simcore::db::DbResult<Page<RowT>>;
    using FetchFn = std::function<TypedFetchResult(const PagedQuery<>& query, const QString& search)>;
    using DescribeFn = std::function<QString(const RowT& row)>;
    using IdFn = std::function<qint64(const RowT& row)>;

    struct Column {
        QString title;
        DescribeFn text;
        int stretch = 1;
    };

    LedgerPickerDialog(const QString& title,
        QVector<Column> columns,
        FetchFn fetchFn,
        IdFn idFn,
        DescribeFn summaryFn,
        QWidget* parent = nullptr)
        : LedgerPickerDialogBase(
            title,
            mapColumns(columns),
            [this, columns, fetchFn = std::move(fetchFn), idFn = idFn, summaryFn = std::move(summaryFn)](const PagedQuery<>& query, const QString& search) {
                const TypedFetchResult result = fetchFn(query, search);
                if (!result.ok) {
                    rows_.clear();
                    FetchResult errorResult;
                    errorResult.ok = false;
                    errorResult.error = result.error;
                    return errorResult;
                }

                rows_ = result.value.items;
                PageData pageData;
                pageData.prev = result.value.prev;
                pageData.next = result.value.next;
                pageData.cells.reserve(static_cast<int>(rows_.size()));
                pageData.ids.reserve(static_cast<int>(rows_.size()));
                pageData.summaries.reserve(static_cast<int>(rows_.size()));
                for (const RowT& row : rows_) {
                    QVector<QString> cellRow;
                    cellRow.reserve(columns.size());
                    for (const Column& column : columns) {
                        cellRow.push_back(column.text(row));
                    }
                    pageData.cells.push_back(std::move(cellRow));
                    pageData.ids.push_back(idFn(row));
                    pageData.summaries.push_back(summaryFn(row));
                }

                FetchResult successResult;
                successResult.ok = true;
                successResult.value = std::move(pageData);
                return successResult;
            },
            parent)
        , idFn_(std::move(idFn))
    {
    }

    std::optional<RowT> selectedRow() const
    {
        const qint64 id = selectedId();
        if (id <= 0) {
            return std::nullopt;
        }

        for (const RowT& row : rows_) {
            if (idFn_(row) == id) {
                return row;
            }
        }
        return std::nullopt;
    }

private:
    static QVector<LedgerPickerDialogBase::Column> mapColumns(const QVector<Column>& columns)
    {
        QVector<LedgerPickerDialogBase::Column> mapped;
        mapped.reserve(columns.size());
        for (const Column& column : columns) {
            mapped.push_back({ column.title, column.stretch });
        }
        return mapped;
    }

    std::vector<RowT> rows_;
    IdFn idFn_ = [](const RowT&) { return 0; };
};
