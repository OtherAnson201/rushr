#pragma once
#include <windows.h>

class precise_sleeper final
{
public:
	precise_sleeper()
	{

		timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
	}

	~precise_sleeper()
	{
		if (timer != nullptr)
		{
			CloseHandle(timer);
		}
	}

	precise_sleeper(const precise_sleeper&) = delete;
	precise_sleeper& operator=(const precise_sleeper&) = delete;

	void sleep_ms(double milliseconds)
	{
		if (timer != nullptr)
		{

			LARGE_INTEGER due{};
			due.QuadPart = -static_cast<LONGLONG>(milliseconds * 10000.0);

			if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			{
				WaitForSingleObject(timer, 1000);
				return;
			}
		}

		Sleep(milliseconds < 1.0 ? 1 : static_cast<DWORD>(milliseconds));
	}

private:
	HANDLE timer = nullptr;
};
