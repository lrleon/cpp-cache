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
  /**
   * @brief States that a cache entry can transition through during its lifecycle.
   *
   * The state machine ensures that entries are correctly synchronized between threads,
   * especially during the "miss solver" computation phase.
   */
  enum class EntryState : uint8_t
  {
    /** @brief Slot is available for a new key; no data is currently associated. */
    Empty,

    /**
     * @brief A key has been assigned and the miss solver is currently running.
     *
     * Entries in this state are protected from eviction to prevent redundant
     * computations (thundering herd prevention).
     */
    Computing,

    /** @brief A positive result has been computed and is currently valid (within TTL). */
    Ready,

    /** @brief a negative result (e.g., "not found" or error) has been cached and is valid. */
    Failed,

    /**
     * @brief The entry has been logically invalidated.
     *
     * It will not be returned as a hit and is prioritized for physical removal by the LRU.
     */
    Invalidated
  };

  /**
   * @brief Converts an EntryState enum value to its string representation.
   * @param state The state to convert.
   * @return A string representing the state name.
   */
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
