#pragma once
#include "imgui.h"
#include <optional>
#include <vector>
#include <future>
#include "DB/Querying/JobListDTO.h"
#include "DB/Querying/JobEventListDTO.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/DataService.h"
#include "DB/Querying/SnapshotMailbox.h"
#include "Utils/IniDoc.h"

struct JobDetailsDrawer {
    // Draws the right-side drawer. Uses lazy fetches. Returns whether still open.
    static bool Draw(const JobLite& job, int& active_tab, std::unordered_map<int, std::string>& program_names);
};
