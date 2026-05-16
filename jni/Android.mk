LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := npcfight
LOCAL_SRC_FILES := main.cpp

LOCAL_CPPFLAGS  := -std=c++17 -O2 -fvisibility=hidden
LOCAL_CFLAGS    := -O2

LOCAL_LDLIBS    := -llog -ldl -lm

# Tidak link ke libGTASA.so — semua akses via offset manual
# Tidak link ke libAML.so  — akses via dlopen runtime

include $(BUILD_SHARED_LIBRARY)
