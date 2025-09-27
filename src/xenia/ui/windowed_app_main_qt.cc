/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <QApplication>
#include <cstdio>
#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_qt.h"

int main(int argc, char** argv) {
  // Force X11 backend (Vulkan needs XCB, not Wayland)
  qputenv("QT_QPA_PLATFORM", "xcb");

  QApplication qt_app(argc, argv);

  int result;

  {
    xe::ui::QtWindowedAppContext app_context(&qt_app);

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    cvar::ParseLaunchArguments(argc, argv, app->GetPositionalOptionsUsage(),
                               app->GetPositionalOptions());

    xe::InitializeLogging(app->GetName());

    if (app->OnInitialize()) {
      app_context.RunMainQtLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  xe::ShutdownLogging();

  return result;
}
