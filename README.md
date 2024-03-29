# SharedMemChannels
A C++ IPC inspired by MS Threading Channels

Right now works in Windows, but will be extended for Linux in the future. (Alas, anynomous shared memory works not as elegantly in Linux, but I have some ideas on how to get around it.)

Before building the solution, make sure VCPKG is installed in your system. 



Let’s look at how shared memory is allocated. The overview:

Memory provisioning (i.e. allocation of the big shared mem segment). This is the raw mem block.
Construction of the individual elements inside the aforementioned segment.
 
So, 1): on line 359 of SharedMemChannel.h you can see how we tell boost library to allocated the shared memory segment. This is part of MemoryPool construction. MemoryPool is the main thing of the entire file. The “meat.” Here we tell the library (which talks to the Win32 system) that we need a segment of overall size m_PoolParamsLocal.m_uiOverallShMemSize. If I recall correctly, when it comes to HardwareInfoGetRet, the only reason that our default value of “0” specified on line 350 (NOT 359) works, is because when PoolParamsT calculates the memory size, it has a nice padding of 2*4096 bytes. We’re in luck. It works for us here, because, I remember, there happens to be only one channel for this memory pool and that channel has only 2 or 3 elements capacity (i.e. the queue has around 3 elements, plus you need to account for the ingress and egress elements (hence, 3+2, at most, altogether)). And yes, true, each such element has a bunch of vectors in it, like featureinfo, but these are relatively small-sized, so fit quite comfortably in the whopping 8192+a bunch of naïve calculated things that we supplied. If we were not so lucky, we’d have to specify the overall memory size for the segment manually, in the constructor of the memory pool for HardwareInfoGetRet (look for either Producer<HardwareInfoGetRet>::MemoryPoolT or MemoryPool<HardwareInfoGetRet> or something like that, to see where the constructor for memory pool is invoked).
 
2) OK, so now we’re done with provisioning/overall allocation of the raw shared memory. But now we need to construct the actual elements inside. Construction will happen using placement new operator. It happens inside the call to CreatePtr() on line 546 and 558. It is here (namely, on line 546) that the custom allocator is used. The idea is that complicated structures may want to do *runtime* (re-)allocation (i.e. when additional reallocations may happen after the initial one). And vectors, obviously, are runtime re-allocatable entities. Strings are too. The shared memory allocator of MemoryPool gets passed on line 546. The next set of magic happens on line 35 and 36 of HardwareInfoGetCmd.h. Here we tell the compiler that the types String and Vector in HardwareInfoGetRet  are using our shared mem allocator type. For String, we say that it needs to allocate elements of type char inside the shmem segment. For Vector<T>, we say that it needs to allocate elements of type T inside the shmem segment.

 
For simple structs, like HardwareInfoGetCmd, the construction happens on line 558. As you can see, no allocator is being passed. It’s enough to just call the constructor of “Element” directly, forwarding the arguments passed to CreatePtr() routine to it.
