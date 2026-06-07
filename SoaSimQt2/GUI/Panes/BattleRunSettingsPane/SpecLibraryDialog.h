#pragma once

#include "AuthoringLibraryDialog.h"

class SpecLibraryDialog final : public AuthoringLibraryDialog
{
    Q_OBJECT

public:
    enum class SpecKind {
        SeedProbe,
        Tas,
        BattleRun,
        Predicate,
        PredicateSet,
        BattlePlan,
        ExplorerSettings,
    };

    explicit SpecLibraryDialog(SpecKind kind, QWidget* parent = nullptr);

private:
    static AuthoringLibraryKey toAuthoringLibraryKey(SpecKind kind);
};
