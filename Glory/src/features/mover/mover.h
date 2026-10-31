#pragma once

namespace mover
{
	void run();

	enum class teleport_status
	{

		off,

		no_target,

		waiting,

		looping
	};

	teleport_status get_teleport_status();
}
