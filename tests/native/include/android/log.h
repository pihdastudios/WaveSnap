#ifndef WAVESNAP_HOST_ANDROID_LOG_H
#define WAVESNAP_HOST_ANDROID_LOG_H

#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6

#ifdef __cplusplus
extern "C" {
#endif

int __android_log_print(int priority, const char *tag, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
