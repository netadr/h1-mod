#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include "scheduler.hpp"
#include "command.hpp"

#include "ui_scripting.hpp"

#include "game/ui_scripting/lua/engine.hpp"
#include "game/ui_scripting/execution.hpp"
#include "game/ui_scripting/lua/error.hpp"

#include <utils/string.hpp>
#include <utils/hook.hpp>

namespace ui_scripting
{
	namespace
	{
		std::unordered_map<game::hks::cclosure*, sol::protected_function> converted_functions;

		utils::hook::detour hksi_lual_error_hook;
		utils::hook::detour hksi_lual_error_hook2;
		utils::hook::detour hks_start_hook;
		utils::hook::detour hks_shutdown_hook;
		utils::hook::detour hks_allocator_hook;
		utils::hook::detour hks_frame_hook;
		utils::hook::detour lui_error_hook;
		utils::hook::detour hksi_hks_error_hook;

		bool error_hook_enabled = false;

		void hksi_lual_error_stub(game::hks::lua_State* s, const char* fmt, ...)
		{
			char va_buffer[2048] = {0};

			va_list ap;
			va_start(ap, fmt);
			vsprintf_s(va_buffer, fmt, ap);
			va_end(ap);

			const auto formatted = std::string(va_buffer);

			if (!error_hook_enabled)
			{
				return hksi_lual_error_hook.invoke<void>(s, formatted.data());
			}

			throw std::runtime_error(formatted);
		}

		int hksi_hks_error_stub(game::hks::lua_State* s, int a2)
		{
			if (!error_hook_enabled)
			{
				return hksi_hks_error_hook.invoke<int>(s, a2);
			}

			throw std::runtime_error("unknown error");
		}

		void lui_error_stub(game::hks::lua_State* s)
		{
			if (!error_hook_enabled)
			{
				lui_error_hook.invoke<void>(s);
			}

			const auto count = static_cast<int>(s->m_apistack.top - s->m_apistack.base);
			const auto arguments = get_return_values(count);

			std::string error_str = "LUI Error";
			if (count && arguments[0].is<std::string>())
			{
				error_str = arguments[0].as<std::string>();
			}

			throw std::runtime_error(error_str);
		}

		void* hks_start_stub(char a1)
		{
			const auto _1 = gsl::finally([]()
			{
				ui_scripting::lua::engine::start();
			});

			return hks_start_hook.invoke<void*>(a1);
		}

		void hks_shutdown_stub()
		{
			ui_scripting::lua::engine::stop();
			hks_shutdown_hook.invoke<void*>();
		}

		void* hks_allocator_stub(void* userData, void* oldMemory, unsigned __int64 oldSize, unsigned __int64 newSize)
		{
			const auto closure = reinterpret_cast<game::hks::cclosure*>(oldMemory);
			if (converted_functions.find(closure) != converted_functions.end())
			{
				converted_functions.erase(closure);
			}

			return hks_allocator_hook.invoke<void*>(userData, oldMemory, oldSize, newSize);
		}

		void hks_frame_stub()
		{
			const auto state = *game::hks::lua_state;
			if (state)
			{
				ui_scripting::lua::engine::run_frame();
			}
		}
	}

	int main_function_handler(game::hks::lua_State* state)
	{
		const auto value = state->m_apistack.base[-1];
		if (value.t != game::hks::TCFUNCTION)
		{
			return 0;
		}

		const auto closure = reinterpret_cast<game::hks::cclosure*>(value.v.cClosure);
		if (converted_functions.find(closure) == converted_functions.end())
		{
			return 0;
		}

		const auto function = converted_functions[closure];
		const auto count = static_cast<int>(state->m_apistack.top - state->m_apistack.base);
		const auto arguments = get_return_values(count);
		const auto s = function.lua_state();

		std::vector<sol::lua_value> converted_args;

		for (const auto& argument : arguments)
		{
			converted_args.push_back(lua::convert(s, argument));
		}

		const auto results = function(sol::as_args(converted_args));
		lua::handle_error(results);

		for (const auto& result : results)
		{
			push_value(lua::convert({s, result}));
		}

		return results.return_count();
	}

	void add_converted_function(game::hks::cclosure* closure, const sol::protected_function& function)
	{
		converted_functions[closure] = function;
	}

	void clear_converted_functions()
	{
		converted_functions.clear();
	}

	void enable_error_hook()
	{
		error_hook_enabled = true;
	}

	void disable_error_hook()
	{
		error_hook_enabled = false;
	}

	class component final : public component_interface
	{
	public:

		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			hks_start_hook.create(SELECT_VALUE(0x1400E4B40, 0x140176A40), hks_start_stub);
			hks_shutdown_hook.create(SELECT_VALUE(0x1400DD3D0, 0x14016CA80), hks_shutdown_stub);
			hksi_lual_error_hook.create(SELECT_VALUE(0x1400A5EA0, 0x14012F300), hksi_lual_error_stub);
			hks_allocator_hook.create(SELECT_VALUE(0x14009B570, 0x14012BAC0), hks_allocator_stub);
			hks_frame_hook.create(SELECT_VALUE(0x1400E37F0, 0x1401755B0), hks_frame_stub);
			lui_error_hook.create(SELECT_VALUE(0x14007D7D0, 0x14010C9E0), lui_error_stub);
			hksi_hks_error_hook.create(SELECT_VALUE(0x14009DD80, 0x14012E390), hksi_hks_error_stub);

			if (game::environment::is_mp())
			{
				hksi_lual_error_hook2.create(0x1401366B0, hksi_lual_error_stub);
			}

			command::add("lui_restart", []()
			{
				utils::hook::invoke<void>(SELECT_VALUE(0x1400DD3D0, 0x14016CA80));
				utils::hook::invoke<void>(SELECT_VALUE(0x1400E6170, 0x1401780D0));
			});
		}
	};
}

REGISTER_COMPONENT(ui_scripting::component)