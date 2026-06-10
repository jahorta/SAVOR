#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace savor::hoststubs {

struct HostEvent {
	std::string name;
	std::string args_json;
};

using HostEventSink = std::function<void(const HostEvent&)>;

void SetHostEventSink(HostEventSink sink);
void ClearHostEventSink();
void EmitHostEvent(std::string_view event_name, std::string args_json = {});

} // namespace savor::hoststubs
