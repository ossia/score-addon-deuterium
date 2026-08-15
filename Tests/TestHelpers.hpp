#pragma once
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

// QCOMPARE-equivalent comparison for the Catch2 port: fuzzy for floating
// point (the tests were written against QCOMPARE's qFuzzyCompare semantics),
// exact for everything else.
template <typename A, typename B>
static bool approxEq(const A& a, const B& b)
{
  if constexpr(std::is_floating_point_v<A> || std::is_floating_point_v<B>)
  {
    using C = std::common_type_t<A, B>;
    const C x = static_cast<C>(a);
    const C y = static_cast<C>(b);
    if(x == y)
      return true;
    return qFuzzyCompare(x, y) || (qFuzzyIsNull(x) && qFuzzyIsNull(y));
  }
  else
  {
    return a == b;
  }
}
