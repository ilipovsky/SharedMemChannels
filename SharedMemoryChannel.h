#pragma once

#include <type_traits> // decay_t
#include <vector>
#include <mutex>
#include <string>
//#include <scoped_allocator>
#include <boost/container/scoped_allocator.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/containers/vector.hpp>
//#include <boost/any.hpp>

//#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/managed_windows_shared_memory.hpp>
#include <boost/interprocess/smart_ptr/shared_ptr.hpp>

#include <boost/interprocess/sync/scoped_lock.hpp> // scoped lock for using the named mutex below
#include <boost/interprocess/sync/named_mutex.hpp> // named_mutex used in init of MemoryPool (prevents data races during initialization)

#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/sync/windows/mutex.hpp>
#include <boost/lockfree/spsc_queue.hpp>


#include <iostream>
#undef max 
#undef min

namespace bip = boost::interprocess;
namespace blf = boost::lockfree;


namespace SharedMemIPC
{
    // kind of like a tag type
    struct FindOrConstructT
    {
        constexpr bool operator==(const FindOrConstructT& rh) const
        {
            return (this == &rh);
        }


        // MUST NEVER be able to copy/move objects of this type.
        // I purposely restrict the usage of this class to predefined objects below (FindOrConstruct, ConstructAnonymous etc)
        constexpr FindOrConstructT() {}
        constexpr FindOrConstructT(const FindOrConstructT&) = delete;
        constexpr FindOrConstructT(FindOrConstructT&&) = delete;
    }; 

    // all objects with UNIQUE addresses (don't use anything else)
    _INLINE_VAR constexpr static FindOrConstructT FindOrConstruct{};
    _INLINE_VAR constexpr static FindOrConstructT FindOnly{};
    _INLINE_VAR constexpr static FindOrConstructT ConstructAnonymous{};

    // these are the references that are used in the Producer/Consumer API:
    //_INLINE_VAR constexpr static const FindOrConstructT & FindOrConstruct = FindOrConstructDummyObject;
    //_INLINE_VAR constexpr static const FindOrConstructT & FindOnly = FindOnlyDummyObject;
    //_INLINE_VAR constexpr static const FindOrConstructT & ConstructAnonymous = ConstructAnonymousDummyObject;

    //using Segment = bip::managed_windows_shared_memory;//managed_mapped_file;

    //the following is almost identical to the above, but removes incompatibility between 32 and 64-bit processes.
    //See more at: https://stackoverflow.com/a/63224381
    using Segment = bip::basic_managed_windows_shared_memory
        <char,
         bip::rbtree_best_fit<
            bip::mutex_family,
            bip::offset_ptr<
                void,
                int64_t,
                uint64_t,
                0>,
            0>,
        bip::iset_index>;

    using LocalSegment = bip::basic_managed_heap_memory
        <char,
         bip::rbtree_best_fit<
            bip::mutex_family,
            bip::offset_ptr<
                void,
                int64_t,
                uint64_t,
                0>,
            0>,
        bip::iset_index>;


    using Manager = Segment::segment_manager;
    template <typename T> using Alloc = bip::allocator<T, Manager>;
    //see https://stackoverflow.com/a/49738871:
    //template <typename T> using Alloc = std::scoped_allocator_adaptor<bip::allocator<T, Manager> >;
    /*template <typename T> using Alloc = boost::container::scoped_allocator_adaptor<
        bip::allocator<T, Manager>
    >;*/
    template <typename T> using ScopedAlloc = boost::container::scoped_allocator_adaptor<Alloc<T> >;


    using String = bip::basic_string<char, std::char_traits<char>, Alloc<char> >;
    template <typename T> using Vector = bip::vector<T, Alloc<T>>;
    using CharVector = Vector<char>;// bip::vector<char, Alloc<char>>;
    using FloatVector = Vector<float>;//bip::vector<float, Alloc<float>>;

    // using Element = String;
    // For debug/demonstration
    //struct Element : String, noisy { using String::String; }; // inherit constructors

    template <typename Element>
    using Ptr = typename bip::managed_shared_ptr<Element, Segment>::type;

    //using Buffer = blf::spsc_queue<Ptr, blf::allocator<Alloc<Ptr> > >;

    template <typename Element>
    using Queue = blf::spsc_queue<Ptr<Element>, blf::allocator<Alloc<Ptr<Element> > > >;



    struct NullSemaphore {
        NullSemaphore(size_t uiInitialCount) {}
        void wait() {}
        void post() {}
    };

    struct DefaultSemaphore {
        DefaultSemaphore(size_t uiInitialCount) : semaphore((assert(uiInitialCount <= std::numeric_limits<unsigned int>::max()), static_cast<uint32_t>(uiInitialCount))) {}
        void wait() { semaphore.wait(); }
        void post() { semaphore.post(); }
        bip::interprocess_semaphore semaphore;
    };

    struct NullMutex
    {
        void lock() {}
        void unlock() {}
    };


    template <typename Element, typename SemaphoreType>
    struct Channel : public Queue<Element>
    {
#if 1
        using Queue<Element>::Queue; //inherit CTORs
#else
        Channel(size_t uiSize, Alloc<Ptr<Element>> a) :
            Queue<Element>(uiSize, a)
        {
            if constexpr (std::is_default_constructible_v<Ptr<Element>>)
            {
                auto uiSlots = Queue<Element>::write_available();
                for (decltype(uiSlots) i = 0; i < uiSlots; i++)
                {
                    Queue<Element>::push(Ptr<Element>{});
                }

                for (decltype(uiSlots) i = 0; i < uiSlots; i++)
                {
                    Ptr<Element> t;
                    Queue<Element>::pop(t);
                }
            }
        }
#endif

        //Queue<Element> queue_{};
        SemaphoreType semAvailableForConsumer_{ Queue<Element>::read_available() };
        SemaphoreType semAvailableForProducer_{ Queue<Element>::write_available() };

        std::atomic_size_t uiElementCounter{ 0 }; // keeps incrementing (and eventually wraps-around), as new shared pointers in the region are created. Since all shared ptrs are eventually retired, the eventual wrap-around is fine.

        // this is a blocking op
        bool Push(Ptr<Element> const& t)
        {
            decltype(Queue<Element>::push(t)) bRet;
            semAvailableForProducer_.wait();
            bRet = Queue<Element>::push(t);
            semAvailableForConsumer_.post();
            return bRet;
        }

        // this is a blocking op
        bool Pop(Ptr<Element>& t)
        {
            decltype(Queue<Element>::pop(t)) bRet;
            semAvailableForConsumer_.wait();
            bRet = Queue<Element>::pop(t);
            semAvailableForProducer_.post();
            return bRet;
        }

        size_t ReadAvailable() const
        {
            return  Queue<Element>::read_available();
        }

        size_t WriteAvailable() const
        {
            return  Queue<Element>::write_available();
        }
    };


    // see the use of this monstrosity (below), for explanation
    template <typename T>
    struct dummyconstexprobj
    {
        constexpr static int value = 0;
    };

#if 0
    template <typename Element, typename SemaphoreType = DefaultSemaphore>
    struct PoolParams
    {
        size_t m_uiMaxNumOfQueues = 0;
        size_t m_uiQueueSize = 0;
        size_t m_uiPoolSize = 0;
        size_t m_uiOverallShMemSize = 0; // has to be declared the last, because initialization order depends on the ones above
    };
#endif

    constexpr static size_t uiDefaultQueueSize = 50; // this is like #define, just compile-time (as opposed to preprocess-time)
    constexpr static size_t uiDefaultMaxNumOfQueues = 64; // aggressively assuming that we have up to 64 producer-consumer pairs
    constexpr static const char uiDefaultChannelName[] = "DefaultChannelName";

    // Memory Pool Object
    template <typename Element, typename SemaphoreType/* = DefaultSemaphore*/>
    struct MemoryPool//: public Channel<Element, SemaphoreType >
    {
        using Base = MemoryPool;//decltype(*this); //Channel<Element, SemaphoreType>;
        using ElementPtrType = Ptr<Element>;

        struct UnrestrictedPerms
        {
            UnrestrictedPerms() { perms.set_unrestricted(); }
            bip::permissions perms;
        };

        using NamedBuffer = Channel<Element, SemaphoreType>;
        //using PoolParamsT = PoolParams<Element, SemaphoreType>;

        // These exist in LOCAL memory ONLY (used as parameters, for set-up of the shmem segment and its initial contents):
        struct PoolParamsT
        {
            static constexpr size_t uiPadding = 2 * 4096;

            constexpr PoolParamsT() = default;

            PoolParamsT(size_t uiMaxNumOfQueues, size_t uiOverallShMemSize, size_t uiQueueSize, size_t uiPoolSize) :
                m_uiMaxNumOfQueues(uiMaxNumOfQueues == 0 ? uiDefaultMaxNumOfQueues : uiMaxNumOfQueues),
                m_uiQueueSize(uiQueueSize == 0? uiDefaultQueueSize : uiQueueSize),
                m_uiPoolSize(uiPoolSize == 0 ? m_uiQueueSize + 2 : uiPoolSize),// should usually be sized as the queue + 1 currently being processed on the producer side + 1 on consumer's
                m_uiOverallShMemSize(uiOverallShMemSize == 0 ?
                    //Note TBD: sizeof(Element) below might be replaced with a type specific "worst_case_sizeof" (for eg Strings, you'd have sizeof(String)+max_num_of_char_in_str*sizeof(char))
                    uiPadding + (m_uiPoolSize * (sizeof(Element) + sizeof(ElementPtrType)/*size of ptr + its ctrl block in ptr pool*/)) + (m_uiMaxNumOfQueues * (uiPadding + ((m_uiQueueSize + 1) * sizeof(ElementPtrType)/*size of ptr*/))) :
                    uiOverallShMemSize)
            {}

            size_t m_uiMaxNumOfQueues = 0;
            size_t m_uiQueueSize = 0;
            size_t m_uiPoolSize = 0;
            size_t m_uiOverallShMemSize = 0; // has to be declared the last, because initialization order depends on the ones above
        } m_PoolParamsLocal{};

        std::string m_strMtxForInit;

        //We need to have a default values for all reference types. Otherwise, compiler will not be able to 
       //generate the default constructor for MemoryPool (MemoryPool::MemoryPool()), marking it as "deleted"
        //We use as dummies, in order to have some default values for the compiler-generated default constructor to use.
        //NOTE: The default constructor is almost never used, other than in situations when we need to have
        //a dummy MemoryPool object (see AppInterface, the PassiveModeT constructor). If the default constructor
        //is not used, these get optimized away by the compiler and linker.
        //The alternative to using inline dummies is to turn all reference types into pointers. This is exactly
        //what was done for pvecSharedPool, because I could not make a static inline dummy object of this
        //non-trivial type (boost doesn't provide easy way):
#if _HAS_CXX17
        _INLINE_VAR static PoolParamsT _PoolParamsDummy{};
        _INLINE_VAR static size_t uiDummy{};
        _INLINE_VAR static std::atomic_size_t uiatomicDummy{};
        //_INLINE_VAR static bip::vector<Ptr<Element>, Alloc<Ptr<Element>>> bipvecDummy{}; <-- can't be done
#else
        _INLINE_VAR static PoolParamsT _PoolParamsDummy;
        _INLINE_VAR static size_t uiDummy;
        _INLINE_VAR static std::atomic_size_t uiatomicDummy;
#endif

        // These refer to objects in SHMEM also
        std::shared_ptr<bip::named_mutex> m_mtxForInit{}; // used to make the constructor data race-safe
        std::shared_ptr<Segment> m_pSegment{};   // this is responsible for managing the lifecycle of the shmem region
        PoolParamsT& m_PoolParams = _PoolParamsDummy; // the user parameters that comprise the metainfo of shmem (ie its size, num of queues, num of elements per queue etc)
        NamedBuffer* channels_ = nullptr; // the array of queues (aka "shared memory channels")
        String* names_ = nullptr; // names for the above, if using named channels
        size_t& uiNumOfNamedQueues_ = uiDummy; // current total number NAMED channels -- these grow UP in the array (like PC program heap)
        size_t& uiNumOfProducers_ = uiDummy; // current total number of anonymous (ie UNNAMED) producers -- these grow DOWN in the array (like PC program stack)
        size_t& uiNumOfConsumers_ = uiDummy; // current total number of anonymous (ie UNNAMED) consumers -- these grow DOWN in the array (like PC program stack)
        bip::vector<Ptr<Element>, Alloc<Ptr<Element>>> *pvecSharedPool = nullptr;// a pool pre-allocated bip::shared_ptrs to interprocess shared memory, ready-made for fast and easy production of elements to push into the queue
        std::atomic_size_t& m_uiCurPoolIndex = uiatomicDummy;

        // find (but not create) a NAMED channel for a producer/consumer
        Channel<Element, SemaphoreType>& FindChannel(const char* acQueueName)
        {
            auto p = std::find(names_, names_ + uiNumOfNamedQueues_, acQueueName);

            if (uiNumOfNamedQueues_ > 0 && p != names_ + uiNumOfNamedQueues_)
            {
                // Found an existing channel with this name, let's return it:
                auto diff = p - names_;
                return *(channels_ + diff);
            }
            else throw "Could not find existing channel with this name: " + std::string(acQueueName);
        }



        // get a NAMED channel for a producer/consumer
        Channel<Element, SemaphoreType>& FindOrConstructChannel(const char* acQueueName)
        {
            auto p = std::find(names_, names_ + uiNumOfNamedQueues_, acQueueName);

            if (uiNumOfNamedQueues_ > 0 && p != names_ + uiNumOfNamedQueues_)
            {
                // Found an existing channel with this name, let's return it:
                auto diff = p - names_;
                return *(channels_ + diff);
            }
            else if (uiNumOfNamedQueues_ + std::max(uiNumOfProducers_, uiNumOfConsumers_) < m_PoolParams.m_uiMaxNumOfQueues)
            {
                // We didn't find it, but it's OK to add it
                names_[uiNumOfNamedQueues_] = acQueueName;
                return channels_[uiNumOfNamedQueues_++];
            }
            else throw "Could neither find as existing nor add new channel (maximum number of channels for MemoryPool has been reached already)";
        }


        struct GetAnonymousProducerT {}; // tag type
        // get an UNNAMED (aka anonymous) channel for a producer
        Channel<Element, SemaphoreType>& FindOrConstructChannel(GetAnonymousProducerT)
        {
            if (uiNumOfNamedQueues_ + uiNumOfProducers_ < m_PoolParams.m_uiMaxNumOfQueues)
            {
                // grab the first available empty channel -- from the END of the array
                return *(channels_ + m_PoolParams.m_uiMaxNumOfQueues-1 - uiNumOfProducers_++); 
            }
            else throw "Could not add new producer channel (maximum number of anonymous channels for MemoryPool has been reached already)";
        }

        struct GetAnonymousConsumerT {}; // tag type
        // get an UNNAMED (aka anonymous) channel for a consumer
        Channel<Element, SemaphoreType>& FindOrConstructChannel(GetAnonymousConsumerT)
        {
            if (uiNumOfNamedQueues_ + uiNumOfConsumers_ < m_PoolParams.m_uiMaxNumOfQueues)
            {
                // grab the first available empty channel -- from the END of the array
                return *(channels_ + m_PoolParams.m_uiMaxNumOfQueues-1 - uiNumOfConsumers_++);
            }
            else throw "Could not add new consumer channel (maximum number of anonymous channels for MemoryPool has been reached already)";
        }

        MemoryPool() = default;
        MemoryPool(const MemoryPool&) = default;
        MemoryPool(MemoryPool&&) = default;

        MemoryPool(const char* m_acSharedMemName, size_t uiMaxNumOfQueues = 0, size_t uiOverallShMemSize = 0, size_t uiQueueSize = 0, size_t uiPoolSize = 0) :
        //MemoryPool(const char *m_acSharedMemName, size_t uiMaxNumOfQueues, size_t uiOverallShMemSize, size_t uiQueueSize, size_t uiPoolSize) :
            /* INIT local-only items:*/
            m_PoolParamsLocal(uiMaxNumOfQueues, uiOverallShMemSize, uiQueueSize, uiPoolSize),
            m_strMtxForInit((std::string("InitMtx") + std::string(m_acSharedMemName)).substr(0, 200)),
            /* INIT shmem items: */
            // The trick with locking the mutex inside the construction is that I always put it on the left of parentheses
            // When you have (a, b, c, d); with a, b, c, d being valid C++ statements, all of them get executed, but ONLY d's value is the final value of the entire expression
            m_mtxForInit(std::make_shared<bip::named_mutex>(bip::open_or_create, m_strMtxForInit.c_str(), UnrestrictedPerms().perms)),
            m_pSegment((m_mtxForInit->lock(), // must LOCK MUTEX before creating/opening segment and populating it w metainfo (see the a,b,c,d comment above)
                std::make_shared<Segment>(Segment(bip::open_or_create, m_acSharedMemName, m_PoolParamsLocal.m_uiOverallShMemSize, 0, UnrestrictedPerms().perms)))),
            //Note, there is an unlikely-but-possible race condition: if two separate processes try to simultaneously initialize the segment w different PoolParams,
            //it may happen that ProcA opened the segment w its own params, but then ProbB ended up constructing "MetaInfo" using its own params. This will lead to crashes,
            //eventually, because CreatePtr will incorrectly use ProcB's params, instead of the correct ProbA. This is why
            //m_mtx is being used to make the segment open/create and m_PoolParams metainfo creation into an atomic operation
            m_PoolParams(*m_pSegment->find_or_construct<PoolParamsT>
                ("MetaInfo")
                (m_PoolParamsLocal)
            ),
            // Now construct the queue table in the shmem segment. The {tableindexes,{names_, channels_}} table consists of array of pairs of "queuename" and queue object (queue name is empty initially)
            channels_((m_mtxForInit->unlock(), //safe to UNLOCK MUTEX after the above
                m_pSegment->find_or_construct<NamedBuffer>
                ("ProtectedQueues")[m_PoolParams.m_uiMaxNumOfQueues]
                (m_PoolParams.m_uiQueueSize, m_pSegment->get_segment_manager())
            )),
            names_(m_pSegment->find_or_construct<String>
                ("NamesForQueues")[m_PoolParams.m_uiMaxNumOfQueues]
                (m_pSegment->get_segment_manager())
            ),
            uiNumOfNamedQueues_(*m_pSegment->find_or_construct<size_t>
                ("NumOfNamedQueues")//(std::piecewise_construct, std::forward_as_tuple( segment.get_segment_manager() ), std::forward_as_tuple( m_PoolParams.m_uiQueueSize, segment.get_segment_manager() ) )
                (0)
            ),
            uiNumOfProducers_(*m_pSegment->find_or_construct<size_t>
                ("NumOfAnonymousProducers")//(std::piecewise_construct, std::forward_as_tuple( segment.get_segment_manager() ), std::forward_as_tuple( m_PoolParams.m_uiQueueSize, segment.get_segment_manager() ) )
                (0)
            ),
            uiNumOfConsumers_(*m_pSegment->find_or_construct<size_t>
                ("NumOfAnonymousConsumers")//(std::piecewise_construct, std::forward_as_tuple( segment.get_segment_manager() ), std::forward_as_tuple( m_PoolParams.m_uiQueueSize, segment.get_segment_manager() ) )
                (0)
            ),
            pvecSharedPool(m_pSegment->find_or_construct<std::decay_t<decltype(*pvecSharedPool)>>
                ("PtrPool")//(std::piecewise_construct, std::forward_as_tuple( segment.get_segment_manager() ), std::forward_as_tuple( m_PoolParams.m_uiQueueSize, segment.get_segment_manager() ) )
                (m_pSegment->get_segment_manager())
            ),
            m_uiCurPoolIndex(*m_pSegment->find_or_construct<std::atomic_size_t>
                ("CurPoolIndex")//(std::piecewise_construct, std::forward_as_tuple( segment.get_segment_manager() ), std::forward_as_tuple( m_PoolParams.m_uiQueueSize, segment.get_segment_manager() ) )
                (0)
            )
            
        {
            ///////////////////////
            // Pool Construction //
            ///////////////////////
            // I apologize for this very ugly code below. The issue is that some Elements require an allocator to be passed to their constructor.
            // The allocator can be passed in various (inconsistent) ways and can also be passed to individual members inside Element, e.g.
            // see the very first if-clause (for the workaround for PrintInfoT). See the INITIAL 2 cases (NOT the final 2 cases -- which are explained
            // separately).
            // There are tricks around having to pass the allocator to individual members. For instance, 
            // https://stackoverflow.com/a/57446079. But they would complicate each of the Element classes, merely shifting complexity to them.
            // The Element classes are often very simple structs, so complicating them would be very confusing.
            //
            // In the future, with C++23, when compile-time reflection is available, it will be possible to browse, at compile-time, through
            // each member of Element type, and try to initialize each public member with the allocator. This can be done recursively (which is
            // possible, if compile-time facilities are available).
            //
            // For now, the strategy of individually handling the weird Element cases individually is what was chosen, even though it is
            // obfuscatory inherently.

            /* The FINAL 2 cases below are a device useful to provide construction in 2 common cases:
            1) When the Ptr is to point to a class that accepts custom allocator (e.g. the ShMemIPC::String class), to allocate internal memory.
            2) For simple classes that don't use virtual function tables (which is all the classes that are used here; in particular, POD classes like DataToUI)

            This solution is still imperfect though. For instance, if:

                struct Element
                {
                    int* ptr = new int(4); // BAD!!, this class now relies on heap-allocated memory, which is internal only to the Producer process!

                    struct Subelement // OK, it's fine to have nested objects
                    {
                        float fSomeVal = 23.f; //OK, just a POD variable
                        float &ref = fSomeVal; //OK, ref is just a compile-time alias
                        const float *pf = &fSomeVal; // BAD!!, because even though pf will point to the interprocess memory area, it is an absolute-addressed pointer, while the interprocess area is mapped using relative addressing and base addresses can be different
                    } subel;
                };

                Conclusion: don't allow pointers in the Element types that you pass here!

                Generally speaking, we don't (cannot!) detect whether any member in an arbitrary class is a pointer. Perhaps, C++23 or later will change this, but impossible now.
                Which means that some classes that should be prohibited from being constructed via CreatePtr might still be constructed, to potential detriment of a naif.
                However, the device below, while imperfect, is much better than nothing.
            */

            Element* pObjectPool; // create the pool of actual objects in interprocess memory
            auto &segment = *m_pSegment;

            // Let's handle the weird Element cases first:
            //if constexpr (requires {Element().str; }) <-- I cannot use this on MSVC 16.8, for some reason (and it *should* work, since works on CLANG and GCC)
            if constexpr (dummyconstexprobj<Element>::value == 1) // have to use this dummy func as a hack for the above problem
            {
                //static_assert(1 == 0, "A PrintInfoT case");
                /*vecSharedPool.emplace_back(
                    make_managed_shared_ptr(segment.construct<Element>(unique_id_gen().c_str())
                        (Element{ decltype(Element::ts)(), SharedMemIPC::CharVector{ segment.get_segment_manager() } }), segment)
                );*/
                pObjectPool =
                    segment.find_or_construct<Element>("ObjPool")[m_PoolParams.m_uiPoolSize]
                    (Element{ decltype(Element::ts)(), SharedMemIPC::CharVector{ segment.get_segment_manager() } });
            }
            else if constexpr (dummyconstexprobj<Element>::value == 2) // have to use this dummy func as a hack for the above problem
            {
                //static_assert(1 == 0, "A CHAN_ADD_OR_DEL_CMD case");
                pObjectPool =
                    segment.find_or_construct<Element>("ObjPool")[m_PoolParams.m_uiPoolSize]
                    (Element{ decltype(Element::eCmd)(), SharedMemIPC::String{ segment.get_segment_manager() } });
            }

            // Now we can handle the 2 common cases
            // 1) The case where Element needs an allocator appended.
            else if constexpr (std::is_constructible_v<Element, decltype(segment.get_segment_manager())>)
            {
                //static_assert(1 == 0, "That's where we are");
                pObjectPool =
                    segment.find_or_construct<Element>("ObjPool")[m_PoolParams.m_uiPoolSize]
                    (segment.get_segment_manager());
            }
            // 2) The case where Element doesn't need an allocator (eg all simple POD structs)
            else
            {
                //static_assert(1 == 0, "Why only here?");
                pObjectPool =
                    segment.find_or_construct<Element>("ObjPool")[m_PoolParams.m_uiPoolSize]
                    ();
            }


#           if 1 //!!!!!!!!!!!!!!
            // Now --if it hasn't ALREADY been created -- create the shared pointer pool in interprocess memory:
            if (pvecSharedPool->size() == 0)
            {
                pvecSharedPool->reserve(m_PoolParams.m_uiPoolSize);
                for (auto i = 0u; i < m_PoolParams.m_uiPoolSize; i++)
                {
                    pvecSharedPool->emplace_back(make_managed_shared_ptr(&pObjectPool[i], segment));
                }
            }

#           elif 0
            //Create a shared pointer in shared memory
            //pointing to a newly created object in the segment
            vecSharedPool =
                segment.find_or_construct_it<Ptr<Element>>("PtrPool")[m_PoolParams.m_uiPoolSize]
                //Arguments to construct the shared pointer
                (pObjectPool, segment.get_segment_manager());

#           endif

#if 0
            for (size_t j = 0; j < m_PoolParams.m_uiMaxNumOfQueues; j++)
            {
                auto uiSlots = Queue<Element>::write_available();
                for (decltype(uiSlots) i = 0; i < uiSlots; i++)
                {
                    Queue<Element>::push(Element{});
                }

                for (decltype(uiSlots) i = 0; i < uiSlots; i++)
                {
                    Queue<Element>::pop(t);
                }
            }
#endif
        }



        template<class ... Types>
        Ptr<Element> CreatePtr(Types&&... args)
        {
            auto &segment = *m_pSegment;
            Ptr<Element> pCurPoolPtr = (*pvecSharedPool)[m_uiCurPoolIndex];

            m_uiCurPoolIndex = (m_uiCurPoolIndex + 1) % m_PoolParams.m_uiPoolSize;

            // if we're passing arguments to CreatePtr, this means that we're intending to override the default object in the pool with our
            // custom object. So, we create a temporary object and copy-assign to the object in the pool which we want to have these values.

            // I don't have many places in the code that do that, but it's nice to have for objects that are POD or at worst, can be constructed
            // if we just append an allocator. Typically, I don't recommend this, because the user can just set individual members *after* invocation
            // of CreatePtr, on an as needed basis. See PrinterClient, for an example.
            if constexpr (sizeof...(args) > 0)
            {
                /*static_assert(std::is_copy_assignable_v<Element>,
                    "Make sure that the type you're assigning has an assignment operator generated by compiler or user-defined");*/

                if constexpr (std::is_constructible_v<Element, Types..., decltype(segment.get_segment_manager())>)
                {
                    //static_assert(1 == 0, "Hi?...");
                    pCurPoolPtr->~Element(); // destroy the obj in the pool, and then construct a new one in its place:
                    new (&*pCurPoolPtr) Element(std::forward<Types>(args)..., segment.get_segment_manager()); //Element(std::forward<Types>(args)..., segment.get_segment_manager());
                    //Note, the funky &* is used, because * is overloaded operator that gives us the reference to the object. Applying & on a reference results in address of the object the ref points to.
                    //Alternatively, you can use pCurPoolPtr.get().get() -- the first get obtains offset_ptr. the next get obtains the actual address from offset_ptr
                    //static_assert (false, "????");
                }
                else if constexpr (std::is_constructible_v<Element, Types...>)
                {
                    //static_assert(1 == 0, "Hi...");
                    pCurPoolPtr->~Element(); // destroy the obj in the pool, and then construct a new one in its place:
                    new (&*pCurPoolPtr) Element(std::forward<Types>(args)...);
                    //Note, the funky &* is used, because * is overloaded operator that gives us the reference to the object. Applying & on a reference results in address of the object the ref points to.
                    //Alternatively, you can use pCurPoolPtr.get().get() -- the first get obtains offset_ptr. the next get obtains the actual address from offset_ptr
                    //static_assert (false, "????");
                }
                else
                {
                    //static_assert (false, "Don't know how to construct object of type 'Element'");
                    [] <bool flag = false>()
                    {
                        static_assert(flag, "Don't know how to construct object of type 'Element'");
                    }();
                }
            }

            return pCurPoolPtr;

        }
    };


#if 0
        template<class ... Types>
        Ptr<Element> CreatePtr(Types&&... args)
        {
            auto unique_id_gen = [&]()->std::string
            {
                return std::string("element") + std::to_string(++Channel<Element>::m_channel.uiElementCounter);
            };

            /* The device below is useful to provide construction in 2 cases:
            1) When the Ptr is to point to a class that accepts custom allocator (e.g. the ShMemIPC::String class), to allocate internal memory.
            2) For simple classes that don't use virtual function tables (which is all the classes that are used here; in particular, DataToUI)

            This solution is still imperfect though. For instance, if:

                struct Element
                {
                    int* ptr = new int(4); // BAD!!, this class now relies on heap-allocated memory, which is internal only to the Producer process!

                    struct Subelement // OK, it's fine to have nested objects
                    {
                        float fSomeVal = 23.f; //OK, just a POD variable
                        float &ref = fSomeVal; //OK, ref is just a compile-time alias
                        const float *pf = &fSomeVal; // BAD!!, because even though pf will point to the interprocess memory area, it is an absolute-addressed pointer, while the interprocess area is mapped using relative addressing and base addresses can be different
                    } subel;
                };

                Conclusion: don't allow pointers in the Element types that you pass here!

                Generally speaking, we don't (cannot!) detect whether any member in an arbitrary class is a pointer. Perhaps, C++23 or later will change this, but impossible now.
                Which means that some classes that should be prohibited from being constructed via CreatePtr might still be constructed, to potential detriment of a naif.
                However, the device below, while imperfect, is much better than nothing.
            */

            // if the object is non-trivially copyable but accepts customizable boost::interprocess allocators, then assume that to construct it, we have to forward those args and affix the allocator (segment_manager from boost::interprocess)
            if constexpr (!std::is_trivially_copyable_v<Element> &&
                std::is_constructible_v<Element, Types..., decltype((Channel<Element>::segment).get_segment_manager())>)
            {
                //static_assert(1 == 0, "That's where we are");
                return make_managed_shared_ptr(Channel<Element>::segment.construct<Element>(unique_id_gen().c_str())
                    (std::forward<Types>(args)..., (Channel<Element>::segment).get_segment_manager()), Channel<Element>::segment);
            }
            else //if constexpr (std::is_trivially_copyable_v<Element>)// Assume that we don't allocate anything outside the class's footprint
                //(i.e. no extra allocations in the object, ie we can copy it with a simple memcopy, just knowing the compile-time sizeof of the class)
            {
                //static_assert(1 == 0, "Why only here?");
                return make_managed_shared_ptr(Channel<Element>::segment.construct<Element>(unique_id_gen().c_str())
                    (std::forward<Types>(args)...), Channel<Element>::segment);
            }
            //else static_assert(false, "Your object is not supported in this routine at this time."); // your class probably has virtual table pointer and/or does custom copy/move/assignment construction
        }

        std::vector<SharedMemIPC::Ptr<Element>> vecSharedPool; // a pool pre-allocated bip::shared_ptrs to interprocess shared memory, ready-made for fast and easy production of elements to push into the queue
};
#elif 0
        template<class ... Types>
        Ptr<Element> CreatePtr(Types&&... args)
        {
            auto unique_id_gen = [&]()->std::string
            {
                return std::string("element") + std::to_string(++Channel<Element>::m_channel.uiElementCounter);
            };

            /* The device below is useful to provide construction in 2 cases:
            1) When the Ptr is to point to a class that accepts custom allocator (e.g. the ShMemIPC::String class), to allocate internal memory.
            2) For simple classes that don't use virtual function tables (which is all the classes that are used here; in particular, DataToUI)

            This solution is still imperfect though. For instance, if:

                struct Element
                {
                    int* ptr = new int(4); // BAD!!, this class now relies on heap-allocated memory, which is internal only to the Producer process!

                    struct Subelement // OK, it's fine to have nested objects
                    {
                        float fSomeVal = 23.f; //OK, just a POD variable
                        float &ref = fSomeVal; //OK, ref is just a compile-time alias
                        const float *pf = &fSomeVal; // BAD!!, because even though pf will point to the interprocess memory area, it is an absolute-addressed pointer, while the interprocess area is mapped using relative addressing and base addresses can be different
                    } subel;
                };

                Conclusion: don't allow pointers in the Element types that you pass here!

                Generally speaking, we don't (cannot!) detect whether any member in an arbitrary class is a pointer. Perhaps, C++23 or later will change this, but impossible now.
                Which means that some classes that should be prohibited from being constructed via CreatePtr might still be constructed, to potential detriment of a naif.
                However, the device below, while imperfect, is much better than nothing.
            */

            return make_managed_shared_ptr(Channel<Element>::segment.construct<Element>(unique_id_gen().c_str())
                (std::forward<Types>(args)...), Channel<Element>::segment);

            //std::vector<SharedMemIPC::Ptr<Element>> vecSharedPool; // a pool pre-allocated bip::shared_ptrs to interprocess shared memory, ready-made for fast and easy production of elements to push into the queue
        }
    };
#endif


#if !_HAS_CXX17 
    template <typename Element, typename SemaphoreType>
    typename MemoryPool<typename Element, typename SemaphoreType>::PoolParamsT MemoryPool<Element, SemaphoreType>::_PoolParamsDummy{};
    template <typename Element, typename SemaphoreType>
    size_t MemoryPool<Element, SemaphoreType>::uiDummy{};
    template <typename Element, typename SemaphoreType>
    std::atomic_size_t MemoryPool<Element, SemaphoreType>::uiatomicDummy{};
#endif // _HAS_CXX17




    template <typename Element, typename SemaphoreType = DefaultSemaphore, typename MutexType = NullMutex>
    struct Producer// : public Channel<Element, SemaphoreType >
    {
        // Add some type reflection, as utility:
        using ElementType = Element;
        using ElementPtrType = Ptr<Element>;
        using GetAnonymousProducerT = typename MemoryPool<Element, SemaphoreType>::GetAnonymousProducerT;
        _INLINE_VAR constexpr static GetAnonymousProducerT GetAnonymousProducer{}; // define tag for convenience of use by others

        using MemoryPoolT = MemoryPool<Element, SemaphoreType>;
        MemoryPoolT m_MemPool{}; // the shmem pool for queues and actual data
        Channel<Element, SemaphoreType>& m_channel; // the channel/queue from mempool
        MutexType m_Mtx{}; //protects, in case several threads/processes are accessing simultaneously (applicable to ControlProducers only)

        // legacy style (1 producer per 1 memory pool)
        Producer(const char* acShMemName, size_t uiOverallShMemSize = 0/*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(acShMemName, 1, uiOverallShMemSize, uiQueueSize, uiPoolSize),
            m_channel(m_MemPool.FindOrConstructChannel("UniqueQueue"))
        {}


        // newest style (note the extra param acQueueName) (N named producers per 1 memory pool)
        Producer(MemoryPoolT MemPool_, const char* acQueueName, const FindOrConstructT &flag = FindOrConstruct) :
            m_MemPool(MemPool_),
            m_channel
            (flag == FindOrConstruct ? 
                m_MemPool.FindOrConstructChannel(acQueueName) : // we can create it by name
                (flag == ConstructAnonymous ? 
                    m_MemPool.FindOrConstructChannel(GetAnonymousProducer) : // we can create it
                    m_MemPool.FindChannel(acQueueName) // someone else pre-created it
			    )
			)
        {}

#if 0
        // new style (note the extra param acQueueName) (N named producers per 1 memory pool)
        Producer(MemoryPoolT MemPool_, const char* acQueueName, FindOrConstructT va = FindOrConstructT) :
            m_MemPool(MemPool_),
            m_channel(m_MemPool.FindChannel(acQueueName))
        {}
#endif

        // new style (note the extra param GetAnonymousProducer) (N unnamed producers per 1 memory pool)
        Producer(MemoryPoolT MemPool_, GetAnonymousProducerT GetAnonymousProducer) :
            m_MemPool(MemPool_),
            m_channel(m_MemPool.FindOrConstructChannel(GetAnonymousProducer))
        {}


#if 0
        // new style (note the extra param acQueueName) (N named producers per 1 memory pool)
        Producer(const char* acShMemName, const char* acQueueName, size_t uiOverallShMemSize = 0 /*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(PoolParams<Element, SemaphoreType>(acShMemName, 0, uiOverallShMemSize, uiQueueSize, uiPoolSize)),
            m_channel(m_MemPool.FindOrConstructChannel(acQueueName))
        {}

        // new style (note the extra param GetAnonymousProducer) (N unnamed producers per 1 memory pool)
        Producer(const char* acShMemName, GetAnonymousProducerT GetAnonymousProducer, size_t uiOverallShMemSize = 0 /*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(PoolParams<Element, SemaphoreType>(acShMemName, 0, uiOverallShMemSize, uiQueueSize, uiPoolSize)),
            m_channel(m_MemPool.FindOrConstructChannel(GetAnonymousProducer))
        {}
#endif

        // this is a blocking op
        bool Push(Ptr<Element> const& t)
        {
            std::scoped_lock lock(m_Mtx);
            return m_channel.Push(t);
        }

        size_t WriteAvailable() const
        {
            return m_channel.WriteAvailable();
        }

        template<class ... Types>
        Ptr<Element> CreatePtr(Types&&... args)
        {
            std::scoped_lock lock(m_Mtx);
            return m_MemPool.CreatePtr(std::forward<Types>(args)...);
        }
    };

#if 0
    template <typename Element, typename SemaphoreType = DefaultSemaphore>
    struct Member
    {
        Member() = default;
        Member(const Member&) = default;
        Member(Member&&) = default;

#if 0
        struct UnrestrictedPerms
        {
            UnrestrictedPerms() { perms.set_unrestricted(); }
            bip::permissions perms;
        };
#endif

        PoolParams<Element, SemaphoreType> m_PoolParams{"", 0, 0, 0, 0};
        //Segment segment;

#if 0
        //TBD inspired by https://stackoverflow.com/questions/26280041/how-to-i-create-a-boost-interprocess-vector-of-interprocess-containers
        struct StrBuffPair
        {
            StrBuffPair() = delete;
            StrBuffPair(size_t n, Manager m) : s(m), b(n, m) {}
            String s;
            Channel<Element, SemaphoreType> b;
        };


        using void_allocator = boost::container::scoped_allocator_adaptor<allocator<void, Manager>>;

        class complex_data
        {
        public:
            String       strName;
            Channel<Element, SemaphoreType> queue;

            //Since void_allocator is convertible to any other allocator<T>, we can simplify
            //the initialization taking just one allocator for all inner containers.
            typedef void_allocator allocator_type;

            complex_data(complex_data const& other, const allocator_type& void_alloc)
                : strName(other.strName, void_alloc), queue(other.queue, void_alloc)
            {}
            complex_data(const allocator_type& void_alloc)
                : strName(void_alloc), queue(void_alloc)
            {}
            //Other members...
            //
        };
#endif
        //using NamedBuffer = std::pair<String, Channel<Element, SemaphoreType>>;
        //using NamedBuffer = StrBuffPair;
#if 1
        using NamedBuffer = Channel<Element, SemaphoreType>;

        NamedBuffer* channels_;
        String* names_; // names for the above
        //size_t& uiNumOfNamedQueues_; // overall size for the above pair of queues and names

        //Channel<Element, SemaphoreType> m_channel;
        //size_t m_uiPoolSize = 0;
        //size_t uiOverallShMemSize = 0;
        //std::vector<Ptr<Element>> vecSharedPool; // a pool pre-allocated bip::shared_ptrs to interprocess shared memory, ready-made for fast and easy production of elements to push into the queue
        Ptr<Element>* vecSharedPool;
        //std::atomic<uint32_t> m_uiCurPoolIndex = 0;
        //std::atomic<float> m_uiCurPoolIndex{ 0.f };
#endif
    };
#endif

    template <typename Element, typename SemaphoreType = DefaultSemaphore, typename MutexType = NullMutex>
    struct Consumer //: public Channel<Element, SemaphoreType>
    {
        // Add some type reflection, as utility:
        using ElementType = Element;
        using ElementPtrType = Ptr<Element>;
        using GetAnonymousConsumerT = typename MemoryPool<Element, SemaphoreType>::GetAnonymousConsumerT;
        _INLINE_VAR constexpr static GetAnonymousConsumerT GetAnonymousConsumer{};

        using MemoryPoolT = MemoryPool<Element, SemaphoreType>;
        MemoryPoolT m_MemPool;
        Channel<Element, SemaphoreType>& m_channel;
        MutexType m_Mtx{}; //protects, in case several threads/processes are accessing simultaneously (applicable to ControlConsumers only)

        Consumer() = default;
        Consumer(const Consumer&) = default;
        Consumer(Consumer&&) = default;


#if 0
        Member<Element, SemaphoreType> Member_;
        // legacy style
        Consumer(const char* acShMemName) : Member_()
            //m_MemPool(PoolParams<Element, SemaphoreType>(acShMemName, 1, 0, 0, 0))
            //m_channel(m_MemPool.FindOrConstructChannel("UniqueQueue"))
        {}
#else

        // legacy style
        Consumer(const char* acShMemName, size_t uiOverallShMemSize = 0/*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(acShMemName, 1, uiOverallShMemSize, uiQueueSize, uiPoolSize),
            m_channel(m_MemPool.FindOrConstructChannel("UniqueQueue"))
        {}


        // newest style (note the extra param acQueueName) (N named producers per 1 memory pool)
        Consumer(MemoryPoolT MemPool_, const char* acQueueName, const FindOrConstructT &flag = FindOrConstruct) :
            m_MemPool(MemPool_),
            m_channel
            (flag == FindOrConstruct ?
                m_MemPool.FindOrConstructChannel(acQueueName) : // we can create it by name
                (flag == ConstructAnonymous ?
                    m_MemPool.FindOrConstructChannel(GetAnonymousConsumer) : // we can create it
                    m_MemPool.FindChannel(acQueueName) // someone else pre-created it
                    )
            )
        {}

#if 0
        // new style (note the extra param acQueueName) (N named Consumers per 1 memory pool)
        Consumer(MemoryPool<Element, SemaphoreType> MemPool_, const char* acQueueName) :
            m_MemPool(MemPool_),
            m_channel(m_MemPool.FindOrConstructChannel(acQueueName))
        {}
#endif

        // new style (note the extra param GetAnonymousConsumer) (N unnamed Consumers per 1 memory pool)
        Consumer(MemoryPool<Element, SemaphoreType> MemPool_, GetAnonymousConsumerT GetAnonymousConsumer) :
            m_MemPool(MemPool_),
            m_channel(m_MemPool.FindOrConstructChannel(GetAnonymousConsumer))
        {}

#if 0
        // new style (note the extra param acQueueName) (N named consumers per 1 memory pool)
        Consumer(const char* acShMemName, const char* acQueueName, size_t uiOverallShMemSize = 0 /*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(PoolParams<Element, SemaphoreType>(acShMemName, 0, uiOverallShMemSize, uiQueueSize, uiPoolSize)),
            m_channel(m_MemPool.FindOrConstructChannel(acQueueName))
        {}

        // new style (note the extra param GetAnonymousConsumer) (N unnamed consumers per 1 memory pool)
        Consumer(const char* acShMemName, GetAnonymousConsumerT GetAnonymousConsumer, size_t uiOverallShMemSize = 0 /*= (4 << 20)*/, size_t uiQueueSize = 50, size_t uiPoolSize = 0) :
            m_MemPool(PoolParams<Element, SemaphoreType>(acShMemName, 0, uiOverallShMemSize, uiQueueSize, uiPoolSize)),
            m_channel(m_MemPool.FindOrConstructChannel(GetAnonymousConsumer))
        {}
#endif
#endif



        // this is a blocking op
        bool Pop(Ptr<Element>& t)
        {
            //m_Mtx.lock();
            return m_channel.Pop(t);
            //m_Mtx.unlock();
        }

        size_t ReadAvailable() const
        {
            return m_channel.ReadAvailable();
        }
    };


    template <typename Element>
    using BlockingProducer = Producer<Element, DefaultSemaphore, NullMutex>;

    template <typename Element>
    using BlockingConsumer = Consumer<Element, DefaultSemaphore, NullMutex>;

    template <typename Element>
    using NonBlockingProducer = Producer<Element, NullSemaphore, NullMutex>;

    template <typename Element>
    using NonBlockingConsumer = Consumer<Element, NullSemaphore, NullMutex>;

    // this one is good for situation where producers in multiple processes/threads
    //want to use SAME queue (usually, for control/cmd purposes)
    template <typename Element> 
    using SharedProducer = Producer<Element, NullSemaphore, bip::ipcdetail::winapi_mutex>;

    // this one is good for situation where consumers in multiple processes/threads
    //want to use SAME queue (usually, as worker threads that are fed from same queue)
    template <typename Element> 
    using SharedConsumer = Consumer<Element, NullSemaphore, bip::ipcdetail::winapi_mutex>;
}
