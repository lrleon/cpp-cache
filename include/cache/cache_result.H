//
// cache_result.H -- Typed result for cache lookups
//
// Part of cpp-cache v2
//

# ifndef CACHE_RESULT_H
# define CACHE_RESULT_H

# include <memory>

namespace CppCache
{
  // Describes how the result was obtained
  enum class ResultOrigin : uint8_t
  {
    HitPositive,       // Positive value retrieved from cache
    HitNegative,       // Negative (failed) result retrieved from cache
    ComputedPositive,  // Value just computed by miss solver and cached
    ComputedNegative   // Miss solver just failed; negative result cached
  };

  // Typed result returned by Cache::get_or_compute() and Cache::find()
  //
  // For positive results, value() returns a non-null shared_ptr<Value>.
  // For negative results, value() returns nullptr.
  //
  // The user can inspect origin() to distinguish cache hits from fresh
  // computations, and positive from negative outcomes.
  template <typename Value>
  class CacheResult
  {
    ResultOrigin _origin;
    std::shared_ptr<Value> _value;

   public:

    CacheResult(ResultOrigin origin, std::shared_ptr<Value> value)
      : _origin(origin), _value(std::move(value))
    {
      // empty
    }

    // Convenience factories
    static CacheResult hit_positive(std::shared_ptr<Value> v)
    {
      return CacheResult(ResultOrigin::HitPositive, std::move(v));
    }

    static CacheResult hit_negative()
    {
      return CacheResult(ResultOrigin::HitNegative, nullptr);
    }

    static CacheResult computed_positive(std::shared_ptr<Value> v)
    {
      return CacheResult(ResultOrigin::ComputedPositive, std::move(v));
    }

    static CacheResult computed_negative()
    {
      return CacheResult(ResultOrigin::ComputedNegative, nullptr);
    }

    ResultOrigin origin() const noexcept { return _origin; }

    bool is_positive() const noexcept
    {
      return _origin == ResultOrigin::HitPositive ||
             _origin == ResultOrigin::ComputedPositive;
    }

    bool is_negative() const noexcept
    {
      return _origin == ResultOrigin::HitNegative ||
             _origin == ResultOrigin::ComputedNegative;
    }

    bool is_hit() const noexcept
    {
      return _origin == ResultOrigin::HitPositive ||
             _origin == ResultOrigin::HitNegative;
    }

    bool was_computed() const noexcept
    {
      return _origin == ResultOrigin::ComputedPositive ||
             _origin == ResultOrigin::ComputedNegative;
    }

    // Returns the cached value. Non-null only for positive results.
    const std::shared_ptr<Value> & value() const noexcept { return _value; }

    // Convenience: true if positive (value is available)
    explicit operator bool() const noexcept { return is_positive(); }
  };

} // namespace CppCache

# endif // CACHE_RESULT_H
