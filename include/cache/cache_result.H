//
// cache_result.H -- Typed result for cache lookups
//
// Part of cpp-cache v2
//

#ifndef CACHE_RESULT_H
#define CACHE_RESULT_H

#include <memory>

namespace CppCache {

/**
 * @brief Categorizes how a CacheResult was obtained.
 */
enum class ResultOrigin : uint8_t
{
  /** @brief Retrieved a previously cached positive value. */
  HitPositive,

  /** @brief Retrieved a previously cached failure/negative result. */
  HitNegative,

  /** @brief The miss solver was just executed and returned a positive value. */
  ComputedPositive,

  /** @brief The miss solver just failed; a negative result was cached. */
  ComputedNegative,

  /** @brief The cache is full and all entries are currently Computing. */
  Saturated
};

/**
 * @brief A typed result representing a cache lookup outcome.
 *
 * For positive results, value() provides a shared_ptr to the cached data.
 * For negative results (cacheable failures), value() returns nullptr.
 *
 * @tparam Value The type of the cached data.
 *
 * @see Cache::get_or_compute()
 * @see Cache::find()
 */
template <typename Value>
class CacheResult
{
  ResultOrigin _origin;
  std::shared_ptr<Value> _value;

public:
  /**
   * @brief Construct a CacheResult with origin and value.
   * @param origin Where the result came from.
   * @param value The value associated with the result (nullable).
   */
  CacheResult(ResultOrigin origin, std::shared_ptr<Value> value)
    : _origin(origin), _value(std::move(value))
  {
    // empty
  }

  /** @brief Factory: positive cache hit. */
  static CacheResult hit_positive(std::shared_ptr<Value> v)
  {
    return CacheResult(ResultOrigin::HitPositive, std::move(v));
  }

  /** @brief Factory: negative cache hit (cached failure). */
  static CacheResult hit_negative()
  {
    return CacheResult(ResultOrigin::HitNegative, nullptr);
  }

  /** @brief Factory: freshly computed positive result. */
  static CacheResult computed_positive(std::shared_ptr<Value> v)
  {
    return CacheResult(ResultOrigin::ComputedPositive, std::move(v));
  }

  /** @brief Factory: freshly computed negative result (solver failed). */
  static CacheResult computed_negative()
  {
    return CacheResult(ResultOrigin::ComputedNegative, nullptr);
  }

  /** @brief Factory: cache saturated (all entries Computing). */
  static CacheResult saturated()
  {
    return CacheResult(ResultOrigin::Saturated, nullptr);
  }

  /** @brief Returns true if the result contains a positive value. */
  [[nodiscard]] bool is_positive() const noexcept
  {
    return _origin == ResultOrigin::HitPositive || _origin == ResultOrigin::ComputedPositive;
  }

  /** @brief Returns true if the result represents a cached failure/negative. */
  [[nodiscard]] bool is_negative() const noexcept
  {
    return _origin == ResultOrigin::HitNegative || _origin == ResultOrigin::ComputedNegative;
  }

  /** @brief Returns true if the cache was saturated (full of computing entries). */
  [[nodiscard]] bool is_saturated() const noexcept
  {
    return _origin == ResultOrigin::Saturated;
  }

  /** @brief Returns true if the result was a cache hit (positive or negative). */
  [[nodiscard]] bool is_hit() const noexcept
  {
    return _origin == ResultOrigin::HitPositive || _origin == ResultOrigin::HitNegative;
  }

  /** @brief Returns true if the result was computed during the current call. */
  [[nodiscard]] bool was_computed() const noexcept
  {
    return _origin == ResultOrigin::ComputedPositive || _origin == ResultOrigin::ComputedNegative;
  }

  /** @brief Returns the internal origin of the result. */
  [[nodiscard]] ResultOrigin origin() const noexcept
  {
    return _origin;
  }

  /**
   * @brief Returns the cached value.
   * @return A shared_ptr to the value (non-null only if is_positive() is true).
   * @warning If multiple threads access this value concurrently and mutate it,
   * the user must ensure the Value type is thread-safe. The cache does not
   * protect against data races on the Value itself.
   */
  const std::shared_ptr<Value> &value() const noexcept
  {
    return _value;
  }

  /** @brief Convenience operator for boolean contexts; true if positive. */
  explicit operator bool() const noexcept
  {
    return is_positive();
  }
};

}  // namespace CppCache

#endif  // CACHE_RESULT_H
