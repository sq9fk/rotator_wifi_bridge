#include <unity.h>

#include "Semver.h"

using namespace semver;

void test_release_tags(void) {
  Version v = parse("v1.4.2");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_EQUAL(1, v.major);
  TEST_ASSERT_EQUAL(4, v.minor);
  TEST_ASSERT_EQUAL(2, v.patch);
  TEST_ASSERT_FALSE(isDevBuild(v));

  // No leading v, and a missing patch reads as .0.
  v = parse("2.10");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_EQUAL(2, v.major);
  TEST_ASSERT_EQUAL(10, v.minor);
  TEST_ASSERT_EQUAL(0, v.patch);
}

void test_git_describe(void) {
  Version v = parse("v1.4.0-3-gabc1234");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_EQUAL(3, v.commitsAhead);
  TEST_ASSERT_FALSE(v.dirty);
  TEST_ASSERT_TRUE(isDevBuild(v));

  v = parse("v1.4.0-3-gabc1234-dirty");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_EQUAL(3, v.commitsAhead);
  TEST_ASSERT_TRUE(v.dirty);

  v = parse("v1.4.0-dirty");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_EQUAL(0, v.commitsAhead);
  TEST_ASSERT_TRUE(v.dirty);
  TEST_ASSERT_TRUE(isDevBuild(v));
}

void test_not_a_version(void) {
  // Before the first tag, git describe --always gives a bare hash.
  TEST_ASSERT_FALSE(parse("b04c149").valid);
  TEST_ASSERT_FALSE(parse("b04c149-dirty").valid);
  TEST_ASSERT_FALSE(parse("1234567").valid);   // all-digit hash
  TEST_ASSERT_FALSE(parse("dev").valid);
  TEST_ASSERT_FALSE(parse("").valid);
  TEST_ASSERT_FALSE(parse(nullptr).valid);
  TEST_ASSERT_FALSE(parse("v1.2.3x").valid);
  TEST_ASSERT_TRUE(isDevBuild(parse("b04c149")));
}

void test_prerelease(void) {
  Version v = parse("v2.0.0-rc1");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_TRUE(v.prerelease);
  TEST_ASSERT_TRUE(isDevBuild(v));

  v = parse("v2.0.0-rc1-4-g0a1b2c3");
  TEST_ASSERT_TRUE(v.valid);
  TEST_ASSERT_TRUE(v.prerelease);
  TEST_ASSERT_EQUAL(4, v.commitsAhead);
}

void test_ordering(void) {
  TEST_ASSERT_TRUE(compare(parse("v1.4.0"), parse("v1.3.9")) > 0);
  TEST_ASSERT_TRUE(compare(parse("v1.10.0"), parse("v1.9.0")) > 0);  // numeric, not lexical
  TEST_ASSERT_TRUE(compare(parse("v2.0.0"), parse("v1.99.99")) > 0);
  TEST_ASSERT_EQUAL(0, compare(parse("v1.4"), parse("1.4.0")));

  // A local build three commits past v1.4.0 is not older than v1.4.0...
  TEST_ASSERT_TRUE(compare(parse("v1.4.0"), parse("v1.4.0-3-gabc1234")) < 0);
  // ...but v1.4.1 is newer than it.
  TEST_ASSERT_TRUE(compare(parse("v1.4.1"), parse("v1.4.0-3-gabc1234")) > 0);
  // The final release supersedes its release candidate.
  TEST_ASSERT_TRUE(compare(parse("v2.0.0"), parse("v2.0.0-rc1")) > 0);
  // Dirty does not change the order.
  TEST_ASSERT_EQUAL(0, compare(parse("v1.4.0"), parse("v1.4.0-dirty")));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_release_tags);
  RUN_TEST(test_git_describe);
  RUN_TEST(test_not_a_version);
  RUN_TEST(test_prerelease);
  RUN_TEST(test_ordering);
  return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
