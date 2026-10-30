// Copyright © 2013 CCP ehf.

#pragma once
#ifndef ALLog_H
#define ALLog_H

namespace CCP
{
inline CcpLogChannel_t& GetTrinityALChannel()
{
	static CcpLogChannel_t s_moduleChannel = CCP_LOG_DEFINE_CHANNEL( "TrinityAL" );
	return s_moduleChannel;
}
}

#define CCP_AL_LOG( ... ) CCP_LOG_CH( CCP::GetTrinityALChannel(), __VA_ARGS__ )
#define CCP_AL_LOGERR( ... ) CCP_LOGERR_CH( CCP::GetTrinityALChannel(), __VA_ARGS__ )
#define CCP_AL_LOGNOTICE( ... ) CCP_LOGNOTICE_CH( CCP::GetTrinityALChannel(), __VA_ARGS__ )
#define CCP_AL_LOGWARN( ... ) CCP_LOGWARN_CH( CCP::GetTrinityALChannel(), __VA_ARGS__ )

#include <cstdint>
#include <map>
#include <mutex>
#include <set>

namespace CCP
{
// True the first time `key` is seen for `category`, false every time after. A category is an
// address that belongs to one call site: a function-local `static char`, not const -- a linker
// folding identical constants (MSVC /OPT:ICF, gold --icf) may give two const sites one address,
// and the two would then share their keys. A key is whatever the site
// wants one line per -- a pipeline, a program, a (layout, binding) pair. At most
// LOG_ONCE_KEYS_AL keys are remembered per category, and a full category answers false for any
// new key, so a scene that creates objects without end costs a bounded amount of memory and
// log. A handle the driver reuses after destruction stays silent; this is for diagnostics.
// Thread-safe. The state is never destroyed: a static destructor in one module may log after
// another module's finalizers have run (native-linux-desktop spec, section 7).
inline bool LogOnceAL( const void* category, uint64_t key )
{
	static const size_t LOG_ONCE_KEYS_AL = 256;
	struct State
	{
		std::mutex mutex;
		std::map<const void*, std::set<uint64_t>> seen;
	};
	static State* s_state = new State();
	std::lock_guard<std::mutex> lock( s_state->mutex );
	auto& seen = s_state->seen[category];
	if( seen.size() >= LOG_ONCE_KEYS_AL )
	{
		return false;
	}
	return seen.insert( key ).second;
}
}


#endif