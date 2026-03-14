#pragma once
#include "IdPicker.h"
#include "DB/Querying/IdRepoListDTO.h"
#include "DB/ExplorerSettingsRepo.h"
#include <optional>

namespace soasim::ui::adapters {

	// Each factory returns a fully-wired adapter:
	// - columns filled
	// - submit_request bound to DataService fetch for this ledger
	// - poll_snapshot reading from an internal SnapshotMailbox<Page<Row>>
	// - get_id implemented
	// - draw_preview can be empty initially (stub)

	LedgerAdapter<simcore::db::SavestateLite> MakeSavestateAdapter(int page_size = 100);

	// ObjectRef: optional extension filter (e.g., ".dtm")
	LedgerAdapter<simcore::db::ObjectRefLite> MakeObjectRefAdapter(std::string ext_filter = ".dtm", int page_size = 100);

	// SeedProbe: default filter status='done'. Optional constrain by savestate_id.
	// Implementations may expose open_aux_filter to launch a nested savestate picker.
	LedgerAdapter<simcore::db::SeedProbeLite> MakeSeedProbeAdapter(int page_size = 100, std::optional<int64_t> filter_savestate_id = std::nullopt);

	// TasMovie: default filter status='done'
	LedgerAdapter<simcore::db::TasMovieLite> MakeTasMovieAdapter(int page_size = 100);

	// ExplorerSettings: search by name/description
	LedgerAdapter <simcore::db::ExplorerSettingsLite> MakeExplorerSettingsAdapter(int page_size = 100);

} // namespace soasim::ui::adapters
