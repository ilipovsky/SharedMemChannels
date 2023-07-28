#include "SharedMemoryChannel.h"
#include "TestUtil.h"

#include <iostream>




void Consumer()
{

#ifdef NOISY
	using MyString = NoisyString;
#else
	using MyString = SharedMemIPC::String;
#endif
	SharedMemIPC::NonBlockingConsumer<MyString> shbuffOut("MySharedMemory2");
	{
		SharedMemIPC::Ptr<MyString> into;
		while (shbuffOut.ReadAvailable())
		{
			shbuffOut.Pop(into); // we destroy previous "tenure" each time we do a new Pop() (shared_ptr refcount becomes 0 for the old value of into)
			std::cout << "Popped: '" << *into << "'" << "use count is " << into.use_count() << "\n";
		} // each time we get here, to the end of current scope, 
	} // RAII
	std::cout << "Out of scope\n";
}

int main()
{
	Consumer();
	return EXIT_SUCCESS;
}