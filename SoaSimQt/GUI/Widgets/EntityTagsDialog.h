#pragma once

#include <QtWidgets/QDialog>
#include "DB/TagRepo.h"

#include <optional>
#include <vector>

class QLineEdit;
class QTreeWidget;
class QPushButton;
class QTreeWidgetItem;

class EntityTagsDialog final : public QDialog
{
    Q_OBJECT

public:
    struct Result {
        bool ok = false;
        std::vector<std::string> selectedTagKeys;
    };

    EntityTagsDialog(const QString& entityKind, qint64 entityId, const QString& title, QWidget* parent = nullptr);

    static Result EditEntityTags(const QString& entityKind, qint64 entityId, const QString& title, QWidget* parent = nullptr);
    static Result SelectTagsForNewEntity(const QString& entityKind, const std::vector<std::string>& initialTagKeys, const QString& title, QWidget* parent = nullptr);

private:
    void createWidgets();
    void wireSignals();
    void loadTags();
    void refreshTagList();
    void applyChanges();
    void setSelectedTags(const std::vector<std::string>& tagKeys);
    std::vector<std::string> selectedTagKeys() const;

    QString entityKind_;
    qint64 entityId_ = 0;
    bool entityExists_ = false;

    QLineEdit* createTagEdit_ = nullptr;
    QPushButton* createTagButton_ = nullptr;
    QTreeWidget* tagTree_ = nullptr;
    QPushButton* saveButton_ = nullptr;

    std::vector<simcore::db::TagRecord> allTags_;
    std::vector<std::string> initialTagKeys_;
};
