#include "GUI/Widgets/LedgerPickerDialog.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

class LedgerPickerDialogModel final : public QAbstractItemModel
{
public:
    explicit LedgerPickerDialogModel(QObject* parent = nullptr)
        : QAbstractItemModel(parent)
    {
    }

    void setColumns(const QVector<LedgerPickerDialogBase::Column>& columns)
    {
        columns_ = columns;
    }

    void setPageData(LedgerPickerDialogBase::PageData pageData)
    {
        beginResetModel();
        pageData_ = std::move(pageData);
        endResetModel();
    }

    QModelIndex index(int row, int column, const QModelIndex& parent) const override
    {
        if (parent.isValid() || row < 0 || column < 0 || row >= rowCount({}) || column >= columnCount({})) {
            return {};
        }
        return createIndex(row, column);
    }

    QModelIndex parent(const QModelIndex& child) const override
    {
        Q_UNUSED(child);
        return {};
    }

    int rowCount(const QModelIndex& parent) const override
    {
        return parent.isValid() ? 0 : pageData_.cells.size();
    }

    int columnCount(const QModelIndex& parent) const override
    {
        return parent.isValid() ? 0 : columns_.size();
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= pageData_.cells.size() || index.column() >= columns_.size()) {
            return {};
        }

        if (role == Qt::DisplayRole) {
            return pageData_.cells.at(index.row()).value(index.column());
        }
        if (role == Qt::UserRole) {
            return pageData_.ids.value(index.row());
        }
        if (role == Qt::ToolTipRole) {
            return pageData_.summaries.value(index.row());
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal || section < 0 || section >= columns_.size()) {
            return QAbstractItemModel::headerData(section, orientation, role);
        }
        return columns_.at(section).title;
    }

    QString summaryForRow(int row) const
    {
        return row >= 0 && row < pageData_.summaries.size() ? pageData_.summaries.at(row) : QString{};
    }

    qint64 idForRow(int row) const
    {
        return row >= 0 && row < pageData_.ids.size() ? pageData_.ids.at(row) : 0;
    }

private:
    QVector<LedgerPickerDialogBase::Column> columns_;
    LedgerPickerDialogBase::PageData pageData_;
};

class LedgerPickerTreeView final : public QTreeView
{
public:
    explicit LedgerPickerTreeView(QWidget* parent = nullptr)
        : QTreeView(parent)
    {
        setRootIsDecorated(false);
        setItemsExpandable(false);
        setUniformRowHeights(true);
        setAllColumnsShowFocus(true);
        setAlternatingRowColors(true);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setSortingEnabled(false);
        setExpandsOnDoubleClick(false);
        setMouseTracking(true);
        header()->setStretchLastSection(false);
    }
};

LedgerPickerDialogBase::LedgerPickerDialogBase(const QString& title,
    QVector<Column> columns,
    FetchFn fetchFn,
    QWidget* parent)
    : QDialog(parent)
    , columns_(std::move(columns))
    , fetchFn_(std::move(fetchFn))
{
    setWindowTitle(title);
    resize(900, 520);
    setModal(true);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

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

    model_ = new LedgerPickerDialogModel(this);
    model_->setColumns(columns_);
    treeView_ = new LedgerPickerTreeView(this);
    treeView_->setModel(model_);
    treeView_->header()->setVisible(true);
    for (int index = 0; index < columns_.size(); ++index) {
        treeView_->header()->setSectionResizeMode(index, columns_.at(index).stretch > 1 ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    }
    rootLayout->addWidget(treeView_, 1);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    rootLayout->addWidget(statusLabel_);

    QHBoxLayout* pagingLayout = new QHBoxLayout();
    newerButton_ = new QPushButton(QStringLiteral("Newer"), this);
    olderButton_ = new QPushButton(QStringLiteral("Older"), this);
    pageSummaryLabel_ = new QLabel(this);
    pagingLayout->addWidget(newerButton_);
    pagingLayout->addWidget(olderButton_);
    pagingLayout->addSpacing(10);
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

    connect(&watcher_, &QFutureWatcher<FetchResult>::finished, this, &LedgerPickerDialogBase::onFetchFinished);
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
        if (!pageData_.prev.has_value()) {
            return;
        }
        before_ = pageData_.prev;
        after_.reset();
        fetchPage();
    });
    connect(olderButton_, &QPushButton::clicked, this, [this]() {
        if (!pageData_.next.has_value()) {
            return;
        }
        after_ = pageData_.next;
        before_.reset();
        fetchPage();
    });
    connect(treeView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updateSelectionFromView();
    });
    connect(treeView_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.isValid()) {
            return;
        }
        treeView_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        updateSelectionFromView();
        if (selectedId_.has_value()) {
            accept();
        }
    });
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    fetchPage();
}

LedgerPickerDialogBase::~LedgerPickerDialogBase() = default;

qint64 LedgerPickerDialogBase::selectedId() const
{
    return selectedId_.value_or(0);
}

void LedgerPickerDialogBase::handlePageLoadedSuccess()
{
}

void LedgerPickerDialogBase::onFetchFinished()
{
    loading_ = false;
    try {
        const FetchResult result = watcher_.result();
        if (result.ok) {
            pageData_ = result.value;
            errorMessage_.clear();
            handlePageLoadedSuccess();
        } else {
            pageData_ = {};
            selectedId_.reset();
            errorMessage_ = QString::fromStdString(result.error.message);
        }
    } catch (...) {
        pageData_ = {};
        selectedId_.reset();
        errorMessage_ = QStringLiteral("Picker load failed due to an unexpected exception.");
    }
    rebuildView();
    refreshStatus();
}

void LedgerPickerDialogBase::fetchPage()
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

void LedgerPickerDialogBase::rebuildView()
{
    model_->setPageData(pageData_);
    treeView_->setRootIsDecorated(false);
    if (treeView_->selectionModel()) {
        treeView_->selectionModel()->clearSelection();
    }
    selectedId_.reset();
    newerButton_->setEnabled(pageData_.prev.has_value() && !loading_);
    olderButton_->setEnabled(pageData_.next.has_value() && !loading_);
    pageSummaryLabel_->setText(QStringLiteral("%1 row(s)").arg(pageData_.cells.size()));
    refreshSelectionSummary();
}

void LedgerPickerDialogBase::refreshStatus()
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

void LedgerPickerDialogBase::refreshSelectionSummary()
{
    const QModelIndex currentIndex = treeView_->currentIndex();
    if (currentIndex.isValid()) {
        selectionSummaryLabel_->setText(model_->summaryForRow(currentIndex.row()));
    } else {
        selectionSummaryLabel_->setText(QStringLiteral("No selection."));
    }
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(selectedId_.has_value());
}

void LedgerPickerDialogBase::updateSelectionFromView()
{
    const QModelIndex currentIndex = treeView_->currentIndex();
    if (currentIndex.isValid()) {
        selectedId_ = model_->idForRow(currentIndex.row());
    } else {
        selectedId_.reset();
    }
    refreshSelectionSummary();
}
