#include <unity.h>

#include "Gs232.h"

using namespace gs232;

static AzimuthRange range;  // defaults: 180..630, the target rotator

void test_raw_to_real_wraps_the_overlap_zone(void) {
  TEST_ASSERT_EQUAL_FLOAT(180.0f, rawToReal(180.0f));
  TEST_ASSERT_EQUAL_FLOAT(359.0f, rawToReal(359.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rawToReal(360.0f));
  TEST_ASSERT_EQUAL_FLOAT(10.0f, rawToReal(370.0f));
  TEST_ASSERT_EQUAL_FLOAT(270.0f, rawToReal(630.0f));
}

void test_target_below_starting_point_uses_the_overlap_zone(void) {
  // Real 10 deg is raw 10 (unreachable, below 180) or raw 370 (reachable).
  TEST_ASSERT_EQUAL_INT(370, chooseRawTarget(10.0f, 200.0f, range));
}

void test_target_prefers_the_shorter_travel(void) {
  // Real 200 deg is reachable as raw 200 and raw 560. From raw 210 the near
  // one wins; from raw 600 the far one does.
  TEST_ASSERT_EQUAL_INT(200, chooseRawTarget(200.0f, 210.0f, range));
  TEST_ASSERT_EQUAL_INT(560, chooseRawTarget(200.0f, 600.0f, range));
}

void test_target_at_the_range_edges(void) {
  TEST_ASSERT_EQUAL_INT(180, chooseRawTarget(180.0f, 190.0f, range));
  TEST_ASSERT_EQUAL_INT(630, chooseRawTarget(270.0f, 620.0f, range));
}

void test_unreachable_azimuth_is_rejected(void) {
  // A 360-only rotator starting at 180: real 10 has no raw representation.
  AzimuthRange narrow;
  narrow.rawMin = 180;
  narrow.rawMax = 360;
  TEST_ASSERT_EQUAL_INT(-1, chooseRawTarget(10.0f, 200.0f, narrow));
}

void test_build_goto_is_always_four_characters(void) {
  char buf[8];
  TEST_ASSERT_EQUAL_UINT(4, buildGoto(buf, sizeof(buf), 5));
  TEST_ASSERT_EQUAL_STRING("M005", buf);
  TEST_ASSERT_EQUAL_UINT(4, buildGoto(buf, sizeof(buf), 630));
  TEST_ASSERT_EQUAL_STRING("M630", buf);
}

void test_build_goto_rejects_out_of_range(void) {
  char buf[8];
  TEST_ASSERT_EQUAL_UINT(0, buildGoto(buf, sizeof(buf), -1));
  TEST_ASSERT_EQUAL_UINT(0, buildGoto(buf, sizeof(buf), 1000));
}

void test_parse_azimuth_reply(void) {
  float az = 0.0f;
  TEST_ASSERT_TRUE(parseAzimuthReply("AZ=123", az));
  TEST_ASSERT_EQUAL_FLOAT(123.0f, az);

  // C2 appends a dummy elevation on this controller.
  TEST_ASSERT_TRUE(parseAzimuthReply("AZ=007EL=000", az));
  TEST_ASSERT_EQUAL_FLOAT(7.0f, az);
}

void test_parse_azimuth_reply_rejects_junk(void) {
  float az = -1.0f;
  TEST_ASSERT_FALSE(parseAzimuthReply("?>", az));
  TEST_ASSERT_FALSE(parseAzimuthReply("AZ=12", az));
  TEST_ASSERT_FALSE(parseAzimuthReply("Speed X4", az));
  TEST_ASSERT_FALSE(parseAzimuthReply(nullptr, az));
}

void test_error_reply(void) {
  TEST_ASSERT_TRUE(isErrorReply("?>"));
  TEST_ASSERT_FALSE(isErrorReply("AZ=123"));
}

void test_classify_drives_the_transaction_timeout(void) {
  TEST_ASSERT_TRUE(classify("C") == ReplyKind::Immediate);
  TEST_ASSERT_TRUE(classify("D0500") == ReplyKind::Immediate);
  TEST_ASSERT_TRUE(classify("X4") == ReplyKind::Immediate);
  TEST_ASSERT_TRUE(classify("H") == ReplyKind::None);
  TEST_ASSERT_TRUE(classify("M370") == ReplyKind::ErrorOnly);
  TEST_ASSERT_TRUE(classify("S") == ReplyKind::ErrorOnly);
  TEST_ASSERT_TRUE(classify("l") == ReplyKind::ErrorOnly);  // case insensitive
  // Stripped commands still answer, with an error.
  TEST_ASSERT_TRUE(classify("\\?AZ") == ReplyKind::Immediate);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_raw_to_real_wraps_the_overlap_zone);
  RUN_TEST(test_target_below_starting_point_uses_the_overlap_zone);
  RUN_TEST(test_target_prefers_the_shorter_travel);
  RUN_TEST(test_target_at_the_range_edges);
  RUN_TEST(test_unreachable_azimuth_is_rejected);
  RUN_TEST(test_build_goto_is_always_four_characters);
  RUN_TEST(test_build_goto_rejects_out_of_range);
  RUN_TEST(test_parse_azimuth_reply);
  RUN_TEST(test_parse_azimuth_reply_rejects_junk);
  RUN_TEST(test_error_reply);
  RUN_TEST(test_classify_drives_the_transaction_timeout);
  return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
