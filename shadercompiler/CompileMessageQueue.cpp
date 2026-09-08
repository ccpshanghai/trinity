// Copyright © 2011 CCP ehf.

#include "stdafx.h"
#include "CompileMessageQueue.h"


CompileMessageQueue::CompileMessageQueue() :
	m_stop( false )
{
	m_thread = std::thread( [this] { this->Run(); } );
}

CompileMessageQueue::~CompileMessageQueue()
{
	m_stop = true;

	m_queueEvent.notify_one();
	m_thread.join();
}

void CompileMessageQueue::AddMessages( ID3DBlob* buffer )
{
	m_messagesMutex.lock();
	m_messages.push( reinterpret_cast<char*>( buffer->GetBufferPointer() ) );
	m_messagesMutex.unlock();
	m_queueEvent.notify_one();
}

void CompileMessageQueue::AddMessages( IDxcBlobEncoding* blob )
{
	DWORD size = DWORD( blob->GetBufferSize() );
	if( size > 0 )
	{
		m_messagesMutex.lock();
		m_messages.push( reinterpret_cast<char*>( blob->GetBufferPointer() ) );
		m_messagesMutex.unlock();
		m_queueEvent.notify_one();
	}
}

void CompileMessageQueue::AddMessage( const char* format, ... )
{
	va_list args;

	va_start( args, format );
	int count = vsnprintf( nullptr, 0, format, args ) + 1;
	va_end( args );

	std::string message;
	message.resize( count );

	va_start( args, format );
	count = vsnprintf( &message[0], message.size(), format, args );
	va_end( args );

	m_messagesMutex.lock();
	m_messages.push( message );
	m_messagesMutex.unlock();
	m_queueEvent.notify_one();
}

// --------------------------------------------------------------------------------------
// Description:
//   Waits until all messages are printed to stdout.
// --------------------------------------------------------------------------------------
void CompileMessageQueue::Flush()
{
	// m_printing, not just the queue, because Run() pops a message BEFORE it prints it:
	// testing emptiness alone returns while the output thread is still inside printf.
	// Measured 2026-09-08 on macOS by VulkanCompilerTest.ASecondResourceArrayIsRefusedRatherThanMisbound,
	// which flushes and then reads gtest's captured stdout: the compiler's diagnostic was
	// correct and complete, and arrived after the capture had been restored. The tool
	// itself never lost a message to this -- the destructor joins the output thread -- so
	// what was broken was this function's contract, which its own comment states.
	std::unique_lock lock( m_messagesMutex );
	while( !m_messages.empty() || m_printing )
	{
		m_queueEvent.notify_one();
		m_idleEvent.wait_for( lock, std::chrono::milliseconds( 50 ) );
	}
}

void CompileMessageQueue::SetEntryFileName( const char* fileName )
{
	m_entryFileName = fileName;
}

const char* CompileMessageQueue::GetEntryFileName() const
{
	return m_entryFileName.c_str();
}

// --------------------------------------------------------------------------------------
// Description:
//   Output thread routine: prints unique messages from the queue.
// --------------------------------------------------------------------------------------
void CompileMessageQueue::Run()
{
	while( true )
	{
		{
			std::unique_lock lock( m_messagesMutex );
			m_queueEvent.wait( lock, [this] { return !m_messages.empty() || m_stop; } );
		}
		while( true )
		{
			m_messagesMutex.lock();
			if( !m_messages.empty() )
			{
				std::string buffer = m_messages.front();
				m_messages.pop();
				m_printing = true;
				m_messagesMutex.unlock();

				OutputMessages( buffer.c_str(), buffer.size() );

				m_messagesMutex.lock();
				m_printing = false;
				m_messagesMutex.unlock();
				m_idleEvent.notify_all();
			}
			else
			{
				m_messagesMutex.unlock();
				break;
			}
		}
		if( m_stop )
		{
			break;
		}
	}
}

// --------------------------------------------------------------------------------------
// Description:
//   Prints unique messages from the input string of several messages.
// Arguments:
//   messages - A string containing several messages. A message is delimed by new line
//              followed by non-space character
// --------------------------------------------------------------------------------------
void CompileMessageQueue::OutputMessages( const char* messages, size_t length )
{
	const char* start = messages;
	const char* end = messages + 1;
	while( *start && start - messages < ptrdiff_t( length ) )
	{
		if( *end == 0 || end - messages >= ptrdiff_t( length ) || ( end[0] == '\n' && ( end - messages + 1 >= ptrdiff_t( length ) || !isspace( end[1] ) ) ) )
		{
			std::string message( start, end );
			if( m_printedMessages.insert( message ).second )
			{
				const char* memoryRefs[] = { "\\memory(", "/memory(", "/memory:", "memory:" };
				for( auto memoryRef : memoryRefs )
				{
					auto pos = message.find( memoryRef );
					if( pos != std::string::npos )
					{
						message.replace( 0, pos + strlen( memoryRef ) - 1, m_entryFileName );
					}
				}
				printf( "%s\n", message.c_str() );
				fflush( stdout );
			}
			if( *end == 0 )
			{
				return;
			}
			start = end + 1;
		}
		++end;
	}
}
