#include "test.h"

#include "forest_dot.h"
#include "ooze/forest.h"

#include <fstream>
#include <string>

namespace ooze {

namespace {

//     a        h
//    / \      / \
//   b   f    i   m
//  / \  |   /|\
// c   e g  j k l
// |
// d
Forest<std::string> make_test_forest() {
  Forest<std::string> f;
  f.merge_path(std::array{"a", "b", "c", "d"});
  f.merge_path(std::array{"a", "b", "e"});
  f.merge_path(std::array{"a", "f", "g"});
  f.merge_path(std::array{"h", "i", "j"});
  f.merge_path(std::array{"h", "i", "k"});
  f.merge_path(std::array{"h", "i", "l"});
  f.merge_path(std::array{"h", "m"});
  return f;
}

} // namespace

BOOST_AUTO_TEST_SUITE(forest)

BOOST_AUTO_TEST_CASE(roots) {
  const auto f = make_test_forest();

  BOOST_CHECK(Forest<int>().roots().empty());
  BOOST_CHECK_RANGE_EQUAL((std::array{"a", "h"}), f.roots());
}

BOOST_AUTO_TEST_CASE(children) {
  const auto f = make_test_forest();

  BOOST_CHECK_EQUAL(13, f.size());

  BOOST_CHECK_RANGE_EQUAL((std::array{"b", "f"}), f.children(0));
  BOOST_CHECK_RANGE_EQUAL((std::array{"c", "e"}), f.children(1));
  BOOST_CHECK_RANGE_EQUAL((std::array{"d"}), f.children(2));
  BOOST_CHECK(f.children(3).empty());
  BOOST_CHECK(f.children(4).empty());
  BOOST_CHECK_RANGE_EQUAL((std::array{"g"}), f.children(5));
  BOOST_CHECK(f.children(6).empty());
  BOOST_CHECK_RANGE_EQUAL((std::array{"i", "m"}), f.children(7));
  BOOST_CHECK_RANGE_EQUAL((std::array{"j", "k", "l"}), f.children(8));
  BOOST_CHECK(f.children(9).empty());
  BOOST_CHECK(f.children(10).empty());
  BOOST_CHECK(f.children(11).empty());
  BOOST_CHECK(f.children(12).empty());
}

BOOST_AUTO_TEST_CASE(pre_order) {
  const auto f = make_test_forest();

  BOOST_CHECK(Forest<int>().pre_order().empty());

  const std::array exp = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"};
  BOOST_CHECK_RANGE_EQUAL(exp, f.pre_order());

  const std::array exp_a = {"a", "b", "c", "d", "e", "f", "g"};
  BOOST_CHECK_RANGE_EQUAL(exp_a, f.pre_order(0));

  const std::array exp_b = {"b", "c", "d", "e"};
  BOOST_CHECK_RANGE_EQUAL(exp_b, f.pre_order(1));

  BOOST_CHECK_RANGE_EQUAL((std::array{"c", "d"}), f.pre_order(2));
  BOOST_CHECK_RANGE_EQUAL((std::array{"d"}), f.pre_order(3));
}

BOOST_AUTO_TEST_CASE(post_order) {
  const auto f = make_test_forest();

  BOOST_CHECK(Forest<int>().post_order().empty());

  const std::array exp = {"d", "c", "e", "b", "g", "f", "a", "j", "k", "l", "i", "m", "h"};
  BOOST_CHECK_RANGE_EQUAL(exp, f.post_order());

  const std::array exp_a = {"d", "c", "e", "b", "g", "f", "a"};
  BOOST_CHECK_RANGE_EQUAL(exp_a, f.post_order(0));

  const std::array exp_b = {"d", "c", "e", "b"};
  BOOST_CHECK_RANGE_EQUAL(exp_b, f.post_order(1));

  BOOST_CHECK_RANGE_EQUAL((std::array{"d", "c"}), f.post_order(2));
  BOOST_CHECK_RANGE_EQUAL((std::array{"d"}), f.post_order(3));
}

BOOST_AUTO_TEST_CASE(dot, *boost::unit_test::disabled()) {
  std::ofstream file("output.dot");
  generate_dot(make_test_forest(), file, [](const std::string& s, int) { return NodeDotOptions{s}; });
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ooze
