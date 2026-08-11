#pragma once
#include <unordered_map>
#include <variant>
#include "CtxRegistry.h"
#include "../../Core/Input/GCInputFrame.h"
#include "../../Core/Input/SoaBattle/ActionTypes.h"

namespace savor {

	using PSValue = std::variant<uint8_t, uint16_t, uint32_t, float, double, std::string,
		GCInputFrame, soa::battle::actions::BattlePath>;

	class PSContext {
	public:
		using key_type = savor::context::key::KeyId;
		using map_type = std::unordered_map<key_type, PSValue>;
		using iterator = map_type::iterator;
		using const_iterator = map_type::const_iterator;

		// map-like API
		bool   empty() const noexcept { return kv_.empty(); }
		size_t size()  const noexcept { return kv_.size(); }
		void   clear()       noexcept { kv_.clear(); }

		iterator       begin()        noexcept { return kv_.begin(); }
		const_iterator begin()  const noexcept { return kv_.begin(); }
		const_iterator cbegin() const noexcept { return kv_.cbegin(); }
		iterator       end()          noexcept { return kv_.end(); }
		const_iterator end()    const noexcept { return kv_.end(); }
		const_iterator cend()   const noexcept { return kv_.cend(); }

		iterator find(key_type k) { return kv_.find(k); }
		const_iterator find(key_type k) const { return kv_.find(k); }

		template<class... Args>
		std::pair<iterator, bool> emplace(key_type k, Args&&... args) {
			return kv_.emplace(k, PSValue(std::forward<Args>(args)...));
		}

		PSValue& operator[](key_type k) { return kv_[k]; }

		// typed getter
		template <typename T>
		bool get(key_type k, T& out) const {
			auto it = kv_.find(k); if (it == kv_.end()) return false;
			if (auto p = std::get_if<T>(&it->second)) { out = *p; return true; }
			return false;
		}

		// erase helper
		size_t erase(key_type k) { return kv_.erase(k); }

	private:
		map_type kv_;
	};
}
