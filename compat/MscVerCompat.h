#pragma once

// 4J prebuilt libraries (4J_Input/4J_Render/4J_Storage) were built with MSVC v110
// (VS2012, _MSC_VER=1700). Modern MSVC emits detect_mismatch("_MSC_VER","1900")
// into every object, which makes the linker refuse to link them (LNK2038).
//
// We keep our objects in the same ABI family as the prebuilt libs:
//   * _ALLOW_MSC_VER_MISMATCH  - suppress the automatic "_MSC_VER=1900" record
//   * this pragma              - emit "_MSC_VER=1700" instead
//
// Runtime library (_ITERATOR_DEBUG_LEVEL=2, MTd) already matches the _d libs.
// This is a PC-testbed-only workaround; the console build paths are untouched.
#pragma detect_mismatch("_MSC_VER", "1700")
