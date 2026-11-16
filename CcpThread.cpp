// Copyright © 2013 CCP ehf.

#include <CcpLog.h>

#include "include/CcpThread.h"
#include "include/CcpMemory.h"

namespace
{
	struct CreateThreadData
	{
		CcpThreadProc_t functionToCall;
		void* context;
	};
}

#if _WIN32

#include <map>

namespace
{
	std::map<CcpThreadHandle_t,CcpThreadId_t> s_handleToId;

	DWORD WINAPI ThreadProcHelper( void* context )
	{
		CreateThreadData* data = reinterpret_cast<CreateThreadData*>( context );
		CcpThreadProc_t f = data->functionToCall;
		void* ctx = data->context;
		CCP_DELETE data;

		return f( ctx );
	}
}

CcpThreadId_t CcpGetCurrentThreadId()
{
	return GetCurrentThreadId();
}

CcpThreadHandle_t CcpCreateThread( CcpThreadProc_t threadProc, void* context, CcpThreadPriority_t priority )
{
	CreateThreadData* data = CCP_NEW( "CcpCreateThread/data" ) CreateThreadData;
	data->functionToCall = threadProc;
	data->context = context;

	CcpThreadId_t id;
	CcpThreadHandle_t threadHandle = CreateThread( 0, 0, ThreadProcHelper, data, CREATE_SUSPENDED, &id );
	s_handleToId[threadHandle] = id;
	SetThreadPriority( threadHandle, priority );

	ResumeThread( threadHandle );

	return threadHandle;
}

CcpThreadId_t CcpGetThreadId( CcpThreadHandle_t handle )
{
	return s_handleToId[handle];
}

int CcpJoinThread( CcpThreadHandle_t threadHandle, uint32_t& result )
{
	return CcpJoinThreadWithTimeout( threadHandle, INFINITE, result );
}

int CcpJoinThreadWithTimeout( CcpThreadHandle_t threadHandle, uint32_t timeoutInMs, uint32_t& result )
{
	DWORD s = WaitForSingleObject( threadHandle, timeoutInMs );
	if( s == 0 )
	{
		GetExitCodeThread( threadHandle, (DWORD*)&result );
		::CloseHandle( threadHandle );
		return 0;
	}
	else
	{
		// TODO: Translate error code
		return s;
	}
}

bool CcpSetThreadPriority( CcpThreadHandle_t thread, CcpThreadPriority_t priority )
{
    return SetThreadPriority( thread, priority ) != 0;
}

void CcpThreadSleep( uint32_t sleepTimeInMs )
{
	Sleep( sleepTimeInMs );
}


void CcpKillThread( CcpThreadHandle_t threadHandle )
{
	TerminateThread( threadHandle, 0 );
}

void CcpSetThreadPriority( CcpThread& thread, CcpThreadPriority_t priority )
{
	if( thread.joinable() )
	{
		SetThreadPriority( thread.native_handle(), priority );
	}
}

bool CcpGetThreadTimes( int64_t& kernelTime, int64_t& userTime )
{
    FILETIME dummy;
    return GetThreadTimes( GetCurrentThread(), &dummy, &dummy, (LPFILETIME)&kernelTime, (LPFILETIME)&userTime ) != 0;
}
#elif _POSIX_THREADS
#include <sys/time.h>
#if __ANDROID__
#include <unistd.h>
#include <atomic>
#endif

namespace
{
	void* ThreadProcHelper( void* context )
	{
		CreateThreadData* data = reinterpret_cast<CreateThreadData*>( context );
		CcpThreadProc_t f = data->functionToCall;
		void* ctx = data->context;
		CCP_DELETE data;

		return (void*)(ptrdiff_t)f( ctx );
	}
}

CcpThreadHandle_t CcpCreateThread( CcpThreadProc_t threadProc, void* context, CcpThreadPriority_t priority )
{
	pthread_attr_t attr;
	if( pthread_attr_init( &attr ) != 0 )
	{
		return CcpThreadHandle_t();
	}

	CreateThreadData* data = CCP_NEW( "CcpCreateThread/data" ) CreateThreadData;
	data->functionToCall = threadProc;
	data->context = context;

	CcpThreadHandle_t threadHandle;
	int s = pthread_create( &threadHandle, &attr, ThreadProcHelper, data );

    if( priority != CCP_THREAD_PRIORITY_NORMAL )
    {
        CcpSetThreadPriority( threadHandle, priority );
    }
    pthread_attr_destroy( &attr );

	if( s == 0 )
	{
		return threadHandle;
	}
	else
	{
		return CcpThreadHandle_t();
	}
}

int CcpJoinThread( CcpThreadHandle_t threadHandle, uint32_t& result )
{
	void* threadResult = nullptr;
	int s = pthread_join( threadHandle, &threadResult );
	if( s == 0 )
	{
		result = uint32_t( uintptr_t( threadResult ) );
		return 0;
	}
	else
	{
		// TODO: Translate error code
		return s;
	}
}

#if __ANDROID__

namespace
{
    // bionic ships no pthread_cancel, so the generic POSIX shape in the #else branch -- cancel the helper thread
    // once the timeout expires -- has no counterpart. Two consequences shape this version.
    // The helper cannot be stopped, so it must not touch anything the caller owns after the
    // caller has given up: the shared block is heap allocated and refcounted, and whichever
    // side finishes last frees it. And it can never be joined, so it detaches itself.
    struct TimedJoinBlock
    {
        CcpThreadHandle_t threadHandle;
        std::atomic<bool> done;
        std::atomic<int> refs;
        void* threadResult;
    };

    void ReleaseTimedJoinBlock( TimedJoinBlock* block )
    {
        if( block->refs.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
        {
            CCP_DELETE block;
        }
    }

    void* TimedJoinHelper( void* arg )
    {
        TimedJoinBlock* block = static_cast<TimedJoinBlock*>( arg );

        pthread_join( block->threadHandle, &block->threadResult );
        block->done.store( true, std::memory_order_release );
        ReleaseTimedJoinBlock( block );
        return nullptr;
    }

    uint32_t GetTicks()
    {
        timeval tv;

        gettimeofday( &tv, nullptr );
        return uint32_t( tv.tv_usec / 1000 + tv.tv_sec * 1000 );
    }
}

int CcpJoinThreadWithTimeout( CcpThreadHandle_t threadHandle, uint32_t timeoutInMs, uint32_t& result )
{
    uint32_t start = GetTicks();

    TimedJoinBlock* block = CCP_NEW( "CcpJoinThreadWithTimeout/block" ) TimedJoinBlock;
    block->threadHandle = threadHandle;
    block->threadResult = nullptr;
    block->done.store( false, std::memory_order_relaxed );
    block->refs.store( 2, std::memory_order_relaxed );

    CcpThreadHandle_t helper;
    if( pthread_create( &helper, nullptr, &TimedJoinHelper, block ) != 0 )
    {
        block->refs.store( 1, std::memory_order_relaxed );
        ReleaseTimedJoinBlock( block );
        return -1;
    }
    pthread_detach( helper );

    do
    {
        if( block->done.load( std::memory_order_acquire ) )
        {
            break;
        }
        CcpThreadSleep( 10 );
    }
    while( GetTicks() - start < timeoutInMs );

    if( !block->done.load( std::memory_order_acquire ) )
    {
        // The helper is still blocked in pthread_join and stays there until the target thread
        // exits. It owns the block from here on -- nothing leaks, but the helper does outlive
        // this call, which the POSIX version does not allow.
        ReleaseTimedJoinBlock( block );
        return 1;
    }
    result = uint32_t( uintptr_t( block->threadResult ) );
    ReleaseTimedJoinBlock( block );
    return 0;
}

#else

namespace
{

    struct JoinTimeoutData
    {
        CcpThreadHandle_t threadHandle;
        bool done;
        void* threadResult;
    };
    
    void* JoinTimeoutHelper( void* arg )
    {
        JoinTimeoutData* data = static_cast<JoinTimeoutData*>( arg );
        
        pthread_join( data->threadHandle, &data->threadResult );
        data->done = true;
        return nullptr;
    }
    
    uint32_t GetTicks()
    {
        timeval tv;
        
        gettimeofday( &tv, nullptr );
        return uint32_t( tv.tv_usec / 1000 + tv.tv_sec * 1000 );
    }
}

int CcpJoinThreadWithTimeout( CcpThreadHandle_t threadHandle, uint32_t timeoutInMs, uint32_t& result )
{
    uint32_t start = GetTicks();
    
    JoinTimeoutData data;
    data.threadHandle = threadHandle;
    data.done = false;
    
    CcpThreadHandle_t id;
    if( pthread_create( &id, nullptr, &JoinTimeoutHelper, &data ) != 0 )
    {
        return -1;
    }
    do
    {
        if( data.done )
        {
            break;
        }
        CcpThreadSleep( 10 );
    }
    while( GetTicks() - start < timeoutInMs );
    if( !data.done )
    {
        pthread_cancel( id );
        return 1;
    }
    pthread_join( id, nullptr );
    result = uint32_t( uintptr_t( data.threadResult ) );
    return 0;
}

#endif

bool CcpSetThreadPriority( CcpThreadHandle_t thread, CcpThreadPriority_t priority )
{
    int policy;
    sched_param param;
    if( pthread_getschedparam( thread, &policy, &param ) )
    {
        return false;
    }
    
    auto minPriority = sched_get_priority_min( policy );
    auto maxPriority = sched_get_priority_max( policy );
    auto normalPriority = ( minPriority + maxPriority ) / 2;

    switch( priority )
    {
    case CCP_THREAD_PRIORITY_LOWEST:
        param.sched_priority = minPriority;
        break;
    case CCP_THREAD_PRIORITY_BELOW_NORMAL:
        param.sched_priority = ( minPriority + normalPriority ) / 2;
        break;
    case CCP_THREAD_PRIORITY_HIGHEST:
        param.sched_priority = maxPriority;
        break;
    case CCP_THREAD_PRIORITY_ABOVE_NORMAL:
        param.sched_priority = ( maxPriority + normalPriority ) / 2;
        break;
    default:
        param.sched_priority = normalPriority;
    }
    return pthread_setschedparam( thread, policy, &param ) == 0;
}

void CcpThreadSleep( uint32_t sleepTimeInMs )
{
	timespec ts;
	ts.tv_sec = sleepTimeInMs / 1000;
	ts.tv_nsec = (sleepTimeInMs % 1000) * 1000000;
	nanosleep( &ts, nullptr );
}

void CcpKillThread( CcpThreadHandle_t threadHandle )
{
#if __ANDROID__
    // bionic implements no thread cancellation at all, deliberately: the position is that
    // cancelling a thread cannot be made safe. pthread_kill would only deliver a signal, and
    // tearing down a thread that holds a lock is exactly what the omission prevents. Nothing
    // in the mobile build calls this; anything that needs it wants a cooperative exit flag.
    ( void )threadHandle;
    CCP_LOGERR( "CcpKillThread does nothing on Android: bionic has no pthread_cancel" );
#else
    pthread_cancel( threadHandle );
#endif
}

void CcpSetThreadPriority( CcpThread& thread, CcpThreadPriority_t priority )
{
    if( !thread.joinable() )
    {
        return;
    }
    CcpSetThreadPriority( thread.native_handle(), priority );
}

#if __APPLE__
#include <mach/thread_act.h>

CcpThreadId_t CcpGetCurrentThreadId()
{
	return pthread_mach_thread_np(pthread_self());
}

CcpThreadId_t CcpGetThreadId( CcpThreadHandle_t handle )
{
	return pthread_mach_thread_np(handle);
}

bool CcpGetThreadTimes( int64_t& kernelTime, int64_t& userTime )
{
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    thread_basic_info_data_t info;
	mach_port_t port = pthread_mach_thread_np( pthread_self() );
	if( !MACH_PORT_VALID( port ) )
	{
		return false;
	}
	int returnCode = thread_info( port, THREAD_BASIC_INFO, (thread_info_t)&info, &count );
    if( returnCode != KERN_SUCCESS )
    {
		return false;
	}
    userTime = int64_t( info.user_time.seconds ) * 10000000 + int64_t( info.user_time.microseconds ) * 10;
    kernelTime = int64_t( info.system_time.seconds ) * 10000000 + int64_t( info.system_time.microseconds ) * 10;
    return true;
}
#else
#include <sys/resource.h>

CcpThreadId_t CcpGetCurrentThreadId()
{
#if __ANDROID__
	// The kernel tid, which is what CcpThreadId_t is a typedef for on Android. Unlike a Mach
	// port name it is also the number logcat, /proc and every Android profiler show.
	return gettid();
#else
	return pthread_self();
#endif
}

CcpThreadId_t CcpGetThreadId( CcpThreadHandle_t handle )
{
#if __ANDROID__
	return pthread_gettid_np(handle);
#else
	return handle;
#endif
}

bool CcpGetThreadTimes( int64_t& kernelTime, int64_t& userTime )
{
	rusage usage{};
	if (getrusage(RUSAGE_THREAD, &usage) != 0)
	{
		return false;
	}

	userTime = usage.ru_utime.tv_sec * 10000000 + usage.ru_utime.tv_usec * 10;
	kernelTime = usage.ru_stime.tv_sec * 10000000 + usage.ru_stime.tv_usec * 10;

	return true;
}
#endif
#endif
