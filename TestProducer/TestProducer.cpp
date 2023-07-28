#include "SharedMemoryChannel.h"
#include "TestUtil.h"
#include <thread>
#include <chrono>
#include <iostream>







int main() 
{


#ifdef NOISY
	using MyString = NoisyString;
#else
	using MyString = SharedMemIPC::String;
#endif

	SharedMemIPC::NonBlockingProducer<MyString> shbuffIn("MySharedMemory2");

	std::cout << "Pushing into shmem queue\n";

	for (auto msg : { "hello", "world", "bye", "cruel", "world" })
	{
		auto p = shbuffIn.CreatePtr(msg);
		if (shbuffIn.WriteAvailable()) shbuffIn.Push(p); // briefly sets refcount to 2, but it then gets set back to 1, after the end of current scope as we loop 
		else std::cout << "Dropping: " << msg << std::endl;
		//std::cout << msg << std::endl;
	}
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(20s);
	return EXIT_SUCCESS;
}