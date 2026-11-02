#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool cat_proto_parse_iq_value(const char *frame, bool *iq_on);
bool cat_proto_parse_mode(const char *frame, char *out, size_t out_len);
uint8_t cat_proto_parse_mode_code(const char *mode);
bool cat_proto_parse_filter(const char *frame, char *out, size_t out_len, uint32_t *hz);
bool cat_proto_parse_if(const char *frame, uint8_t *mode_code, int32_t *rit_hz, bool *rit_on);
/* "VN1_04_010QMX;" -> "1_04_010". Rejects anything that is not digits/_/. */
bool cat_proto_parse_version(const char *frame, char *out, size_t out_len);
