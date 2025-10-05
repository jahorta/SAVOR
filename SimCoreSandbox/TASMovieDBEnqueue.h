#pragma once
#include "SandboxAppState.h"


namespace sandbox {
	// Enqueue TasMovie jobs into the DB (does not run them)
	void menu_tas_movie_db_enqueue(AppState& g);
}