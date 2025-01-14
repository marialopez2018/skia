/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrDawnUtil_DEFINED
#define GrDawnUtil_DEFINED

#include "include/private/GrTypesPriv.h"
#include "dawn/dawncpp.h"

GrPixelConfig GrDawnFormatToPixelConfig(dawn::TextureFormat format);
bool GrDawnFormatIsRenderable(dawn::TextureFormat format);
bool GrPixelConfigToDawnFormat(GrPixelConfig config, dawn::TextureFormat* format);
#if GR_TEST_UTILS
const char* GrDawnFormatToStr(dawn::TextureFormat format);
#endif

#endif // GrDawnUtil_DEFINED
