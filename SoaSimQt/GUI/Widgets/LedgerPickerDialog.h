#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QString>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include "DB/DBCore/DbResult.h"
#include "DB/Querying/PagedQuery.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

template <typename RowT>
class LedgerPickerDialog final : public QDialog
{
public:
    using FetchResult = simcore::db::DbResult<Page<RowT>>;
    using FetchFn = std::function<FetchResult(const PagedQuery<>& query, const QString& search)>;
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
        : QDialog(parent)
        , columns_(std::move(columns))
        , fetchFn_(std::move(fetchFn))
        , idFn_(std::move(idFn))
        , summaryFn_(std::move(summaryFn))
    {
        setWindowTitle(title);
        resize(900, 520);
        setModal(true);

        QVBoxLayout* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(16, 16, 16, 16);
        rootLayout->setSpacing(12);

        QHBoxLayout* searchLayout = new QHBoxLayout();
        searchEdit_ = new QLineEdit(this);
        searchEdit_->setPlaceholderText(QStringLiteral("Search…"));
        pageSizeSpin_ = new QSpinBox(this);
        pageSizeSpin_->setRange(10, 500);
        pageSizeSpin_->setSingleStep(10);
        pageSizeSpin_->setValue(100);
        applyButton_ = new QPushButton(QStringLiteral("Apply"), this);
        resetButton_ = new QPushButton(QStringLiteral("Reset"), this);
        searchLayout->addWidget(new QLabel(QStringLiteral("Search"), this));
        searchLayout->addWidget(searchEdit_, 1);
        searchLayout->addWidget(new QLabel(QStringLiteral("Page size"), this));
        searchLayout->addWidget(pageSizeSpin_);
        searchLayout->addWidget(applyButton_);
        searchLayout->addWidget(resetButton_);
        rootLayout->addLayout(searchLayout);

        table_ = new QTableWidget(this);
        table_->setColumnCount(columns_.size());
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->setVisible(false);
        table_->horizontalHeader()->setStretchLastSection(false);
        QStringList headers;
        for (const Column& column : columns_) {
            headers.push_back(column.title);
        }
        table_->setHorizontalHeaderLabels(headers);
        for (int index = 0; index < columns_.size(); ++index) {
            table_->horizontalHeader()->setSectionResizeMode(index, QHeaderView::Stretch);
        }
        rootLayout->addWidget(table_, 1);

        statusLabel_ = new QLabel(this);
        statusLabel_->setWordWrap(true);
        rootLayout->addWidget(statusLabel_);

        QHBoxLayout* pagingLayout = new QHBoxLayout();
        newerButton_ = new QPushButton(QStringLiteral("Newer"), this);
        olderButton_ = new QPushButton(QStringLiteral("Older"), this);
        pageSummaryLabel_ = new QLabel(this);
        pagingLayout->addWidget(newerButton_);
        pagingLayout->addWidget(olderButton_);
        pagingLayout->addSpacing(12);
        pagingLayout->addWidget(pageSummaryLabel_);
        pagingLayout->addStretch();
        rootLayout->addLayout(pagingLayout);

        selectionSummaryLabel_ = new QLabel(QStringLiteral("No selection."), this);
        selectionSummaryLabel_->setWordWrap(true);
        rootLayout->addWidget(selectionSummaryLabel_);

        buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttonBox_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Select"));
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
        rootLayout->addWidget(buttonBox_);

        connect(&watcher_, &QFutureWatcher<FetchResult>::finished, this, [this]() {
            loading_ = false;
            try {
                const FetchResult result = watcher_.result();
                if (result.ok) {
                    page_ = result.value;
                    rows_ = result.value.items;
                    errorMessage_.clear();
                } else {
                    page_ = {};
                    rows_.clear();
                    errorMessage_ = QString::fromStdString(result.error.message);
                }
            } catch (...) {
                page_ = {};
                rows_.clear();
                errorMessage_ = QStringLiteral("Picker load failed due to an unexpected exception.");
            }
            rebuildTable();
            refreshStatus();
        });

        connect(applyButton_, &QPushButton::clicked, this, [this]() {
            before_.reset();
            after_.reset();
            fetchPage();
        });
        connect(resetButton_, &QPushButton::clicked, this, [this]() {
            searchEdit_->clear();
            pageSizeSpin_->setValue(100);
            before_.reset();
            after_.reset();
            fetchPage();
        });
        connect(newerButton_, &QPushButton::clicked, this, [this]() {
            if (!page_.prev.has_value()) {
                return;
            }
            before_ = page_.prev;
            after_.reset();
            fetchPage();
        });
        connect(olderButton_, &QPushButton::clicked, this, [this]() {
            if (!page_.next.has_value()) {
                return;
            }
            after_ = page_.next;
            before_.reset();
            fetchPage();
        });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
            const QList<QTableWidgetItem*> selectedItems = table_->selectedItems();
            const int row = selectedItems.isEmpty() ? -1 : selectedItems.front()->row();
            if (row >= 0 && row < rows_.size()) {
                selectedRow_ = rows_.at(row);
            } else {
                selectedRow_.reset();
            }
            refreshSelectionSummary();
        });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row >= 0 && row < rows_.size()) {
                selectedRow_ = rows_.at(row);
                accept();
            }
        });
        connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

        fetchPage();
    }

    std::optional<RowT> selectedRow() const
    {
        return selectedRow_;
    }

    qint64 selectedId() const
    {
        if (!selectedRow_.has_value()) {
            return 0;
        }
        return idFn_(*selectedRow_);
    }

private:
    void fetchPage()
    {
        if (loading_) {
            return;
        }

        loading_ = true;
        errorMessage_.clear();
        refreshStatus();

        PagedQuery<> query;
        query.before = before_;
        query.after = after_;
        query.limit = pageSizeSpin_->value();
        const QString search = searchEdit_->text().trimmed();
        watcher_.setFuture(QtConcurrent::run([fetch = fetchFn_, query, search]() {
            return fetch(query, search);
        }));
    }

    void rebuildTable()
    {
        table_->setRowCount(static_cast<int>(rows_.size()));
        for (int rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
            const RowT& row = rows_.at(rowIndex);
            for (int columnIndex = 0; columnIndex < columns_.size(); ++columnIndex) {
                QTableWidgetItem* item = new QTableWidgetItem(columns_.at(columnIndex).text(row));
                item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(idFn_(row)));
                table_->setItem(rowIndex, columnIndex, item);
            }
        }
        newerButton_->setEnabled(page_.prev.has_value() && !loading_);
        olderButton_->setEnabled(page_.next.has_value() && !loading_);
        pageSummaryLabel_->setText(QStringLiteral("%1 row(s)").arg(rows_.size()));
        refreshSelectionSummary();
    }

    void refreshStatus()
    {
        if (loading_) {
            statusLabel_->setText(QStringLiteral("Loading results…"));
            return;
        }
        if (!errorMessage_.isEmpty()) {
            statusLabel_->setText(QStringLiteral("Load failed: %1").arg(errorMessage_));
            return;
        }
        statusLabel_->setText(QStringLiteral("Choose a row and click Select."));
    }

    void refreshSelectionSummary()
    {
        if (selectedRow_.has_value()) {
            selectionSummaryLabel_->setText(summaryFn_(*selectedRow_));
        } else {
            selectionSummaryLabel_->setText(QStringLiteral("No selection."));
        }
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(selectedRow_.has_value());
    }

    QVector<Column> columns_;
    FetchFn fetchFn_;
    IdFn idFn_;
    DescribeFn summaryFn_;
    QFutureWatcher<FetchResult> watcher_;
    QLineEdit* searchEdit_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* newerButton_ = nullptr;
    QPushButton* olderButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* selectionSummaryLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    std::vector<RowT> rows_;
    Page<RowT> page_;
    std::optional<RowT> selectedRow_;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    QString errorMessage_;
    bool loading_ = false;
};
