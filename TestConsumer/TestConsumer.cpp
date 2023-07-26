#include <SharedMemoryChannel.h>
/// noisy - traces what's going on (CONTAINS NO DATA MEMBERS)
struct noisy {
	noisy& operator=(noisy&&) noexcept { std::cout << "operator=(noisy&&)\n"; return *this; }
	noisy& operator=(const noisy&) { std::cout << "operator=(const noisy&)\n"; return *this; }
	noisy(const noisy&) { std::cout << "noisy(const noisy&)\n"; }
	noisy(noisy&&) noexcept { std::cout << "noisy(noisy&&)\n"; }
	~noisy() { std::cout << "~noisy()\n"; }
	noisy() { std::cout << "noisy()\n"; }
};

template <class T>
struct NoisyConsumer : public SharedMemIPC::Consumer<T>, noisy
{
	using SharedMemIPC::Consumer<T>::Consumer; // inherit CTORs
};

void Consumer()
{
	struct DataToUI
	{
		uint32_t points[512 * 512] = { 0 }; //total histogram
		uint32_t zvalue = 0; // zvalue for color map
	};

#if 0
	{
		auto pConsumerHistogramData = std::make_shared<SharedMemIPC::Consumer<DataToUI>>((std::string("HistogramData") + "InstanceID0").c_str(), (600 + 1 + 1 + 1) * sizeof(DataToUI) + 16 * 4096, 1);
		static int hello = 0;
	}
#endif

	struct NoisyString : public SharedMemIPC::String, noisy
	{
		using SharedMemIPC::String::String; // inherit CTORs
	};

	// if you don't want the noise, just use:
	//using NoisyString = SharedMemIPC::String;



	std::cout << "Popping\n";
	//SharedMemIPC::Consumer<NoisyString> shbuffOut("MySharedMemory2");
	//NoisyConsumer<NoisyString> shbuffOut("MySharedMemory2");

#ifdef NOISY
	using MyString = NoisyString;
#else
	using MyString = SharedMemIPC::String;
#endif
	SharedMemIPC::NonBlockingConsumer<MyString> shbuffOut("MySharedMemory2");
	{
		SharedMemIPC::Ptr<MyString> into;
		std::cout << "Popping??\n";
		while (shbuffOut.ReadAvailable())
		{
			shbuffOut.Pop(into); // we destroy previous "tenure" each time we do a new Pop() (shared_ptr refcount becomes 0 for the old value of into)
			std::cout << "Popped: '" << *into << "'" << "use count is " << into.use_count() << "\n";
			//into.reset();
		} // each time we get here, to the end of current scope, 
		std::cout << "Going out of scope\n";
	} // RAII
	std::cout << "Out of scope\n";
}

int main() {


	Consumer();

	return EXIT_SUCCESS;
}