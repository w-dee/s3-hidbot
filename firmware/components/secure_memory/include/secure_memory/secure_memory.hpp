#pragma once

#include <cstddef>

namespace secure_memory {

// Compiler-resistant wiping for project-owned secret-bearing storage.
void zero(void *storage, std::size_t length);

// Wipe the complete allocation returned by the platform allocator. This is
// used for parser-owned strings whose decoded length can contain an embedded
// NUL and therefore cannot be recovered with strlen().
void zero_allocation(void *storage);

}  // namespace secure_memory
