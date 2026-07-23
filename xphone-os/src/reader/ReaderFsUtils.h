#pragma once
#include <string>

// The two FsHelpers routines the pruned engine needs (x4-os lib/FsHelpers is
// not vendored into xphone-os). Implementations copied from FsHelpers.cpp.
namespace reader {
namespace fsutil {

// Decode %XX URI escapes in an EPUB-internal href.
std::string decodeUriEscapes(const std::string& path);

// Collapse "a/b/../c" style paths and strip leading slashes/empty components.
std::string normalisePath(const std::string& path);

}  // namespace fsutil
}  // namespace reader
