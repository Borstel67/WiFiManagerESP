// SPDX-License-Identifier: MIT
// AWM_Logging.h – Logging-Makros für WiFiManagerESP
//
// Konfiguration (vor dem ersten #include definieren):
//   AWM_ENABLE_LOG  – 0 = aus, 1 = an           (Standard: 1)
//   AWM_LOG_LEVEL   – 0..5  E=1,W=2,I=3,D=4,V=5 (Standard: 3 = INFO)
//   AWM_LOG_TAG     – Prefix-Tag                 (Standard: "AWM")
#pragma once
#include <Arduino.h>

#ifndef AWM_ENABLE_LOG
#  define AWM_ENABLE_LOG 1
#endif

#ifndef AWM_LOG_LEVEL
#  define AWM_LOG_LEVEL 3   // INFO – für Produktion; 4=DEBUG, 5=VERBOSE
#endif

#ifndef AWM_LOG_TAG
#  define AWM_LOG_TAG "AWM"
#endif

#define AWM_L_ERROR   1
#define AWM_L_WARN    2
#define AWM_L_INFO    3
#define AWM_L_DEBUG   4
#define AWM_L_VERBOSE 5

#if AWM_ENABLE_LOG
  #define AWM__PRINTF(_lvl, fmt, ...) \
    do { Serial.printf("[" AWM_LOG_TAG "] " _lvl ": " fmt "\n", ##__VA_ARGS__); } while(0)

  #if (AWM_LOG_LEVEL >= AWM_L_ERROR)
    #define AWM_LOGE(fmt, ...) AWM__PRINTF("E", fmt, ##__VA_ARGS__)
  #else
    #define AWM_LOGE(...) do{}while(0)
  #endif
  #if (AWM_LOG_LEVEL >= AWM_L_WARN)
    #define AWM_LOGW(fmt, ...) AWM__PRINTF("W", fmt, ##__VA_ARGS__)
  #else
    #define AWM_LOGW(...) do{}while(0)
  #endif
  #if (AWM_LOG_LEVEL >= AWM_L_INFO)
    #define AWM_LOGI(fmt, ...) AWM__PRINTF("I", fmt, ##__VA_ARGS__)
  #else
    #define AWM_LOGI(...) do{}while(0)
  #endif
  #if (AWM_LOG_LEVEL >= AWM_L_DEBUG)
    #define AWM_LOGD(fmt, ...) AWM__PRINTF("D", fmt, ##__VA_ARGS__)
  #else
    #define AWM_LOGD(...) do{}while(0)
  #endif
  #if (AWM_LOG_LEVEL >= AWM_L_VERBOSE)
    #define AWM_LOGV(fmt, ...) AWM__PRINTF("V", fmt, ##__VA_ARGS__)
  #else
    #define AWM_LOGV(...) do{}while(0)
  #endif
#else
  #define AWM_LOGE(...) do{}while(0)
  #define AWM_LOGW(...) do{}while(0)
  #define AWM_LOGI(...) do{}while(0)
  #define AWM_LOGD(...) do{}while(0)
  #define AWM_LOGV(...) do{}while(0)
#endif
