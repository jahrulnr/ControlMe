---
name: controlme-board-viewer
description: Update ControlMe hardware documentation and its three.js viewer. Use for docs/board-design.html, board-components.js, board-model.js, board-viewer.js, board-viewer.css, pin diagrams, component selection, wire layers, exploded views, or desktop/mobile and WebGL issues.
---

# ControlMe Board Viewer

## Choose The Right Layer

- Read `docs/05-hardware-pins.md` and active driver sources for pin or device
  behavior claims. The viewer is an illustrative draft, not manufacturing CAD
  or proof that a particular board has been qualified.
- Change component data, inspector text, pins, and wire endpoints in
  `docs/board-components.js`; keep this module pure, without DOM/THREE.
- Change geometry/materials and model connections in `docs/board-model.js`.
  Preserve component IDs and the model's ownership of GPU resources.
- Change DOM, renderer/camera, OrbitControls, interaction state, selection, and
  lifecycles in `docs/board-viewer.js`; styles live in `docs/board-viewer.css`.
- Change the page shell, 2D diagrams, and import map in `docs/board-design.html`.
  Preserve the existing paper/grid visual language and typography.

## Preserve Visual And Hardware Contracts

- Keep `COMPONENTS`, `WIRES`, `getVisibleComponents()`, and `resolveSelection()`
  as shared data sources. Do not duplicate component tables in the viewer or
  model. When an optional component is hidden, selection must fall back to a
  visible component.
- Keep list selection and 3D picking consistent with the inspector, layer
  toggles, optional components, view presets, and exploded view.
- Update wire endpoints alongside component positions when the explode value
  changes. Connections are logical relationships, not PCB routing; dimensions
  use illustrative units.
- Preserve on-demand rendering, resize/fit behavior, and GPU resource disposal.
  Do not replace this with an animation loop without a concrete interaction need.
- Preserve the 2D fallback for WebGL/CDN failures and the context-loss and
  pagehide/pageshow lifecycles, including bfcache.
- Preserve conditional GPIO13/SD DAT3 sharing, GPIO12 strapping for tilt,
  programmer UART use, and the optional expander. Do not turn illustrations into
  universal compatibility claims or add hardware guarantees.
- Some historical text uses the Partner name and links to unavailable documents.
  Do not expand a task into historical rebranding or filling in those documents
  unless requested. Do not copy missing links into new documentation.

## Verify

1. Run from the repository root:

   ```bash
   node --check docs/board-components.js
   node --check docs/board-model.js
   node --check docs/board-viewer.js
   python3 -m http.server 8000 --bind 127.0.0.1 --directory docs
   ```

2. Open `http://127.0.0.1:8000/board-design.html`. `docs/package.json` only marks
   ESM; there is no npm build/test script or local dependency. three.js uses a
   browser CDN import map. Do not add npm scaffolding just to run the viewer.
   Syntax checks do not prove that CDN imports or rendering work.
3. Inspect desktop and mobile screenshots. Test scrolling/overflow, resize,
   selection from the list and model, orbit/pan/zoom, layer and optional-component
   toggles, exploded view, the inspector, and the 2D fallback. Check the console
   for errors.
4. If lifecycle behavior changed, test back navigation and WebGL/context-loss
   failures. If pure data changed, add focused Node tests for affected functions
   where needed; do not claim a JavaScript test suite already exists.
5. Report viewports, scenarios, visual results, and verification limits. Do not
   equate an illustration with electrical validation on the device.
