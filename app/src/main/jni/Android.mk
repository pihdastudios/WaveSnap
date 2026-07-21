LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := gesture_detector
LOCAL_SRC_FILES := gesture_detector.cpp third_party/picojpeg/picojpeg.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/third_party/picojpeg
LOCAL_ARM_MODE := arm
LOCAL_CFLAGS := -O3 -fomit-frame-pointer -ffunction-sections -fdata-sections -Wall
LOCAL_CPPFLAGS := -O3 -fomit-frame-pointer -std=c++98 -fno-exceptions -fno-rtti -Wall
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
