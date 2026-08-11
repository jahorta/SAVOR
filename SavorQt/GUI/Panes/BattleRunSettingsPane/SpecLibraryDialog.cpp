#include "SpecLibraryDialog.h"

SpecLibraryDialog::SpecLibraryDialog(SpecKind kind, QWidget* parent)
    : AuthoringLibraryDialog(parent)
{
    selectLibrary(toAuthoringLibraryKey(kind));
}

AuthoringLibraryKey SpecLibraryDialog::toAuthoringLibraryKey(SpecKind kind)
{
    switch (kind) {
    case SpecKind::SeedProbe:
        return AuthoringLibraryKey::SeedProbe;
    case SpecKind::Tas:
        return AuthoringLibraryKey::Tas;
    case SpecKind::BattleRun:
        return AuthoringLibraryKey::BattleRun;
    case SpecKind::BattlePlan:
        return AuthoringLibraryKey::BattlePlan;
    case SpecKind::ExplorerSettings:
        return AuthoringLibraryKey::ExplorerSettings;
    }
    return AuthoringLibraryKey::SeedProbe;
}
