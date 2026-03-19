//
// entry_state.H -- States for cache entries
//
// Part of cpp-cache v2
//

# ifndef CACHE_ENTRY_STATE_H
# define CACHE_ENTRY_STATE_H

# include <cstdint>
# include <string>

namespace CppCache
{
  // States that a cache entry can have
  enum class EntryState : uint8_t
  {
    Empty,        // Slot available, no active key
    Computing,    // Key assigned, miss solver running; not evictable
    Ready,        // Positive result cached and valid
    Failed,       // Negative result cached and valid
    Invalidated   // Logically invalidated; awaiting physical removal by LRU
  };

  inline std::string entry_state_to_string(EntryState state)
  {
    switch (state)
      {
        case EntryState::Empty:       return "Empty";
        case EntryState::Computing:   return "Computing";
        case EntryState::Ready:       return "Ready";
        case EntryState::Failed:      return "Failed";
        case EntryState::Invalidated: return "Invalidated";
        default:                      return "Unknown";
      }
  }

} // namespace CppCache

# endif // CACHE_ENTRY_STATE_H
