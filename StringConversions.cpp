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
#include "CCPMemory.h"


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

#ifndef __APPLE__

// On Apple these two live in StringConversions.mm, which CMakeLists.txt only compiles
// under if(APPLE), so every other non-Windows platform links with them missing.
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
