"""Read the production editor routes after the view components were separated."""
from pathlib import Path

def editor_route_source(root: Path) -> str:
    folder = root / 'apps/editor'
    main = (folder/'Main.cpp').read_text(encoding='utf-8')
    workspace = (folder/'EditorWorkspace.h').read_text(encoding='utf-8')
    assert '#include "EditorWorkspace.h"' in main
    assert '#include "EditorGraphCanvas.h"' in workspace
    return (folder/'EditorGraphCanvas.h').read_text(encoding='utf-8') + '\n' + workspace + '\n' + main
