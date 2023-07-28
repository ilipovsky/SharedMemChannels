#pragma once
#include <SharedMemoryChannel.h>
#include <iostream>

/// noisy - traces what's going on (CONTAINS NO DATA MEMBERS)
struct noisy {
	noisy& operator=(noisy&&) noexcept { std::cout << "operator=(noisy&&)\n"; return *this; }
	noisy& operator=(const noisy&) { std::cout << "operator=(const noisy&)\n"; return *this; }
	noisy(const noisy&) { std::cout << "noisy(const noisy&)\n"; }
	noisy(noisy&&) noexcept { std::cout << "noisy(noisy&&)\n"; }
	~noisy() { std::cout << "~noisy()\n"; }
	noisy() { std::cout << "noisy()\n"; }
};

struct NoisyString : public SharedMemIPC::String, noisy
{
	using SharedMemIPC::String::String; // inherit CTORs
};
