#pragma once
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <string>
#include "ContextKeys/KeyIds.h"
#include "ContextKeys/VMCoreKeys.reg.h"
#include "ContextKeys/SeedProbeKeys.reg.h"
#include "ContextKeys/TasMovieKeys.reg.h"
#include "ContextKeys/TasFrameDetectorKeys.reg.h"
#include "ContextKeys/BattleRunnerKeys.reg.h"
#include "ContextKeys/NavigationContextKeys.reg.h"

namespace savor::context::key {

	class CtxRegistry {
	public:
		static std::string_view name_for_id(KeyId id);
		static bool id_for_name(std::string_view name, KeyId& out);
		static const KeyPair* all_keys(size_t& out_count);
		static uint32_t registry_hash();
		static bool validate_registry(std::string* err_out = nullptr);
	};

	inline std::string_view name_for_id(KeyId id) { return CtxRegistry::name_for_id(id); }
	inline bool id_for_name(std::string_view name, KeyId& out) { return CtxRegistry::id_for_name(name, out); }
	inline const KeyPair* all_keys(size_t& out_count) { return CtxRegistry::all_keys(out_count); }
	inline uint32_t registry_hash() { return CtxRegistry::registry_hash(); }
	inline bool validate_registry(std::string* err_out = nullptr) { return CtxRegistry::validate_registry(err_out); }

} // namespace savor::context::key
