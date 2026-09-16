// Copyright © 2011 CCP ehf.

#pragma once
#ifndef CompileMessageQueue_H
#define CompileMessageQueue_H

// --------------------------------------------------------------------------------------
// Description:
//   Maintains a queue of output compile error/warning messages and outputs unique messages
//   to stdout. Uses a separate thread for outputting messages.
// --------------------------------------------------------------------------------------
class CompileMessageQueue
{
public:
	CompileMessageQueue();
	~CompileMessageQueue();

	// Both blob types come from headers macOS has as well since the directx-dxc port
	// started installing them: ID3DBlob from d3dcommon.h, IDxcBlobEncoding from dxcapi.h.
	// The second overload is how the SPIR-V path reports dxc's own diagnostics.
	void AddMessages( ID3DBlob* buffer );
	void AddMessages( IDxcBlobEncoding* buffer );
	void AddMessage( const char* format, ... );

	void Flush();
	void SetEntryFileName( const char* fileName );

	const char* GetEntryFileName() const;

private:
	void Run();
	void OutputMessages( const char* messages, size_t length );

	// Message queue
	std::queue<std::string> m_messages;
	std::mutex m_messagesMutex;
	std::condition_variable m_queueEvent;
	std::thread m_thread;

	// True from the moment the output thread takes a message off the queue until it has
	// finished printing it. Without this, "the queue is empty" and "everything has been
	// printed" are not the same statement and Flush() answers the first one -- see Flush().
	bool m_printing = false;
	std::condition_variable m_idleEvent;

	// A set of already printed messages
	std::set<std::string> m_printedMessages;
	// Entry point file name (for fixing compiler messages)
	std::string m_entryFileName;
	// Flag to stop the message processing thread
	std::atomic<bool> m_stop;
};

#endif // CompileMessageQueue_H
