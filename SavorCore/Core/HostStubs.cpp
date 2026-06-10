// Minimal stubs to satisfy Dolphin Core when running headless.
#include "HostStubs.h"
#include "Core/Host.h"
#include "Core/System.h"
#include <mutex>
#include <string>

namespace {
std::mutex g_sink_mtx;
savor::hoststubs::HostEventSink g_sink;
}

namespace savor::hoststubs {
void SetHostEventSink(HostEventSink sink)
{
	std::lock_guard<std::mutex> lock(g_sink_mtx);
	g_sink = std::move(sink);
}

void ClearHostEventSink()
{
	std::lock_guard<std::mutex> lock(g_sink_mtx);
	g_sink = nullptr;
}

void EmitHostEvent(std::string_view event_name, std::string args_json)
{
	HostEventSink sink_copy;
	{
		std::lock_guard<std::mutex> lock(g_sink_mtx);
		sink_copy = g_sink;
	}
	if (sink_copy) {
		sink_copy(HostEvent{ std::string(event_name), std::move(args_json) });
	}
}
} // namespace savor::hoststubs

namespace {
std::string escape_json_string(const std::string& input)
{
	std::string out;
	out.reserve(input.size() + 8);
	for (const char c : input) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out.push_back(c); break;
		}
	}
	return out;
}
}

void Host_Message(HostMessageID id) { savor::hoststubs::EmitHostEvent("Host_Message", std::string("{\"id\":") + std::to_string(static_cast<int>(id)) + "}"); }
void Host_UpdateDisasmDialog() { savor::hoststubs::EmitHostEvent("Host_UpdateDisasmDialog"); }
void Host_UpdateMainFrame() { savor::hoststubs::EmitHostEvent("Host_UpdateMainFrame"); }
void Host_RefreshDSPDebuggerWindow() { savor::hoststubs::EmitHostEvent("Host_RefreshDSPDebuggerWindow"); }

bool Host_RendererHasFocus() { return true; }
bool Host_RendererHasFullFocus() { return true; }
bool Host_TASInputHasFocus() { return false; }
bool Host_UIBlocksControllerState() { return false; }
void Host_RequestRenderWindowSize(int width, int height) {
	savor::hoststubs::EmitHostEvent("Host_RequestRenderWindowSize",
		std::string("{\"width\":") + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}");
}

void Host_PPCSymbolsChanged() { savor::hoststubs::EmitHostEvent("Host_PPCSymbolsChanged"); }
void Host_JitCacheInvalidation() { savor::hoststubs::EmitHostEvent("Host_JitCacheInvalidation"); }
void Host_JitProfileDataWiped() { savor::hoststubs::EmitHostEvent("Host_JitProfileDataWiped"); }
void Host_PPCBreakpointsChanged() { savor::hoststubs::EmitHostEvent("Host_PPCBreakpointsChanged"); }

void Host_UpdateTitle(Core::System&, const std::string& title) {
	savor::hoststubs::EmitHostEvent("Host_UpdateTitleWithSystem", std::string("{\"title\":\"") + escape_json_string(title) + "\"}");
}
void Host_UpdateTitle(const std::string& title) {
	savor::hoststubs::EmitHostEvent("Host_UpdateTitle", std::string("{\"title\":\"") + escape_json_string(title) + "\"}");
}
void Host_TitleChanged() { savor::hoststubs::EmitHostEvent("Host_TitleChanged"); }
void Host_YieldToUI() { savor::hoststubs::EmitHostEvent("Host_YieldToUI"); };

std::vector<std::string> Host_GetPreferredLocales() {
	return {};
}

void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&,
	const std::string&, const std::string&,
	const std::string&, const std::string&,
	std::int64_t, std::int64_t) {
	return false;
}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&,
	const std::string&, const std::string&,
	const std::string&, const std::string&,
	std::int64_t, std::int64_t, int, int) {
	return false;
}
