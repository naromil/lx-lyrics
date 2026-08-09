/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#pragma once

#include <QtCore/qtenvironmentvariables.h>

// Headless test bootstrap: every suite that includes this header runs on the
// offscreen platform, so window/renderer suites work in CI/ctest without a
// display server. The inline variable's dynamic initialization is sequenced
// before main() in each translation unit that odr-uses it, and the tests
// odr-use it by taking the value into their own static bootstrap constant.
// The NOLINT below is justified: qputenv cannot realistically throw here, and
// if it did, a hard crash is the right failure mode for a test.
inline const bool kForceOffscreen = [] { // NOLINT(bugprone-throwing-static-initialization)
  qputenv("QT_QPA_PLATFORM", "offscreen");
  return true;
}();
