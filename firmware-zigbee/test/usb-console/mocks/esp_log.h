#pragma once
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGE test_log
#define ESP_LOGI test_log
