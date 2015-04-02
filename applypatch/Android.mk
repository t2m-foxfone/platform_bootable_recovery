# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)
#Begin[wjb, Modify for foxfone recovery, 20150325]
include $(CLEAR_VARS)
LOCAL_MODULE        := libapplypatch
LOCAL_MODULE_TAGS   := eng
LOCAL_MODULE_CLASS  := STATIC_LIBRARIES
LOCAL_MODULE_SUFFIX := .a
LOCAL_SRC_FILES     := libapplypatch.a
include $(BUILD_PREBUILT)
#End[wjb, 20150325]

include $(CLEAR_VARS)

LOCAL_SRC_FILES := main.c
LOCAL_MODULE := applypatch
LOCAL_C_INCLUDES += bootable/recovery
LOCAL_STATIC_LIBRARIES += libapplypatch libmtdutils libmincrypt libbz libminelf
LOCAL_SHARED_LIBRARIES += libz libcutils libstdc++ libc
# [FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin
ifeq ($(TARGET_USES_TCT_FOTA), true)
LOCAL_CFLAGS += -DFEATURE_TCT_FULL_UPDATE
endif
# [FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := main.c
LOCAL_MODULE := applypatch_static
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_TAGS := eng
LOCAL_C_INCLUDES += bootable/recovery
LOCAL_STATIC_LIBRARIES += libapplypatch libmtdutils libmincrypt libbz libminelf
LOCAL_STATIC_LIBRARIES += libz libcutils libstdc++ libc

# [FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin
ifeq ($(TARGET_USES_TCT_FOTA), true)
LOCAL_CFLAGS += -DFEATURE_TCT_FULL_UPDATE
endif
# [FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := imgdiff.c utils.c bsdiff.c
LOCAL_MODULE := imgdiff
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_C_INCLUDES += external/zlib external/bzip2
LOCAL_STATIC_LIBRARIES += libz libbz

include $(BUILD_HOST_EXECUTABLE)
