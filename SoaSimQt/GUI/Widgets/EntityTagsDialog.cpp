#include "EntityTagsDialog.h"

#include "DB/TagRepo.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>

EntityTagsDialog::EntityTagsDialog(const QString& entityKind, qint64 entityId, const QString& title, QWidget* parent)
    : QDialog(parent)
    , entityKind_(entityKind.trimmed())
    , entityId_(entityId)
    , entityExists_(entityId_ > 0)
{
    setWindowTitle(title);
    resize(520, 500);
    createWidgets();
    wireSignals();
    loadTags();
}

EntityTagsDialog::Result EntityTagsDialog::EditEntityTags(const QString& entityKind, qint64 entityId, const QString& title, QWidget* parent)
{
    EntityTagsDialog dialog(entityKind, entityId, title, parent);
    const int rc = dialog.exec();
    if (rc != QDialog::Accepted) {
        return {};
    }

    Result out{};
    out.ok = true;
    out.selectedTagKeys = dialog.selectedTagKeys();
    return out;
}

EntityTagsDialog::Result EntityTagsDialog::SelectTagsForNewEntity(const QString& entityKind, const std::vector<std::string>& initialTagKeys, const QString& title, QWidget* parent)
{
    EntityTagsDialog dialog(entityKind, 0, title, parent);
    dialog.setSelectedTags(initialTagKeys);
    const int rc = dialog.exec();
    if (rc != QDialog::Accepted) {
        return {};
    }

    Result out{};
    out.ok = true;
    out.selectedTagKeys = dialog.selectedTagKeys();
    return out;
}

void EntityTagsDialog::createWidgets()
{
    QVBoxLayout* root = new QVBoxLayout(this);

    QLabel* helper = new QLabel(QStringLiteral("Create new tags or select existing tags."), this);
    helper->setWordWrap(true);
    root->addWidget(helper);

    QHBoxLayout* createRow = new QHBoxLayout();
    createTagEdit_ = new QLineEdit(this);
    createTagEdit_->setPlaceholderText(QStringLiteral("namespace::leaf or leaf"));
    createTagButton_ = new QPushButton(QStringLiteral("Create Tag"), this);
    createTagButton_->setObjectName("jobsSecondaryButton");
    createRow->addWidget(createTagEdit_, 1);
    createRow->addWidget(createTagButton_);
    root->addLayout(createRow);

    tagTree_ = new QTreeWidget(this);
    tagTree_->setHeaderHidden(true);
    tagTree_->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(tagTree_, 1);

    QDialogButtonBox* box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    saveButton_ = box->addButton(entityExists_ ? QStringLiteral("Save") : QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
    saveButton_->setObjectName("jobsPrimaryButton");
    root->addWidget(box);

    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(saveButton_, &QPushButton::clicked, this, [this]() {
        applyChanges();
    });
}

void EntityTagsDialog::wireSignals()
{
    connect(createTagButton_, &QPushButton::clicked, this, [this]() {
        const QString key = createTagEdit_->text().trimmed();
        if (key.isEmpty()) {
            return;
        }
        const auto ensureResult = simcore::db::TagRepo::EnsureTag(key.toStdString(), std::nullopt);
        if (!ensureResult.ok) {
            QMessageBox::warning(this, QStringLiteral("Create Tag"), QStringLiteral("Failed to create tag: %1").arg(QString::fromStdString(ensureResult.error.message)));
            return;
        }
        createTagEdit_->clear();
        loadTags();
    });
}

void EntityTagsDialog::loadTags()
{
    const auto tagResult = simcore::db::TagRepo::ListTags(std::nullopt);
    if (!tagResult.ok) {
        QMessageBox::warning(this, QStringLiteral("Tags"), QStringLiteral("Failed to load tags: %1").arg(QString::fromStdString(tagResult.error.message)));
        return;
    }
    allTags_ = tagResult.value;

    if (entityExists_ && entityId_ > 0 && !entityKind_.isEmpty()) {
        const auto entityTagsResult = simcore::db::TagRepo::ListEntityTags(entityKind_.toStdString(), entityId_);
        if (!entityTagsResult.ok) {
            QMessageBox::warning(this, QStringLiteral("Tags"), QStringLiteral("Failed to load entity tags: %1").arg(QString::fromStdString(entityTagsResult.error.message)));
            return;
        }
        initialTagKeys_.clear();
        for (const auto& t : entityTagsResult.value) {
            initialTagKeys_.push_back(t.tag_key);
        }
    }

    refreshTagList();
}

void EntityTagsDialog::refreshTagList()
{
    tagTree_->clear();
    std::map<QString, QTreeWidgetItem*> namespaceNodes;
    for (const auto& tag : allTags_) {
        const QString fullKey = QString::fromStdString(tag.tag_key);
        const QStringList parts = fullKey.split(QStringLiteral("::"), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            continue;
        }

        QTreeWidgetItem* parentNode = nullptr;
        if (parts.size() > 1) {
            QString namespacePath;
            for (int i = 0; i < parts.size() - 1; ++i) {
                namespacePath = namespacePath.isEmpty() ? parts.at(i) : namespacePath + QStringLiteral("::") + parts.at(i);
                auto it = namespaceNodes.find(namespacePath);
                if (it != namespaceNodes.end()) {
                    parentNode = it->second;
                    continue;
                }

                QTreeWidgetItem* namespaceItem = new QTreeWidgetItem(QStringList(parts.at(i)));
                namespaceItem->setFlags(namespaceItem->flags() & ~Qt::ItemIsUserCheckable);
                if (parentNode) {
                    parentNode->addChild(namespaceItem);
                }
                else {
                    tagTree_->addTopLevelItem(namespaceItem);
                }
                namespaceNodes.emplace(namespacePath, namespaceItem);
                parentNode = namespaceItem;
            }
        }

        const QString leafText = parts.last();
        QTreeWidgetItem* leafItem = new QTreeWidgetItem(QStringList(leafText));
        leafItem->setFlags(leafItem->flags() | Qt::ItemIsUserCheckable);
        leafItem->setData(0, Qt::UserRole, fullKey);
        const bool checked = std::find(initialTagKeys_.begin(), initialTagKeys_.end(), tag.tag_key) != initialTagKeys_.end();
        leafItem->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        if (parentNode) {
            parentNode->addChild(leafItem);
        }
        else {
            tagTree_->addTopLevelItem(leafItem);
        }
    }
    tagTree_->expandAll();
}

void EntityTagsDialog::applyChanges()
{
    if (entityExists_ && entityId_ > 0 && !entityKind_.isEmpty()) {
        const std::vector<std::string> current = selectedTagKeys();
        for (const std::string& existing : initialTagKeys_) {
            if (std::find(current.begin(), current.end(), existing) == current.end()) {
                const auto result = simcore::db::TagRepo::DetachTagFromEntity(entityKind_.toStdString(), entityId_, existing);
                if (!result.ok) {
                    QMessageBox::warning(this, QStringLiteral("Tags"), QStringLiteral("Failed to remove tag '%1': %2").arg(QString::fromStdString(existing), QString::fromStdString(result.error.message)));
                    return;
                }
            }
        }
        for (const std::string& selected : current) {
            if (std::find(initialTagKeys_.begin(), initialTagKeys_.end(), selected) == initialTagKeys_.end()) {
                const auto result = simcore::db::TagRepo::AttachTagToEntity(entityKind_.toStdString(), entityId_, selected, std::nullopt);
                if (!result.ok) {
                    QMessageBox::warning(this, QStringLiteral("Tags"), QStringLiteral("Failed to add tag '%1': %2").arg(QString::fromStdString(selected), QString::fromStdString(result.error.message)));
                    return;
                }
            }
        }
    }
    accept();
}

void EntityTagsDialog::setSelectedTags(const std::vector<std::string>& tagKeys)
{
    initialTagKeys_ = tagKeys;
    refreshTagList();
}

std::vector<std::string> EntityTagsDialog::selectedTagKeys() const
{
    std::vector<std::string> out;
    const std::function<void(QTreeWidgetItem*)> collectChecked = [&](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        if (item->childCount() == 0 && item->checkState(0) == Qt::Checked) {
            const QString fullKey = item->data(0, Qt::UserRole).toString().trimmed();
            if (!fullKey.isEmpty()) {
                out.push_back(fullKey.toStdString());
            }
        }
        for (int i = 0; i < item->childCount(); ++i) {
            collectChecked(item->child(i));
        }
    };

    for (int i = 0; i < tagTree_->topLevelItemCount(); ++i) {
        collectChecked(tagTree_->topLevelItem(i));
    }
    return out;
}
