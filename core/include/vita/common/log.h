#pragma once

#include <cstdio>

// Journalisation minimale : logcat sur Android, stderr ailleurs.
#if defined(__ANDROID__)
#include <android/log.h>
#define VITA_LOG_TAG "FTVita"
#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, VITA_LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) __android_log_print(ANDROID_LOG_WARN, VITA_LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, VITA_LOG_TAG, __VA_ARGS__)
#else
#define VITA_LOG_IMPL(level, ...)              \
    do {                                       \
        std::fprintf(stderr, "[%s] ", level);  \
        std::fprintf(stderr, __VA_ARGS__);     \
        std::fputc('\n', stderr);              \
    } while (0)
#define LOG_INFO(...) VITA_LOG_IMPL("INFO", __VA_ARGS__)
#define LOG_WARN(...) VITA_LOG_IMPL("WARN", __VA_ARGS__)
#define LOG_ERROR(...) VITA_LOG_IMPL("ERROR", __VA_ARGS__)
#endif
