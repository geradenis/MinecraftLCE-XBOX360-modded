// Compatibility shims for the prebuilt 4J libraries (built with VS2012 / v110).
//
// The libs are statically linked against the old CRT/STL but leave a couple of
// symbols unresolved, expecting the final link to provide them. Modern MSVC
// provides neither, so we supply equivalents here. This file is compiled only
// for the Windows x64 testbed builds.
//
// NOTE: this file must NOT include stdafx.h or any STL header. Modern <system_error>
// declares std::_Winerror_map(int) returning int, which would clash with the
// VS2012 signature we have to provide. <stdio.h> alone is enough.

#include <stdio.h>

// VS2012 code refers to stdin/stdout/stderr through __iob_func(), which used to
// return a pointer to a contiguous array of three FILE objects. UCRT replaced it
// with __acrt_iob_func(index); the three FILE objects are contiguous there too,
// so returning the first one keeps the old indexing working.
extern "C" FILE * __cdecl __iob_func(void)
{
    return __acrt_iob_func(0);
}

// VS2012's <system_error> implementation calls std::_Winerror_map() to turn a
// Win32 error code into a message. The modern STL no longer ships this helper
// with that signature, so provide the exact symbol the old libs expect.
namespace std
{
    const char * __cdecl _Winerror_map(int errorCode);
}

const char * __cdecl std::_Winerror_map(int errorCode)
{
    static char buffer[64];
    sprintf_s(buffer, "Win32 error %d", errorCode);
    return buffer;
}
