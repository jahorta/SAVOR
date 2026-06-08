#pragma once

#include <QtCore/QAbstractTableModel>

#include "UIRead/IUiReadDb.h"

#include <vector>

class ArtifactsTableModel final : public QAbstractTableModel
{
public:
    explicit ArtifactsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setArtifacts(const std::vector<simcore::db::UiJobArtifact>& artifacts);

private:
    static bool artifactAffectsDisplay(const simcore::db::UiJobArtifact& lhs, const simcore::db::UiJobArtifact& rhs);

    std::vector<simcore::db::UiJobArtifact> artifacts_;
};
