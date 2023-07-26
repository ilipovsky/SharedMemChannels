#include <thread>
#include <chrono>
#include <SharedMemoryChannel.h>

int main() 
{
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

	// if you don't want the noise, just use:
	//using NoisyString = SharedMemIPC::String;

	/*{
		SharedMemIPC::NonBlockingProducer<NoisyString>::MemoryPoolT testmempool("TestMemPool1");
		std::cout << "mem pool about to go out of scope\n";
	}*/

#ifdef NOISY
	using MyString = NoisyString;
#else
	using MyString = SharedMemIPC::String;
#endif

	SharedMemIPC::NonBlockingProducer<MyString> shbuffIn("MySharedMemory2");

	std::cout << "Pushing\n";

	//SharedMemIPC::Ptr p = create();
	//p->assign("hi");

	int dummy = 0;
	//for (std::string msg : { "hello", "world", "bye", "cruel", "world" })
	for (auto msg : { "hello", "world", "bye", "cruel", "world" })
	{
		//auto p = shbuffIn.CreatePtr(msg.c_str(), msg.size()); // we're getting a new "tenure" in the managed shared memory, by grabbing a fresh piece of memory and setting refcount to 1
		auto p = shbuffIn.CreatePtr(msg);
		if (shbuffIn.WriteAvailable()) shbuffIn.Push(p); // briefly sets refcount to 2, but it then gets set back to 1, after the end of current scope as we loop 
		else std::cout << "Dropping: " << msg << std::endl;
		//std::cout << msg << std::endl;
	}
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(20s);
	return EXIT_SUCCESS;
}