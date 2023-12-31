/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"

#if !defined(__has_feature)
    #define __has_feature(x) 0
#endif

#if __has_feature(address_sanitizer) && !defined(SK_BUILD_FOR_WIN)

extern "C" {

    const char* __lsan_default_suppressions();
    const char* __lsan_default_suppressions() {
        return "leak:libfontconfig\n"              // FontConfig looks like it leaks, but it doesn't.
               "leak:libfreetype\n"                // Unsure, appeared upgrading Debian 9->10.
               "leak:libGLX_nvidia.so\n"           // For NVidia driver.
               "leak:libnvidia-glcore.so\n"        // For NVidia driver.
               "leak:libnvidia-tls.so\n"           // For NVidia driver.
               "leak:terminator_CreateDevice\n"    // Intel Vulkan drivers.
               "leak:vkEnumeratePhysicalDevices\n" // Intel Vulkan drivers.
               ;
    }

}

#endif
