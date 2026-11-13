// Copyright © 2013 CCP ehf.


#include "include/StringConversions.h"

std::wstring UTF8ToWide( const std::string& utf8String )
{
	return UTF8ToWide( utf8String.c_str() );
}

std::string WideToUTF8( const std::wstring& wideString )
{
	return WideToUTF8( wideString.c_str() );
}

#if _WIN32

std::wstring UTF8ToWide( const char* utf8String )
{
	return std::wstring( CA2W( utf8String, CP_UTF8 ) );
}

std::string WideToUTF8( const wchar_t* wideString )
{
	return std::string( CW2A( wideString, CP_UTF8 ) );
}

#else

#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include "CcpMemory.h"


BlueConvertWideToAscii::BlueConvertWideToAscii( const wchar_t* src ) : m_converted( nullptr )
{
	Init( src );
}

BlueConvertWideToAscii::~BlueConvertWideToAscii()
{
	if( m_converted != m_buffer )
	{
		CCP_FREE( (void*)m_converted );
	}
}

void BlueConvertWideToAscii::Init( const wchar_t* src )
{
	size_t sizeNeeded = wcsrtombs( nullptr, &src, 0, nullptr );
	if( sizeNeeded == (size_t)-1 )
	{
		m_converted = m_buffer;
		strcpy( m_converted, "Invalid string" );
		return;
	}

	if( sizeNeeded >= BUFFER_SIZE )
	{
		m_converted = (char*)CCP_MALLOC( "ConvertWideToAscii", sizeNeeded + 1 );
	}
	else
	{
		m_converted = m_buffer;
	}
	wcsrtombs( m_converted, &src, sizeNeeded, nullptr );
	m_converted[sizeNeeded] = 0;
}

BlueConvertAsciiToWide::BlueConvertAsciiToWide( const char* src ) : m_converted( nullptr )
{
	Init( src );
}

BlueConvertAsciiToWide::~BlueConvertAsciiToWide()
{
	if( m_converted != m_buffer )
	{
		CCP_FREE( (void*)m_converted );
	}
}

void BlueConvertAsciiToWide::Init( const char* src )
{
	size_t srcLen = strlen( src );
	size_t sizeNeeded = mbsrtowcs( nullptr, &src, srcLen, nullptr );
	if( sizeNeeded == (size_t)-1 )
	{
		m_converted = m_buffer;
		wcscpy( m_converted, L"Invalid string" );
		return;
	}

	if( sizeNeeded >= BUFFER_SIZE )
	{
		m_converted = (wchar_t*)CCP_MALLOC( "ConvertAsciiToWide", (sizeNeeded + 1) * sizeof( wchar_t ) );
	}
	else
	{
		m_converted = m_buffer;
	}
	mbsrtowcs( m_converted, &src, srcLen, nullptr );
	m_converted[sizeNeeded] = 0;
}

#if defined(__ANDROID__)

// On Apple these two live in StringConversions.mm, which CMakeLists.txt only compiles
// under if(APPLE). Desktop Linux uses the iconv versions further down; Android, which
// also defines __linux__, keeps these and is excluded from the iconv block.
//
// The .mm implementation converts through NSString using UTF32LE, i.e. it assumes a
// 4-byte wchar_t. bionic agrees, and its only supported locale is C.UTF-8, so the
// standard multibyte routines already perform exactly UTF-8 <-> UTF-32 here. That is
// the same assumption the BlueConvert* classes above already make with wcsrtombs.

std::wstring UTF8ToWide( const char* utf8String )
{
	if( utf8String == nullptr )
	{
		return std::wstring();
	}

	size_t sizeNeeded = mbstowcs( nullptr, utf8String, 0 );
	if( sizeNeeded == (size_t)-1 )
	{
		return std::wstring();
	}

	std::wstring result( sizeNeeded, L'\0' );
	mbstowcs( &result[0], utf8String, sizeNeeded );
	return result;
}

std::string WideToUTF8( const wchar_t* wideString )
{
	if( wideString == nullptr )
	{
		return std::string();
	}

	size_t sizeNeeded = wcstombs( nullptr, wideString, 0 );
	if( sizeNeeded == (size_t)-1 )
	{
		return std::string();
	}

	std::string result( sizeNeeded, '\0' );
	wcstombs( &result[0], wideString, sizeNeeded );
	return result;
}

#endif

#endif

#if __linux__ && !defined(__ANDROID__)
#include <iconv.h>

std::wstring UTF8ToWide(const char* utf8String)
{
	std::wstring result;

	const auto descriptor = iconv_open("WCHAR_T", "UTF-8");
	if (descriptor == reinterpret_cast<iconv_t>(-1))
	{
		CCP_LOGERR("Failed to create code conversion descriptor from UTF-8 to WCHAR_T: [errno: %d]", errno);
		return result;
	}

	std::string_view input = utf8String;
	auto inBytesLeft = input.size();
	auto inputBuffer = const_cast<char*>(input.data()); // iconv will increment buffer

	auto outputBufferSize = input.size() * sizeof(wchar_t); // Size is in bytes
	result.resize(outputBufferSize);
	auto outputBuffer = reinterpret_cast<char*>(result.data()); // Calculate buffer position in case backing buffer is reallocated
	auto outBytesLeft = outputBufferSize;
	if (iconv(descriptor, &inputBuffer , &inBytesLeft, &outputBuffer, &outBytesLeft) == static_cast<size_t>(-1))
	{
		CCP_LOGERR("Failed to convert from WCHAR_T to UTF-8: [errno: %d, inBytesLeft: %lu, outBytesLeft: %lu, outputBufferSize: %lu]", errno, inBytesLeft, outBytesLeft, outputBufferSize);
		iconv_close(descriptor);
		return result;
	}

	iconv_close(descriptor);

	return result;
}

std::string WideToUTF8( const wchar_t* wideString )
{
	std::string result;

	const auto descriptor = iconv_open("UTF-8", "WCHAR_T");
	if (descriptor == reinterpret_cast<iconv_t>(-1))
	{
		CCP_LOGERR("Failed to create code conversion descriptor from UTF-8 to WCHAR_T: [errno: %d]", errno);
		return result;
	}

	const std::wstring_view input = wideString;
	auto inBytesLeft = input.size() * sizeof(wchar_t);
	auto inputBuffer = const_cast<char*>(reinterpret_cast<const char*>(input.data())); // iconv will increment buffer

	// UTF8 code points require four bytes at most
	auto outputBufferSize = input.size() * 4; // Size is in bytes
	result.resize(outputBufferSize);
	auto outputBuffer = result.data(); // Calculate buffer position in case backing buffer is reallocated
	auto outBytesLeft = outputBufferSize;
	if (iconv(descriptor, &inputBuffer , &inBytesLeft, &outputBuffer, &outBytesLeft) == static_cast<size_t>(-1))
	{
		CCP_LOGERR("Failed to convert from UTF-8 to WCHAR_T: [errno: %d, inBytesLeft: %lu, outBytesLeft: %lu, outputBufferSize: %lu]", errno, inBytesLeft, outBytesLeft, outputBufferSize);
		iconv_close(descriptor);
		return result;
	}

	iconv_close(descriptor);

	// The result must be truncated to the number of character elements read into the output buffer
	result.resize(outputBufferSize - outBytesLeft);

	return result;
}
#endif
