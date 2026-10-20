#pragma once

// Compatibility forwarder. The history now lives in the application layer so headless commands and
// the view share one implementation. Remove this forwarder in R2 once the view includes the
// application header directly.
#include "audio_editor_application/EditorTopologyHistory.h"
