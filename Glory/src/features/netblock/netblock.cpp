
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <fwpmu.h>
#include <objbase.h>

#include "netblock.h"

#include <globals.h>
#include <game/game.h>
#include <features/stabilizer/stabilizer.h>
#include <features/macros/macros.h>
#include <logger/logger.h>
#include <memory/memory.h>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <render/render.h>
#include <timing/precise_sleep.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "fwpuclnt.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")

using namespace std::chrono_literals;

namespace
{
	using steady = std::chrono::steady_clock;

	constexpr steady::time_point never{};

	// swap a port from network to host byte order
	std::uint16_t host_order(DWORD network_order_port)
	{
		return _byteswap_ushort(static_cast<std::uint16_t>(network_order_port));
	}

	// read the systems udp socket table
	std::vector<std::uint8_t> udp_table(ULONG family)
	{
		std::vector<std::uint8_t> buffer;
		ULONG size = 0;

		for (int attempt = 0; attempt < 5; attempt++)
		{
			const DWORD result = GetExtendedUdpTable(buffer.empty() ? nullptr : buffer.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
			if (result == NO_ERROR && !buffer.empty())
				return buffer;

			if (result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR)
				break;

			buffer.assign(static_cast<std::size_t>(size) + 512, 0);
			size = static_cast<ULONG>(buffer.size());
		}

		return {};
	}

	struct udp_port_t
	{
		std::uint16_t port = 0;

		bool v6 = false;
	};

	// order two ports (sort helper)
	bool lower_port(const udp_port_t& a, const udp_port_t& b)
	{
		return a.port < b.port;
	}

	// the udp ports a process has open
	std::vector<udp_port_t> udp_ports_of(std::uint32_t process_id)
	{
		std::vector<udp_port_t> ports;

		if (process_id == 0)
			return ports;

		const std::vector<std::uint8_t> v4 = udp_table(AF_INET);
		if (!v4.empty())
		{
			const MIB_UDPTABLE_OWNER_PID* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(v4.data());
			for (DWORD i = 0; i < table->dwNumEntries; i++)
			{
				const MIB_UDPROW_OWNER_PID& row = table->table[i];

				if (row.dwOwningPid == process_id && (row.dwLocalAddr & 0xFF) != 127)
				{
					ports.push_back({ host_order(row.dwLocalPort), false });
				}
			}
		}

		const std::vector<std::uint8_t> v6 = udp_table(AF_INET6);
		if (!v6.empty())
		{

			static const UCHAR loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

			const MIB_UDP6TABLE_OWNER_PID* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(v6.data());
			for (DWORD i = 0; i < table->dwNumEntries; i++)
			{
				const MIB_UDP6ROW_OWNER_PID& row = table->table[i];

				if (row.dwOwningPid == process_id && std::memcmp(row.ucLocalAddr, loopback, sizeof(loopback)) != 0)
				{
					ports.push_back({ host_order(row.dwLocalPort), true });
				}
			}
		}

		std::sort(ports.begin(), ports.end(), [](const udp_port_t& a, const udp_port_t& b) { return a.port != b.port ? a.port < b.port : a.v6 && !b.v6; });
		ports.erase(std::unique(ports.begin(), ports.end(), [](const udp_port_t& a, const udp_port_t& b) { return a.port == b.port; }), ports.end());
		ports.erase(std::remove_if(ports.begin(), ports.end(), [](const udp_port_t& entry) { return entry.port == 0; }), ports.end());
		return ports;
	}

	class packet_hold_t final
	{
	public:
		packet_hold_t() = default;
		~packet_hold_t() { close(); }

		packet_hold_t(const packet_hold_t&) = delete;
		packet_hold_t& operator=(const packet_hold_t&) = delete;

		// add wfp filters to block a process's udp ports
		bool hold(std::uint32_t process_id)
		{
			release();

			if (!open())
				return false;

			const std::vector<udp_port_t> ports = udp_ports_of(process_id);
			if (ports.empty())
			{
				LOG_WARN("lag switch: process {} has no UDP socket to a server, there is nothing to hold", process_id);
				return false;
			}

			DWORD result = FwpmTransactionBegin0(engine, 0);
			if (result != ERROR_SUCCESS)
			{
				LOG_ERROR("lag switch: Windows would not begin adding filters (error 0x{:08X})", result);
				close();
				return false;
			}

			for (const udp_port_t& owned : ports)
			{
				held_t entry;
				entry.port = owned.port;
				entry.has_v6 = owned.v6;

				result = add_filter(FWPM_LAYER_OUTBOUND_TRANSPORT_V4, owned.port, entry.v4);
				if (result == ERROR_SUCCESS && owned.v6)
				{
					result = add_filter(FWPM_LAYER_OUTBOUND_TRANSPORT_V6, owned.port, entry.v6);
				}

				if (result != ERROR_SUCCESS)
				{
					LOG_ERROR("lag switch: Windows refused the filter for port {} (error 0x{:08X})", owned.port, result);
					FwpmTransactionAbort0(engine);
					held.clear();
					return false;
				}

				held.push_back(entry);
			}

			result = FwpmTransactionCommit0(engine);
			if (result != ERROR_SUCCESS)
			{
				LOG_ERROR("lag switch: Windows would not put the filters in place (error 0x{:08X})", result);
				held.clear();
				close();
				return false;
			}

			owner = process_id;
			return true;
		}

		// remove the wfp block filters
		void release()
		{
			if (held.empty())
				return;

			bool removed = FwpmTransactionBegin0(engine, 0) == ERROR_SUCCESS;
			if (removed)
			{
				for (const held_t& entry : held)
				{
					removed = removed && remove_filters(entry);
				}

				removed = removed && FwpmTransactionCommit0(engine) == ERROR_SUCCESS;
			}

			held.clear();
			owner = 0;

			if (!removed)
			{

				LOG_WARN("lag switch: a filter could not be taken away one by one, so all of them are, by closing the session");
				close();
			}
		}

		// are still held.
		std::size_t drop_stale_ports()
		{
			if (held.empty())
				return 0;

			const std::vector<udp_port_t> now_owned = udp_ports_of(owner);

			for (std::size_t i = 0; i < held.size();)
			{
				if (std::binary_search(now_owned.begin(), now_owned.end(), udp_port_t{ held[i].port }, lower_port))
				{
					i++;
					continue;
				}

				LOG_INFO("lag switch: the game let go of port {}, so does the switch", held[i].port);
				if (!remove_filters(held[i]))
				{
					held.clear();
					owner = 0;
					close();
					return 0;
				}

				held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
			}

			if (held.empty())
			{
				owner = 0;
			}

			return held.size();
		}

		// true if any block filters are active
		bool holding() const { return !held.empty(); }

		// readable list of the blocked ports
		std::string ports_text() const
		{
			std::string text;
			for (const held_t& entry : held)
			{
				text += (text.empty() ? "" : ", ") + std::to_string(entry.port);
			}
			return text;
		}

		// close the wfp engine handle
		void close()
		{
			held.clear();
			owner = 0;

			if (engine != nullptr)
			{
				FwpmEngineClose0(engine);
				engine = nullptr;
			}
		}

	private:
		struct held_t
		{
			std::uint16_t port = 0;

			UINT64 v4 = 0;
			UINT64 v6 = 0;
			bool has_v6 = false;
		};

		// open a wfp engine session
		bool open()
		{
			if (engine != nullptr)
				return true;

			FWPM_SESSION0 session{};
			session.flags = FWPM_SESSION_FLAG_DYNAMIC;
			session.displayData.name = const_cast<wchar_t*>(L"Glory lag switch");

			DWORD result = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, &session, &engine);
			if (result != ERROR_SUCCESS)
			{
				engine = nullptr;
				LOG_ERROR("lag switch: the Windows Filtering Platform could not be opened (error 0x{:08X}). It needs this program to run as administrator and the Base Filtering Engine service to be running", result);
				return false;
			}

			if (CoCreateGuid(&sublayer) != S_OK)
			{
				close();
				return false;
			}

			FWPM_SUBLAYER0 layer{};
			layer.subLayerKey = sublayer;
			layer.displayData.name = const_cast<wchar_t*>(L"Glory lag switch");
			layer.weight = 0xFFFF;

			result = FwpmSubLayerAdd0(engine, &layer, nullptr);
			if (result != ERROR_SUCCESS)
			{
				LOG_ERROR("lag switch: Windows refused the sublayer (error 0x{:08X})", result);
				close();
				return false;
			}

			return true;
		}

		// add one wfp block filter for a port
		DWORD add_filter(const GUID& layer, std::uint16_t port, UINT64& id)
		{
			FWPM_FILTER_CONDITION0 conditions[3]{};

			conditions[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
			conditions[0].matchType = FWP_MATCH_EQUAL;
			conditions[0].conditionValue.type = FWP_UINT8;
			conditions[0].conditionValue.uint8 = IPPROTO_UDP;

			conditions[1].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
			conditions[1].matchType = FWP_MATCH_EQUAL;
			conditions[1].conditionValue.type = FWP_UINT16;
			conditions[1].conditionValue.uint16 = port;

			conditions[2].fieldKey = FWPM_CONDITION_FLAGS;
			conditions[2].matchType = FWP_MATCH_FLAGS_NONE_SET;
			conditions[2].conditionValue.type = FWP_UINT32;
			conditions[2].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

			FWPM_FILTER0 filter{};
			filter.displayData.name = const_cast<wchar_t*>(L"Glory lag switch");
			filter.layerKey = layer;
			filter.subLayerKey = sublayer;
			filter.weight.type = FWP_EMPTY;
			filter.numFilterConditions = static_cast<UINT32>(std::size(conditions));
			filter.filterCondition = conditions;
			filter.action.type = FWP_ACTION_BLOCK;

			return FwpmFilterAdd0(engine, &filter, nullptr, &id);
		}

		// remove a blocked entrys filters
		bool remove_filters(const held_t& entry)
		{
			const DWORD first = FwpmFilterDeleteById0(engine, entry.v4);
			const DWORD second = entry.has_v6 ? FwpmFilterDeleteById0(engine, entry.v6) : static_cast<DWORD>(ERROR_SUCCESS);

			const auto gone = [](DWORD result) { return result == ERROR_SUCCESS || result == static_cast<DWORD>(FWP_E_FILTER_NOT_FOUND); };
			return gone(first) && gone(second);
		}

		HANDLE engine = nullptr;
		GUID sublayer{};
		std::vector<held_t> held;

		std::uint32_t owner = 0;
	};

	enum class let_go
	{
		key,
		time_limit,
		could_not_hold
	};

	constexpr auto pause_between = 300ms;

	struct lag_clock_t
	{
		bool holding = false;
		steady::time_point since = never;
		steady::time_point let_go_at = never;

		bool spent = false;

		// true if the lag switch should start now
		bool should_begin(bool asked, steady::time_point now)
		{
			if (!asked)
			{
				spent = false;
				return false;
			}

			return !holding && !spent && (let_go_at == never || now - let_go_at >= pause_between);
		}

		// true if the lag switch should stop now
		bool should_end(bool asked, steady::time_point now, float max_seconds, let_go& why) const
		{
			if (!holding)
				return false;

			if (!asked)
			{
				why = let_go::key;
				return true;
			}

			if (std::chrono::duration<float>(now - since).count() >= max_seconds)
			{
				why = let_go::time_limit;
				return true;
			}

			return false;
		}

		// note the lag switch started
		void began(steady::time_point now)
		{
			holding = true;
			since = now;
		}

		// note it stopped, with the reason
		void ended(steady::time_point now, let_go why)
		{
			holding = false;
			let_go_at = now;
			spent = why != let_go::key;
		}
	};

	struct state_t
	{
		math::matrix3 rotation;
		math::vector3 position;
		math::vector3 linear;
		math::vector3 angular;
	};

	static_assert(sizeof(state_t) == 72 &&
		Offsets::Primitive::Position == Offsets::Primitive::Rotation + sizeof(math::matrix3) &&
		Offsets::Primitive::AssemblyLinearVelocity == Offsets::Primitive::Position + sizeof(math::vector3) &&
		Offsets::Primitive::AssemblyAngularVelocity == Offsets::Primitive::AssemblyLinearVelocity + sizeof(math::vector3),
		"heading, place and the two velocities are read and written as one block");

	constexpr std::uint64_t job_running = 0x60;
	constexpr std::size_t job_search = 0x300;

	std::atomic<int> crouch_presses{ 0 };

	struct truss_t
	{
		math::matrix3 rotation;
		math::vector3 position;
		math::vector3 half;
	};

	std::vector<truss_t> trusses;
	steady::time_point trusses_found{};
	std::uint32_t trusses_session = 0;

	// find climbable truss/ladder parts in the workspace
	void find_trusses(steady::time_point now)
	{
		if (trusses_session == game::session && trusses_found != steady::time_point{} && now - trusses_found < 30s)
			return;

		const std::uint64_t workspace = game::workspace.address;
		if (workspace == 0 || game::game_changed)
			return;

		const bool new_game = trusses_session != game::session;
		trusses_session = game::session;
		trusses_found = now;

		const std::uint64_t world = memory->read<std::uint64_t>(workspace + Offsets::Workspace::World);
		const std::uint64_t first = world != 0 ? memory->read<std::uint64_t>(world + Offsets::World::Primitives) : 0;
		const std::uint64_t last = world != 0 ? memory->read<std::uint64_t>(world + Offsets::World::Primitives + sizeof(std::uint64_t)) : 0;
		if (first == 0 || last <= first || (last - first) % 8 != 0 || last - first > 8ull * 4000000)
			return;

		std::vector<std::uint64_t> primitives((last - first) / 8);
		memory->read_buffer(first, primitives.data(), last - first);

		static std::unordered_map<std::uint64_t, bool> is_truss;
		if (new_game)
		{
			is_truss.clear();
		}

		std::vector<truss_t> found;
		for (const std::uint64_t primitive : primitives)
		{
			if (primitive == 0)
				continue;

			const math::vector3 size = memory->read<math::vector3>(primitive + Offsets::Primitive::Size);
			if (std::abs(size.x - 2.0f) > 0.01f || std::abs(size.z - 2.0f) > 0.01f || !(size.y >= 2.0f && size.y < 2048.0f))
				continue;

			const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
			const std::uint64_t descriptor = part != 0 ? memory->read<std::uint64_t>(part + Offsets::Instance::ClassDescriptor) : 0;
			if (descriptor == 0)
				continue;

			auto known = is_truss.find(descriptor);
			if (known == is_truss.end())
			{
				const std::uint64_t name = memory->read<std::uint64_t>(descriptor + Offsets::Instance::ClassName);
				known = is_truss.emplace(descriptor, name != 0 && memory->read_string(name) == "TrussPart").first;
			}
			if (!known->second)
				continue;

			truss_t truss{};
			truss.rotation = memory->read<math::matrix3>(primitive + Offsets::Primitive::Rotation);
			truss.position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			truss.half = size * 0.5f;

			if (truss.rotation.m[1][1] > 0.9f && std::isfinite(truss.position.x))
			{
				found.push_back(truss);
			}
		}

		if (new_game || found.size() != trusses.size())
		{
			LOG_INFO("lag switch: {} ladder{} found for climbing while it holds", found.size(), found.size() == 1 ? "" : "s");
		}
		trusses = std::move(found);
	}

	// true if a ladder is at this spot, and which way it faces
	bool ladder_at(const math::vector3& at, math::vector3& toward, float& above_top)
	{
		for (const truss_t& truss : trusses)
		{
			const math::vector3 d{ at.x - truss.position.x, at.y - truss.position.y, at.z - truss.position.z };
			const math::matrix3& m = truss.rotation;
			const float lx = m.m[0][0] * d.x + m.m[1][0] * d.y + m.m[2][0] * d.z;
			const float ly = m.m[0][1] * d.x + m.m[1][1] * d.y + m.m[2][1] * d.z;
			const float lz = m.m[0][2] * d.x + m.m[1][2] * d.y + m.m[2][2] * d.z;

			const float out_x = (std::max)(std::abs(lx) - truss.half.x, 0.0f);
			const float out_z = (std::max)(std::abs(lz) - truss.half.z, 0.0f);
			if (std::hypot(out_x, out_z) > 1.75f || ly < -truss.half.y - 2.0f || ly > truss.half.y + 3.0f)
				continue;

			const float length = std::hypot(d.x, d.z);
			toward = length > 0.001f ? math::vector3{ -d.x / length, 0.0f, -d.z / length } : math::vector3{};
			above_top = ly - truss.half.y;
			return true;
		}
		return false;
	}

	class ghost_t final
	{
	public:

		// start a hold: find the scheduler jobs and freeze the character
		bool begin()
		{
			heartbeat = 0;
			sender = 0;
			root = 0;

			const std::uint64_t base = memory->get_module_address();
			if (base == 0)
				return false;

			const std::uint64_t fake = memory->read<std::uint64_t>(base + Offsets::FakeDataModel::Pointer);
			const std::uint64_t real = fake != 0 ? memory->read<std::uint64_t>(fake + Offsets::FakeDataModel::RealDataModel) : 0;

			const std::uint64_t scheduler = memory->read<std::uint64_t>(base + Offsets::TaskScheduler::Pointer);
			const std::uint64_t first = scheduler != 0 ? memory->read<std::uint64_t>(scheduler + Offsets::TaskScheduler::JobStart) : 0;
			const std::uint64_t last = scheduler != 0 ? memory->read<std::uint64_t>(scheduler + Offsets::TaskScheduler::JobEnd) : 0;

			if (first == 0 || last <= first || last - first > 16 * 4096 || (real == 0 && fake == 0))
			{
				LOG_WARN("lag switch: the game's list of jobs was not found (scheduler 0x{:X})", scheduler);
				return false;
			}

			for (std::uint64_t slot = first; slot < last; slot += 16)
			{
				const std::uint64_t job = memory->read<std::uint64_t>(slot);
				if (job == 0)
					continue;

				const std::string name = memory->read_string(job + Offsets::TaskScheduler::JobName);
				const bool is_heartbeat = name == "Heartbeat";
				if (!is_heartbeat && name != "Allocate Bandwidth and Run Senders")
					continue;

				std::uint64_t words[job_search / 8]{};
				memory->read_buffer(job, words, sizeof(words));

				bool of_game = false;
				for (const std::uint64_t word : words)
				{
					of_game = of_game || (word != 0 && (word == real || word == fake));
				}

				if (of_game)
				{
					(is_heartbeat ? heartbeat : sender) = job;
				}
			}

			if (heartbeat == 0 || sender == 0)
			{
				LOG_WARN("lag switch: the game's {} job was not found", heartbeat == 0 ? "Heartbeat" : "sender");
				return false;
			}

			const std::uint64_t model = game::local_player.address != 0 ? memory->read<std::uint64_t>(game::local_player.address + Offsets::Player::ModelInstance) : 0;
			const std::uint64_t part = model != 0 ? rbx::instance_t{ model }.find_first_child("HumanoidRootPart").address : 0;
			root = part != 0 ? memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) : 0;

			if (root == 0 || !alive())
			{
				LOG_WARN("lag switch: there is no character to hold in place right now");
				root = 0;
				return false;
			}

			memory->read_buffer(root + Offsets::Primitive::Rotation, &held, sizeof(held));
			held.linear = {};
			held.angular = {};

			if (!std::isfinite(held.position.x) || !std::isfinite(held.position.y) || !std::isfinite(held.position.z))
			{
				LOG_WARN("lag switch: the character's place could not be read");
				root = 0;
				return false;
			}

			humanoid = rbx::model_instance_t{ model }.find_first_child_by_class("Humanoid").address;
			start_glide();

			rounds = 0;
			missed = 0;
			late = 0;
			lifts = 0;
			longest = 0.0f;
			skipped = 0;
			last_round = never;
			usual = 0.0f;
			return true;
		}

		// one hold cycle: write the frozen state during heartbeat, then restore
		bool round()
		{
			const steady::time_point began = steady::now();

			while (!running(heartbeat))
			{
				if (keeping)
				{
					keep_on();
				}
				if (steady::now() - began > 50ms)
					return alive();
			}

			state_t real{};
			netblock::held_on_part = true;
			memory->read_buffer(root + Offsets::Primitive::Rotation, &real, sizeof(real));
			memory->write_buffer(root + Offsets::Primitive::Rotation, &held, sizeof(held));

			const steady::time_point heartbeat_began = steady::now();
			steady::time_point written = heartbeat_began;
			bool lifted = false;
			while (running(heartbeat) && steady::now() - heartbeat_began < 20ms)
			{
				if (!lifted && steady::now() - written > lift_after)
				{
					put_back(real, false);
					netblock::held_on_part = false;
					lifted = true;
					lifts++;
				}
			}

			if (lifted)
			{

				netblock::held_on_part = true;
				memory->read_buffer(root + Offsets::Primitive::Rotation, &real, sizeof(real));
				memory->write_buffer(root + Offsets::Primitive::Rotation, &held, sizeof(held));
				written = steady::now();
			}

			const steady::time_point ended = steady::now();
			bool seen = false;
			while (!seen && steady::now() - ended < 30us)
			{
				seen = running(sender);
			}

			while (seen && running(sender) && steady::now() - ended < 5ms)
			{
			}

			put_back(real, true);
			netblock::held_on_part = false;
			after_restore();

			const steady::time_point restored = steady::now();
			const float on_part = std::chrono::duration<float, std::milli>(restored - written).count();
			longest = (std::max)(longest, on_part);
			if (on_part > 2.5f)
			{
				late++;
			}

			if (last_round != never)
			{
				const float apart = std::chrono::duration<float, std::milli>(written - last_round).count();
				if (usual > 0.0f && apart > usual * 1.8f)
				{
					skipped++;
				}
				usual = usual > 0.0f ? usual * 0.95f + (std::min)(apart, usual * 2.0f) * 0.05f : apart;
			}
			last_round = written;

			rounds++;
			if (!seen)
			{
				missed++;
			}

			prepare();

			return rounds % 64 != 0 || alive();
		}

		// restore the characters real state (optionally glided)
		void put_back(const state_t& real, bool with_glide)
		{
			state_t now{};
			memory->read_buffer(root + Offsets::Primitive::Rotation, &now, sizeof(now));
			if (std::memcmp(&now.position, &held.position, sizeof(held.position)) != 0)
			{

				keeping = false;
				return;
			}

			state_t out = real;
			if (std::memcmp(&now.rotation, &held.rotation, sizeof(held.rotation)) != 0)
			{
				out.rotation = now.rotation;
			}
			if (std::memcmp(&now.linear, &held.linear, sizeof(held.linear)) != 0)
			{
				out.linear = now.linear;
			}
			if (std::memcmp(&now.angular, &held.angular, sizeof(held.angular)) != 0)
			{
				out.angular = now.angular;
			}

			if (with_glide)
			{
				apply(out, steady::now());
			}
			memory->write_buffer(root + Offsets::Primitive::Rotation, &out, sizeof(out));
		}

		// restore walkspeed, camera and humanoid states
		void give_back()
		{
			if (walk_held && memory->read<float>(humanoid + Offsets::Humanoid::Walkspeed) == 0.0f)
			{
				memory->write<float>(humanoid + Offsets::Humanoid::Walkspeed, walk_speed);
				memory->write<float>(humanoid + Offsets::Humanoid::WalkspeedCheck, walk_speed);
			}

			if (offset_known)
			{
				memory->write<math::vector3>(humanoid + Offsets::Humanoid::CameraOffset, saved_offset);
			}
			if (hip_known && hip_written != saved_hip)
			{
				memory->write<float>(humanoid + Offsets::Humanoid::HipHeight, saved_hip);
				hip_written = saved_hip;
			}
			std::uint8_t row[state_count]{};
			memory->read_buffer(state_row(), row, sizeof(row));
			if (states_saved && row_looks_right(row))
			{
				for (std::size_t i = 0; i < std::size(still_states); i++)
				{
					if (row[still_states[i]] == 0)
					{
						memory->write<std::uint8_t>(state_row() + still_states[i], saved_states[i]);
					}
				}
			}
		}

		// end the hold and log what happened
		void finish_glide()
		{
			netblock::gliding = false;
			netblock::lifting = false;

			if (humanoid != 0 && root != 0 && alive() && !handed)
			{
				give_back();
			}

			if (glided > 0 || climbed > 0 || macros_turns > 0)
			{

				const math::vector3 at = root != 0 && alive() ? memory->read<math::vector3>(root + Offsets::Primitive::Position) : held.position;
				LOG_INFO("lag switch: the character glided {} rounds of the game on this screen ({:.1f} s of moving at {:.0f} studs a second), {} jumps (the highest {:.1f} studs up, at most {:.1f} studs off a jump's path, hip height {:.1f} on the ground), {} crouches or stand-ups, {:.1f} s on a ladder; kept on while moving, the physics put its own place back {} times ({} of them a wall), the humanoid out of its running state in {} rounds (last in state {}), the glitch macro had the character {} times, and ended {:.0f} studs from where it was held and {:+.0f} up",
					glided, glided * (usual > 0.0f ? usual : 4.2f) / 1000.0f, walk_speed, jumps, highest_jump, furthest_off, saved_hip, crouches, climbed * (usual > 0.0f ? usual : 4.2f) / 1000.0f,
					put_backs, walls, other_states, last_other, macros_turns,
					std::hypot(at.x - held.position.x, at.z - held.position.z), at.y - held.position.y);
			}

			humanoid = 0;
			walk_held = false;
			states_saved = false;
			keeping = false;
		}

		// the held position
		math::vector3 place() const { return held.position; }
		int rounds = 0;

		int missed = 0;

		int late = 0;
		float longest = 0.0f;
		int skipped = 0;

		int lifts = 0;

	private:
		static constexpr auto lift_after = 1ms;

		// true if a scheduler job is currently running
		bool running(std::uint64_t job) const
		{
			return memory->read<std::uint8_t>(job + job_running) == 1;
		}

		// true if the held part is still the characters root
		bool alive() const
		{
			const std::uint64_t part = memory->read<std::uint64_t>(root + Offsets::Primitive::Owner);
			return part != 0 && memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) == root;
		}

		static constexpr int state_count = 19;

		static constexpr int still_states[] = { 3, 4, 5, 12 };

		static constexpr int material_air = 1792;

		// address of the humanoid state row
		std::uint64_t state_row() const
		{
			return humanoid + Offsets::Humanoid::IsWalking - 0x13;
		}

		// sanity check the humanoid state bytes
		static bool row_looks_right(const std::uint8_t (&row)[state_count])
		{
			int on = 0;
			for (int i = 0; i < state_count; i++)
			{
				if (row[i] > 1)
					return false;
				if (i <= 16)
					on += row[i];
			}
			return on >= 6;
		}

		// reset glide state and capture the hold
		void start_glide()
		{
			walk_held = false;
			states_saved = false;
			last_glide = never;
			last_jump = never;
			centred = 0;
			glided = 0;
			jumps = 0;
			has_target = false;
			crouch_seen = crouch_presses.load();
			crouched = false;
			really_crouched = false;
			offset_known = false;
			crouches = 0;
			climb_known = false;
			climbed = 0;
			airborne = false;
			netblock::lifting = false;
			hip_known = false;
			highest_jump = 0.0f;
			furthest_off = 0.0f;
			landed = never;
			keeping = false;
			put_backs = 0;
			walls = 0;
			other_states = 0;
			last_other = -1;
			handed = false;
			macros_turns = 0;
			intent = {};

			const std::uint64_t state = humanoid != 0 ? memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::HumanoidState) : 0;
			climbing = state != 0 && memory->read<std::uint16_t>(state + Offsets::Humanoid::HumanoidStateID) == 12;

			if (humanoid == 0)
			{
				LOG_WARN("lag switch: the character has no humanoid to be found, its walking still shows to the others");
				return;
			}

			take_hold();
			netblock::gliding = true;

			prepare();
		}

		// save walkspeed, crouch, camera and states before holding
		void take_hold()
		{
			const float walk = memory->read<float>(humanoid + Offsets::Humanoid::Walkspeed);
			walk_speed = std::isfinite(walk) && walk > 0.01f ? walk : 16.0f;

			really_crouched = walk_speed < 14.0f || (GetAsyncKeyState('C') & 0x8000) != 0;
			crouched = really_crouched;
			saved_offset = memory->read<math::vector3>(humanoid + Offsets::Humanoid::CameraOffset);
			offset_known = std::isfinite(saved_offset.x) && std::isfinite(saved_offset.y) && std::isfinite(saved_offset.z);
			saved_hip = memory->read<float>(humanoid + Offsets::Humanoid::HipHeight);
			hip_known = std::isfinite(saved_hip) && saved_hip >= 0.0f && saved_hip < 50.0f;
			hip_target = saved_hip;
			hip_written = saved_hip;
			if (!hip_known)
			{
				LOG_WARN("lag switch: the humanoid's hip height reads {}, which no character has (the offset may have moved): Space does not jump while it holds", saved_hip);
			}

			LOG_INFO("lag switch: the character is {} (walk speed {:.0f}, camera offset {:.1f} up){}", really_crouched ? "crouched" : "standing", walk_speed,
				offset_known ? saved_offset.y : 0.0f, really_crouched ? ": the others keep seeing it crouched, C stands it up on this screen" : "");

			std::uint8_t row[state_count]{};
			memory->read_buffer(state_row(), row, sizeof(row));
			if (row_looks_right(row))
			{
				for (std::size_t i = 0; i < std::size(still_states); i++)
				{
					saved_states[i] = row[still_states[i]];
				}
				states_saved = true;
			}
			else
			{
				LOG_WARN("lag switch: the humanoid's states are not where they were found before (the offsets may have moved), jumping and falling still show to the others");
			}
		}

		// true if the mouse is locked to the screen centre
		bool mouse_locked()
		{
			const HWND window = game::get_roblox_window();
			RECT client{};
			POINT cursor{};
			if (window == nullptr || GetForegroundWindow() != window || !GetClientRect(window, &client) || !GetCursorPos(&cursor))
			{
				centred = 0;
				return false;
			}

			POINT centre{ client.right / 2, client.bottom / 2 };
			ClientToScreen(window, &centre);

			const bool at_centre = std::abs(cursor.x - centre.x) <= 3 && std::abs(cursor.y - centre.y) <= 24;
			centred = at_centre ? centred + 1 : 0;
			return centred >= 3;
		}

		// the characters jump launch speed
		float jump_speed() const
		{
			const bool by_power = memory->read<std::uint8_t>(humanoid + Offsets::Humanoid::UseJumpPower) != 0;
			const float speed = by_power ? memory->read<float>(humanoid + Offsets::Humanoid::JumpPower) :
				std::sqrt(2.0f * 196.2f * (std::max)(memory->read<float>(humanoid + Offsets::Humanoid::JumpHeight), 0.0f));
			return std::isfinite(speed) ? speed : 0.0f;
		}

		// a rotation matrix facing a look direction
		static math::matrix3 facing(const math::vector3& look)
		{
			math::matrix3 m{};
			m.m[0][0] = -look.z; m.m[0][1] = 0.0f; m.m[0][2] = -look.x;
			m.m[1][0] = 0.0f;    m.m[1][1] = 1.0f; m.m[1][2] = 0.0f;
			m.m[2][0] = look.x;  m.m[2][1] = 0.0f; m.m[2][2] = -look.z;
			return m;
		}

		// flatten a vector to the ground and normalize it
		static math::vector3 flat(math::vector3 v)
		{
			v.y = 0.0f;
			const float length = v.length();
			return std::isfinite(length) && length > 0.001f ? v * (1.0f / length) : math::vector3{};
		}

		struct intent_t
		{

			bool active = false;

			float forward = 0.0f;
			bool space = false;

			math::vector3 direction{};
			bool moving = false;

			bool locked = false;
			math::vector3 look{};
			bool on_ground = true;
			float speed = 0.0f;
			float jump = 0.0f;
		};

		intent_t intent;

		// read WASD/space input and set the glide intent
		void prepare()
		{
			intent = {};

			if (humanoid == 0)
				return;

			const std::uint64_t since_macros = GetTickCount64() - macros::last_run;
			if (since_macros < 400 || (handed && since_macros < 3000 && memory->read<int>(humanoid + Offsets::Humanoid::FloorMaterial) == material_air))
			{
				if (!handed)
				{
					give_back();
					handed = true;
					macros_turns++;
				}
				return;
			}
			if (handed)
			{
				handed = false;
				take_hold();
			}

			const float walk = memory->read<float>(humanoid + Offsets::Humanoid::Walkspeed);
			if (std::isfinite(walk) && walk > 0.01f)
			{
				walk_speed = walk;
			}
			if (walk != 0.0f)
			{
				memory->write<float>(humanoid + Offsets::Humanoid::Walkspeed, 0.0f);
				memory->write<float>(humanoid + Offsets::Humanoid::WalkspeedCheck, 0.0f);
			}
			walk_held = true;

			if (states_saved)
			{
				std::uint8_t row[state_count]{};
				memory->read_buffer(state_row(), row, sizeof(row));
				if (row_looks_right(row))
				{
					for (const int state : still_states)
					{
						if (row[state] != 0)
						{
							memory->write<std::uint8_t>(state_row() + state, 0);
						}
					}
				}
			}

			const bool sits = memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::SeatPart) != 0;
			const bool anchored = (memory->read<std::uint8_t>(root + Offsets::Primitive::Flags) & static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored)) != 0;

			const int presses = crouch_presses.load();
			if (presses != crouch_seen)
			{
				if ((presses - crouch_seen) % 2 != 0)
				{
					crouched = !crouched;
					crouches++;
				}
				crouch_seen = presses;
			}
			if (!anchored && offset_known)
			{

				math::vector3 want = saved_offset;
				if (crouched != really_crouched)
				{
					want.y = crouched ? saved_offset.y - crouch_drop : (std::max)(saved_offset.y, 0.0f);
				}
				const math::vector3 has = memory->read<math::vector3>(humanoid + Offsets::Humanoid::CameraOffset);
				if (std::memcmp(&want, &has, sizeof(want)) != 0)
				{
					memory->write<math::vector3>(humanoid + Offsets::Humanoid::CameraOffset, want);
				}
			}

			if (sits || anchored || GetTickCount64() < stabilizer::own_move_until)
				return;

			// an animation
			const std::uint64_t state = memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::HumanoidState);
			const int state_id = state != 0 ? memory->read<std::uint16_t>(state + Offsets::Humanoid::HumanoidStateID) : running_state;
			if (state_id != running_state)
			{
				other_states++;
				last_other = state_id;
			}

			float side = 0.0f;
			if (GetForegroundWindow() == game::get_roblox_window() && !game::typing_in_chat())
			{
				if (GetAsyncKeyState('W') & 0x8000) intent.forward += 1.0f;
				if (GetAsyncKeyState('S') & 0x8000) intent.forward -= 1.0f;
				if (GetAsyncKeyState('D') & 0x8000) side += 1.0f;
				if (GetAsyncKeyState('A') & 0x8000) side -= 1.0f;
				intent.space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
			}

			math::matrix3 camera{};
			const bool has_camera = game::camera != 0;
			if (has_camera)
			{
				camera = memory->read<math::matrix3>(game::camera + Offsets::Camera::Rotation);
			}

			intent.direction = has_camera && (intent.forward != 0.0f || side != 0.0f) ? flat(camera * math::vector3{ side, 0.0f, -intent.forward }) : math::vector3{};
			intent.moving = intent.direction.length() > 0.5f;
			intent.locked = has_camera && mouse_locked();
			intent.look = has_camera ? flat(math::vector3{ -camera.m[0][2], 0.0f, -camera.m[2][2] }) : math::vector3{};
			intent.on_ground = memory->read<int>(humanoid + Offsets::Humanoid::FloorMaterial) != material_air;

			// standing character does
			const float standing = really_crouched ? 16.0f : walk_speed;
			intent.speed = (std::max)(crouched ? (really_crouched ? walk_speed : walk_speed * 0.5f) : standing, stabilizer::own_ground_speed.load());
			intent.jump = intent.space ? jump_speed() : 0.0f;
			intent.active = true;
		}

		// move the frozen character locally so it glides (walk, jump, climb)
		void apply(state_t& out, steady::time_point now)
		{

			const float dt = last_glide == never ? 0.0f : std::clamp(std::chrono::duration<float>(now - last_glide).count(), 0.0f, 0.02f);
			last_glide = now;

			if (!intent.active)
			{

				airborne = false;
				netblock::lifting = false;
				climbing = false;
				keeping = false;
				has_target = false;
				hip_target = saved_hip;
				return;
			}

			const math::vector3 real_at = out.position;

			if (wall_hit)
			{
				wall_hit = false;
				target.x = real_at.x;
				target.z = real_at.z;
			}

			const float forward = intent.forward;
			const bool space = intent.space;
			const math::vector3& direction = intent.direction;
			bool moving = intent.moving;
			const bool on_ground = intent.on_ground;

			math::vector3 toward{};
			float above_top = 0.0f;
			const bool at_ladder = ladder_at(out.position, toward, above_top);
			if (!at_ladder || (forward < 0.0f && on_ground) || space)
			{
				climbing = false;
			}
			else if (!climbing && moving && direction.x * toward.x + direction.z * toward.z > 0.3f)
			{
				climbing = true;
			}

			if (climbing)
			{
				const float climb = (std::min)(walk_speed * 0.7f, 25.0f);
				const float up = forward > 0.0f ? climb : forward < 0.0f ? -climb : 0.0f;
				if (!climb_known || std::abs(climb_y - out.position.y) > 2.0f)
				{
					climb_y = out.position.y;
					climb_known = true;
				}
				climb_y += up * dt;
				out.position.y = climb_y;
				out.linear.y = up;
				climbed++;

				if (above_top < 1.5f)
				{
					moving = false;
				}
			}
			else
			{

				if (climb_known)
				{
					out.linear.y = 0.0f;
				}
				climb_known = false;
			}

			const bool in_air = airborne || (landed != never && now - landed < 150ms);
			const float speed = intent.speed;
			if (!has_target || (!moving && !in_air) || std::hypot(target.x - out.position.x, target.z - out.position.z) > 1.0f)
			{
				target = out.position;
				has_target = true;
			}
			if (moving)
			{
				target.x += direction.x * speed * dt;
				target.z += direction.z * speed * dt;
				glided++;
			}
			out.position.x = target.x;
			out.position.z = target.z;

			out.linear.x = 0.0f;
			out.linear.z = 0.0f;

			// it left.

			if (climbing)
			{
				airborne = false;
			}
			else if (space && on_ground && !airborne && hip_known && std::abs(out.linear.y) < 10.0f && now - last_jump > 350ms)
			{
				const float jump = intent.jump;
				if (jump > 1.0f)
				{
					airborne = true;
					air_y = out.position.y;
					air_vy = jump;
					takeoff_y = out.position.y;
					last_jump = now;
					jumps++;
				}
			}

			if (airborne)
			{
				air_vy -= 196.2f * dt;
				air_y += air_vy * dt;

				hip_target = saved_hip + (std::max)(air_y - takeoff_y, 0.0f);

				highest_jump = (std::max)(highest_jump, out.position.y - takeoff_y);
				furthest_off = (std::max)(furthest_off, std::abs(out.position.y - air_y));

				if (air_vy < 0.0f && air_y <= takeoff_y)
				{
					airborne = false;
					landed = now;
				}
			}

			if (!airborne)
			{
				hip_target = saved_hip;
			}

			netblock::lifting = airborne || (landed != never && now - landed < 250ms);

			const math::vector3 face = climbing ? toward : intent.locked ? intent.look : (moving ? direction : math::vector3{});
			if (face.length() > 0.5f)
			{
				out.rotation = facing(face);
				out.angular = {};
			}

			const bool keep = moving || airborne || (landed != never && now - landed < 150ms) || !on_ground || climbing;
			if (keep && !keeping)
			{
				physics_at = real_at;
			}
			keeping = keep;
			keep_whole = climbing;
			kept = out;
			written[written_next] = out.position;
			written_next = (written_next + 1) % std::size(written);
		}

		std::uint64_t heartbeat = 0;
		std::uint64_t sender = 0;
		std::uint64_t root = 0;
		state_t held{};

		std::uint64_t humanoid = 0;
		float walk_speed = 16.0f;
		bool walk_held = false;
		std::uint8_t saved_states[std::size(still_states)]{};
		bool states_saved = false;
		steady::time_point last_glide = never;
		steady::time_point last_jump = never;
		int centred = 0;
		int glided = 0;
		int jumps = 0;

		bool has_target = false;
		math::vector3 target{};

		static constexpr float crouch_drop = 1.5f;
		int crouch_seen = 0;
		bool crouched = false;
		bool really_crouched = false;
		bool offset_known = false;
		math::vector3 saved_offset{};
		int crouches = 0;

		bool airborne = false;
		float air_y = 0.0f;
		float air_vy = 0.0f;
		float takeoff_y = 0.0f;
		bool hip_known = false;
		float saved_hip = 0.0f;

		float hip_target = 0.0f;
		float hip_written = 0.0f;
		float highest_jump = 0.0f;

		float furthest_off = 0.0f;
		steady::time_point landed = never;

		// re-freeze if the hold is still active after restoring
		void after_restore()
		{
			if (hip_known && humanoid != 0 && hip_target != hip_written)
			{
				memory->write<float>(humanoid + Offsets::Humanoid::HipHeight, hip_target);
				hip_written = hip_target;
			}
		}

		bool keeping = false;
		bool keep_whole = false;
		state_t kept{};

		math::vector3 written[8]{};
		std::size_t written_next = 0;

		math::vector3 physics_at{};
		bool wall_hit = false;
		int put_backs = 0;
		int walls = 0;

		static constexpr int running_state = 8;
		int other_states = 0;
		int last_other = -1;

		bool handed = false;
		int macros_turns = 0;

		// keep walkspeed and states zeroed while the job is idle
		void keep_on()
		{
			if (keep_whole)
			{
				state_t now{};
				memory->read_buffer(root + Offsets::Primitive::Rotation, &now, sizeof(now));
				if (std::memcmp(&now, &kept, sizeof(kept)) != 0)
				{
					memory->write_buffer(root + Offsets::Primitive::Rotation, &kept, sizeof(kept));
				}
				return;
			}

			const math::vector3 at = memory->read<math::vector3>(root + Offsets::Primitive::Position);
			if (at.x == kept.position.x && at.z == kept.position.z)
				return;

			bool ours = std::hypot(at.x - physics_at.x, at.z - physics_at.z) < 0.01f;
			for (const math::vector3& place : written)
			{
				ours = ours || std::hypot(at.x - place.x, at.z - place.z) < 0.01f;
			}
			if (ours)
			{
				put_backs++;
			}
			else
			{
				walls++;
				wall_hit = true;
				kept.position.x = at.x;
				kept.position.z = at.z;
			}
			physics_at = at;
			memory->write<float>(root + Offsets::Primitive::Position, kept.position.x);
			memory->write<float>(root + Offsets::Primitive::Position + 2 * sizeof(float), kept.position.z);
		}

		bool climbing = false;
		bool climb_known = false;
		float climb_y = 0.0f;
		int climbed = 0;

		steady::time_point last_round = never;
		float usual = 0.0f;
	};

	keybind::keybind_t lag_key;

	bool key_was_down = false;

	enum class front_t
	{
		game,

		console,

		menu,
		elsewhere
	};

	// which window is in front
	front_t window_in_front()
	{
		const HWND front = GetForegroundWindow();
		if (front == nullptr)
			return front_t::elsewhere;

		if (front == game::get_roblox_window())
			return front_t::game;

		if (front == GetConsoleWindow())
			return front_t::console;

		return front == render->detail->window ? front_t::menu : front_t::elsewhere;
	}

	// skip the lag switch, with the reason for the menu
	void leave_alone(netblock::ignored_t why)
	{
		netblock::ignored_why = why;
		netblock::ignored_at = GetTickCount64();

		if (why == netblock::ignored_t::menu_keyboard)
		{
			LOG_INFO("lag switch: its key was pressed while the menu had the keyboard (a text box, or a key being picked), so it was left alone");
		}
		else if (why == netblock::ignored_t::no_game)
		{
			LOG_INFO("lag switch: switched on, but there is no game to hold anything in right now");
		}
	}

	// true if the user is holding the lag switch key
	bool asks()
	{
		const int key = settings::mover::netblock::keybind;

		if (lag_key.key != key)
		{
			lag_key.key = key;

			lag_key.was_pressed = true;
		}

		const front_t front = window_in_front();
		const bool here = front == front_t::game || front == front_t::console;

		if (key != 0)
		{

			const bool pressed = keybind::key_down(key);
			const bool went_down = pressed && !key_was_down;
			key_was_down = pressed;

			const bool is_mouse_key = key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2;
			const bool menu_has_it = is_mouse_key && render->menu_has_mouse;

			if (!here || menu_has_it)
			{

				lag_key.was_pressed = pressed;

				if (went_down && front == front_t::menu && !menu_has_it)
				{
					leave_alone(netblock::ignored_t::menu_keyboard);
				}
			}
			else
			{
				if (pressed && !lag_key.was_pressed)
				{
					settings::mover::netblock::enabled = !settings::mover::netblock::enabled;
					LOG_INFO("lag switch: its key switched it {}", settings::mover::netblock::enabled ? "on" : "off");
				}
				lag_key.was_pressed = pressed;
			}
		}

		return settings::mover::netblock::enabled;
	}
}

namespace
{

	// true if a key is the crouch key
	bool is_crouch_key(DWORD vk)
	{
		if (vk != 'C')
			return false;

		return static_cast<int>(vk) != settings::mover::netblock::keybind && static_cast<int>(vk) != settings::clicker::keybind;
	}

	bool taken_down[256]{};
	std::atomic<int> keys_taken{ 0 };

	bool release_owed = false;

	// release the C key
	void release_c()
	{
		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = 'C';
		input.ki.wScan = static_cast<WORD>(MapVirtualKeyW('C', MAPVK_VK_TO_VSC));
		input.ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(1, &input, sizeof(INPUT));
	}

	// keyboard hook that hides crouch presses from roblox
	LRESULT CALLBACK keep_crouch_out(int code, WPARAM message, LPARAM data)
	{
		if (code == HC_ACTION)
		{
			const KBDLLHOOKSTRUCT* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
			const DWORD vk = key->vkCode & 0xFF;
			const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;

			if ((key->flags & LLKHF_INJECTED) == 0 && is_crouch_key(vk))
			{

				const bool here = netblock::holding && GetForegroundWindow() == game::get_roblox_window() && !game::typing_in_chat();

				if (down && here)
				{

					if (!taken_down[vk])
					{
						keys_taken++;
						crouch_presses++;
					}
					taken_down[vk] = true;
					return 1;
				}

				if (!down && (taken_down[vk] || here))
				{

					if (!taken_down[vk])
					{
						release_owed = true;
					}
					taken_down[vk] = false;
					return 1;
				}
			}
		}

		return CallNextHookEx(nullptr, code, message, data);
	}

	// install the crouch-key hook
	void watch_crouch_keys()
	{
		HHOOK hook = nullptr;

		while (g_running)
		{
			if (netblock::holding && hook == nullptr)
			{
				hook = SetWindowsHookExW(WH_KEYBOARD_LL, keep_crouch_out, GetModuleHandleW(nullptr), 0);
				keys_taken = 0;
				if (hook == nullptr)
				{
					LOG_WARN("lag switch: the crouch keys could not be kept from the game (error {})", GetLastError());
				}
			}
			else if (!netblock::holding && hook != nullptr)
			{
				UnhookWindowsHookEx(hook);
				hook = nullptr;

				if (release_owed && (GetAsyncKeyState('C') & 0x8000) == 0 && GetForegroundWindow() == game::get_roblox_window())
				{
					release_c();
					LOG_INFO("lag switch: C, held down when it went on and let go meanwhile, let go in the game now");
				}
				release_owed = false;

				if (keys_taken > 0)
				{
					LOG_INFO("lag switch: {} press{} of C (crouch) kept from the game while it held", keys_taken.load(), keys_taken == 1 ? "" : "es");
				}
			}

			MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);

			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
		}

		if (hook != nullptr)
		{
			UnhookWindowsHookEx(hook);
		}
	}
}

// background loop: drive the lag switch
void netblock::run()
{
	precise_sleeper sleeper;
	packet_hold_t packets;
	ghost_t ghost;
	lag_clock_t clock;

	std::thread crouch_keys([] { logger::name_thread("lagswitch keys"); watch_crouch_keys(); });

	bool ghosting = false;

	std::uint32_t held_process = 0;
	steady::time_point last_port_check = never;
	bool asked_before = false;

	auto end_hold = [&](steady::time_point now, const char* reason, let_go why)
	{
		const double seconds = std::chrono::duration<double>(now - clock.since).count();

		if (ghosting)
		{
			ghost.finish_glide();
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
			LOG_INFO("lag switch off after {:.2f} s: {}. {} rounds of the game were sent the held place; the sender was not seen to run in {}, the held place stayed on the part too long in {} (longest {:.1f} ms), {} long Heartbeats had it taken off meanwhile, and about {} rounds went by uncaught", seconds, reason, ghost.rounds, ghost.missed, ghost.late, ghost.longest, ghost.lifts, ghost.skipped);
		}
		else
		{
			packets.release();
			LOG_INFO("lag switch off after {:.2f} s: {}", seconds, reason);
		}

		ghosting = false;
		clock.ended(now, why);
		held_process = 0;
		holding = false;

		if (why == let_go::could_not_hold)
		{
			failed_at = GetTickCount64();
		}

		if (why != let_go::key)
		{
			settings::mover::netblock::enabled = false;
		}
	};

	while (g_running)
	{
		const std::uint32_t process = memory->get_process_id();
		const bool game_is_there = process != 0 && !game::game_changed;
		const bool wanted = asks();

		if (wanted && !game_is_there)
		{
			if (!asked_before)
			{
				leave_alone(ignored_t::no_game);
			}
			settings::mover::netblock::enabled = false;
		}
		asked_before = wanted;

		const bool asked = wanted && game_is_there;
		const steady::time_point now = steady::now();

		if (clock.holding)
		{
			let_go why{};

			if (process != held_process || memory->process_ended())
			{
				end_hold(now, "Roblox is gone", let_go::could_not_hold);
			}

			else if (clock.should_end(asked, now, 1.0e9f, why))
			{
				end_hold(now, why == let_go::time_limit ? "the time limit was reached" : "switched off", why);
			}
			else if (ghosting)
			{

				if (!ghost.round())
				{
					end_hold(steady::now(), "the character is no longer there", let_go::could_not_hold);
				}
				continue;
			}
			else if (now - last_port_check >= 250ms)
			{
				last_port_check = now;
				if (packets.drop_stale_ports() == 0)
				{
					end_hold(now, "the game closed the sockets that were held", let_go::could_not_hold);
				}
			}
		}
		else if (clock.should_begin(asked, now))
		{
			if (ghost.begin())
			{
				ghosting = true;

				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

				LOG_INFO("lag switch on: the others are sent ({:.0f}, {:.0f}, {:.0f}), where the character is now, until it is switched off. Everything else goes on as usual",
					ghost.place().x, ghost.place().y, ghost.place().z);
			}
			else if (packets.hold(process))
			{
				ghosting = false;
				last_port_check = now;

				LOG_WARN("lag switch on the old way, the place that is sent could not be held: what process {} sends from UDP port{} {} is held back until it is switched off. The others will stand still on this screen too, and a long hold gets the game disconnected",
					process, packets.ports_text().find(',') == std::string::npos ? "" : "s", packets.ports_text());
			}
			else
			{

				clock.spent = true;
				failed_at = GetTickCount64();
				settings::mover::netblock::enabled = false;
				sleeper.sleep_ms(5.0);
				continue;
			}

			clock.began(now);
			held_process = process;
			holding_since = GetTickCount64();
			holding = true;
			continue;
		}

		if (!clock.holding && game_is_there)
		{
			find_trusses(now);
		}
		sleeper.sleep_ms(clock.holding ? 1.0 : 5.0);
	}

	if (ghosting)
	{
		ghost.finish_glide();
	}
	packets.close();
	holding = false;
	crouch_keys.join();
}
