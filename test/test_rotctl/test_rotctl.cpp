#include <unity.h>

#include "Rotctl.h"

using namespace rotctl;

void test_short_forms(void) {
  TEST_ASSERT_TRUE(parse("p").cmd == Cmd::GetPos);
  TEST_ASSERT_TRUE(parse("S").cmd == Cmd::Stop);
  TEST_ASSERT_TRUE(parse("q").cmd == Cmd::Quit);
  TEST_ASSERT_TRUE(parse("Q").cmd == Cmd::Quit);
  TEST_ASSERT_TRUE(parse("_").cmd == Cmd::GetInfo);
  TEST_ASSERT_TRUE(parse("K").cmd == Cmd::Park);
}

void test_set_position(void) {
  // This is exactly what Hamlib's netrotctl_set_position sends.
  const Command c = parse("P 123.400000 0.000000");
  TEST_ASSERT_TRUE(c.cmd == Cmd::SetPos);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.4f, c.az);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.el);
}

void test_dump_state_with_and_without_backslash(void) {
  TEST_ASSERT_TRUE(parse("\\dump_state").cmd == Cmd::DumpState);
  TEST_ASSERT_TRUE(parse("dump_state").cmd == Cmd::DumpState);
}

void test_long_forms(void) {
  TEST_ASSERT_TRUE(parse("\\get_pos").cmd == Cmd::GetPos);
  TEST_ASSERT_TRUE(parse("\\get_info").cmd == Cmd::GetInfo);
  TEST_ASSERT_TRUE(parse("\\stop").cmd == Cmd::Stop);

  const Command c = parse("\\set_pos 90.0 0.0");
  TEST_ASSERT_TRUE(c.cmd == Cmd::SetPos);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, c.az);
}

void test_move(void) {
  const Command c = parse("M 16 50");
  TEST_ASSERT_TRUE(c.cmd == Cmd::Move);
  TEST_ASSERT_EQUAL_INT(kMoveRight, c.direction);
  TEST_ASSERT_EQUAL_INT(50, c.speed);
}

void test_extended_prefix_is_recognised(void) {
  const Command c = parse("+\\get_pos");
  TEST_ASSERT_TRUE(c.cmd == Cmd::GetPos);
  TEST_ASSERT_TRUE(c.extended);

  TEST_ASSERT_FALSE(parse("p").extended);
}

void test_leading_whitespace_and_empty(void) {
  TEST_ASSERT_TRUE(parse("   p").cmd == Cmd::GetPos);
  TEST_ASSERT_TRUE(parse("").cmd == Cmd::Empty);
  TEST_ASSERT_TRUE(parse("   ").cmd == Cmd::Empty);
  TEST_ASSERT_TRUE(parse(nullptr).cmd == Cmd::Unsupported);
}

void test_unknown_command(void) {
  TEST_ASSERT_TRUE(parse("Z").cmd == Cmd::Unsupported);
  TEST_ASSERT_TRUE(parse("\\nonsense").cmd == Cmd::Unsupported);
}

void test_normalize_azimuth(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, normalizeAzimuth(-10.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, normalizeAzimuth(370.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.0f, normalizeAzimuth(123.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, normalizeAzimuth(360.0f));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_short_forms);
  RUN_TEST(test_set_position);
  RUN_TEST(test_dump_state_with_and_without_backslash);
  RUN_TEST(test_long_forms);
  RUN_TEST(test_move);
  RUN_TEST(test_extended_prefix_is_recognised);
  RUN_TEST(test_leading_whitespace_and_empty);
  RUN_TEST(test_unknown_command);
  RUN_TEST(test_normalize_azimuth);
  return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
