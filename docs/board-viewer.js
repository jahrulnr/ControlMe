// DOM/render controller for the 3D board viewer. Owns renderer, camera,
// OrbitControls, raycasting, UI buttons, and lifecycle — model geometry stays
// in board-model.js, data in board-components.js.
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { createBoardModel } from './board-model.js';
import { COMPONENTS, getVisibleComponents, resolveSelection } from './board-components.js';

const WIRE_LAYERS = [['5V', 'Daya 5V (ochre)'], ['GND', 'GND (abu)'], ['signal', 'Sinyal (teal)'], ['onboard', 'Onboard (tembaga)']];
const ISO_POS = [9, 10, 12], ISO_TGT = [0, 0.7, 0];

export function initBoardViewer(root) {
  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  if (!renderer.getContext()) throw new Error('WebGL context unavailable');
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));

  const state = { selected: 'cam', explode: 0, tilt: false, expander: false, showWires: true, layers: new Set(['5V', 'GND', 'signal', 'onboard']) };
  let disposed = false, contextLost = false, failed = false;
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
  camera.position.set(...ISO_POS);
  scene.add(new THREE.HemisphereLight(0xfff6e8, 0x8a7d5f, 1.1));
  const dir = new THREE.DirectionalLight(0xfff0d8, 1.4); dir.position.set(6, 12, 8); scene.add(dir);
  const model = createBoardModel();
  scene.add(model.group);
  const grid = new THREE.GridHelper(12, 24, 0xd8cdb2, 0xe8dfca); grid.position.y = -0.02; scene.add(grid);

  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  root.dataset.viewerState = 'loading';
  const wrap = el('div', 'bv-wrap');
  const stage = el('div', 'bv-stage');
  const toolbar = el('div', 'bv-toolbar');
  const canvasBox = el('div', 'bv-canvas');
  const status = el('p', 'bv-status'); status.setAttribute('role', 'status'); status.textContent = 'Menyiapkan visualisasi 3D…';
  const panel = el('aside', 'bv-panel');
  const listWrap = el('div', 'bv-list');
  const inspector = el('div', 'bv-inspector');
  canvasBox.appendChild(renderer.domElement);
  stage.append(toolbar, canvasBox, status);
  panel.append(inspector, listWrap);
  wrap.append(stage, panel);
  root.appendChild(wrap);

  // Toolbar: camera views + explode + layer + option toggles
  const views = [['Isometri', 'iso'], ['Atas', 'top'], ['Bawah', 'bottom'], ['Zoom +', 'zin'], ['Zoom −', 'zout'], ['Reset', 'reset']];
  for (const [lbl, v] of views) {
    const b = el('button', 'bv-btn', lbl); b.type = 'button'; b.dataset.view = v;
    b.addEventListener('click', () => applyView(v));
    toolbar.appendChild(b);
  }
  const explWrap = el('label', 'bv-slider');
  explWrap.append(el('span', null, 'Pisahkan komponen '));
  const expl = document.createElement('input');
  expl.type = 'range'; expl.min = 0; expl.max = 100; expl.value = 0; expl.setAttribute('aria-label', 'Tingkat pemisahan komponen');
  explWrap.appendChild(expl);
  const layersBox = el('div', 'bv-layers');
  for (const [kind, lbl] of WIRE_LAYERS) {
    const lb = el('label', 'bv-check');
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = true; cb.dataset.kind = kind;
    cb.addEventListener('change', () => { cb.checked ? state.layers.add(kind) : state.layers.delete(kind); apply(); });
    lb.append(cb, document.createTextNode(' ' + lbl)); layersBox.appendChild(lb);
  }
  const optsBox = el('div', 'bv-layers');
  const optDefs = [['tilt', 'Servo tilt (opsional)'], ['expander', 'PCA9685 (opsional)']];
  for (const [key, lbl] of optDefs) {
    const lb = el('label', 'bv-check');
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.dataset.option = key;
    cb.addEventListener('change', () => { state[key] = cb.checked; apply(); rebuildList(); setSelected(resolveSelection(state.selected, state)); fitView(); });
    lb.append(cb, document.createTextNode(' ' + lbl)); optsBox.appendChild(lb);
  }
  toolbar.append(explWrap, layersBox, optsBox);

  // Inspector + component list
  const inspTitle = el('h3', 'bv-insp-title');
  const inspType = el('p', 'bv-insp-type');
  const inspRole = el('p', 'bv-insp-role');
  const inspPins = el('ul', 'bv-insp-pins');
  const inspNotes = el('p', 'bv-insp-notes');
  const inspLink = el('p', 'bv-insp-link');
  inspLink.textContent = 'Detail pin: ';
  const a05 = el('a', null, '05-hardware-pins.md'); a05.href = '05-hardware-pins.md';
  const a06 = el('a', null, '06-board-design.md'); a06.href = '06-board-design.md';
  inspLink.append(a05, document.createTextNode(' · '), a06);
  inspector.append(inspTitle, inspType, inspRole, inspPins, inspNotes, inspLink);
  listWrap.appendChild(el('h4', 'bv-list-h', 'Pilih komponen'));
  const ul = el('ul', 'bv-components');
  listWrap.appendChild(ul);

  function rebuildList() {
    ul.textContent = '';
    for (const c of getVisibleComponents(state)) {
      const li = document.createElement('li');
      const b = el('button', 'bv-item', `${c.label} · ${c.type}`);
      b.type = 'button'; b.dataset.component = c.id;
      b.setAttribute('aria-pressed', String(c.id === state.selected));
      b.addEventListener('click', () => setSelected(c.id));
      li.appendChild(b); ul.appendChild(li);
    }
  }

  function setSelected(id) {
    state.selected = resolveSelection(id, state);
    root.dataset.selectedComponent = state.selected;
    const c = COMPONENTS.find((x) => x.id === state.selected);
    inspTitle.textContent = c.label;
    inspType.textContent = `Tipe: ${c.type} · ${c.voltage || ''}`;
    inspRole.textContent = c.role;
    inspPins.textContent = '';
    for (const p of c.pins) {
      const li = document.createElement('li');
      li.textContent = `${p.name}${p.gpio != null ? ' — GPIO' + p.gpio : ''}${p.note ? ' · ' + p.note : ''}`;
      inspPins.appendChild(li);
    }
    inspNotes.textContent = c.notes || '';
    for (const b of ul.querySelectorAll('.bv-item')) b.setAttribute('aria-pressed', String(b.dataset.component === state.selected));
    model.highlight(state.selected);
    render();
  }

  // Gesture tracking: a selection is only a left-primary pointer that never
  // travels >5px and never shares the gesture with a second pointer.
  const pointers = new Map();
  const gesture = { multi: false, travel: 0, button0: false };
  const cvs = renderer.domElement;
  cvs.addEventListener('pointerdown', (e) => {
    if (pointers.size === 0) { gesture.multi = false; gesture.travel = 0; gesture.button0 = e.button === 0; }
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size > 1) gesture.multi = true;
  });
  cvs.addEventListener('pointermove', (e) => {
    const p = pointers.get(e.pointerId);
    if (p) gesture.travel = Math.max(gesture.travel, Math.hypot(e.clientX - p.x, e.clientY - p.y));
  });
  cvs.addEventListener('pointerup', (e) => {
    const last = pointers.size === 1 && pointers.has(e.pointerId);
    pointers.delete(e.pointerId);
    if (!last || !gesture.button0 || gesture.multi || gesture.travel > 5) return;
    const r = canvasBox.getBoundingClientRect();
    const ndc = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    raycaster.setFromCamera(ndc, camera);
    const hit = raycaster.intersectObjects(model.selectableMeshes, false)
      .find((h) => h.object.userData.componentId && model.componentGroups.get(h.object.userData.componentId)?.visible);
    if (hit) setSelected(hit.object.userData.componentId);
  });
  const dropPointer = (e) => pointers.delete(e.pointerId);
  cvs.addEventListener('pointercancel', dropPointer);
  cvs.addEventListener('lostpointercapture', dropPointer);

  const raycaster = new THREE.Raycaster();
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = false;
  controls.target.set(...ISO_TGT);
  controls.minDistance = 5; controls.maxDistance = 60;
  controls.update();
  controls.addEventListener('change', render);

  // Autofit: frame the union of VISIBLE components only (no grid/hidden parts/
  // helper), preserving current orbit direction. Called on layout/extent
  // changes (init, resize, optional toggles, explode, named views) — never on
  // raw controls changes, so user zoom is not undone mid-interaction.
  function fitView() {
    const bb = new THREE.Box3();
    for (const c of getVisibleComponents(state)) bb.expandByObject(model.componentGroups.get(c.id));
    if (bb.isEmpty()) return;
    const sphere = bb.getBoundingSphere(new THREE.Sphere());
    const dir = camera.position.clone().sub(controls.target);
    if (dir.lengthSq() < 1e-6) dir.set(...ISO_POS).sub(new THREE.Vector3(...ISO_TGT));
    dir.normalize();
    const vFov = camera.fov * Math.PI / 180;
    const hFov = 2 * Math.atan(Math.tan(vFov / 2) * camera.aspect);
    const dist = Math.min(Math.max(sphere.radius / Math.sin(Math.min(vFov, hFov) / 2) * 1.1, controls.minDistance), controls.maxDistance);
    controls.target.copy(sphere.center);
    camera.position.copy(sphere.center).addScaledVector(dir, dist);
    controls.update(); render();
  }

  function applyView(v) {
    const t = controls.target;
    if (v === 'zin') { camera.position.sub(t).multiplyScalar(0.8).add(t); controls.update(); render(); return; }
    if (v === 'zout') { camera.position.sub(t).multiplyScalar(1.25).add(t); controls.update(); render(); return; }
    const dist = camera.position.distanceTo(t);
    let d;
    if (v === 'top') d = new THREE.Vector3(0, 1, 0.001);
    else if (v === 'bottom') d = new THREE.Vector3(0, -1, 0.001);
    else d = new THREE.Vector3(...ISO_POS).sub(new THREE.Vector3(...ISO_TGT));
    camera.position.copy(t).addScaledVector(d.normalize(), dist);
    fitView();
  }

  function apply() {
    model.update({ explode: state.explode, showTilt: state.tilt, showExpander: state.expander, showWires: state.showWires, visibleWireTypes: state.layers });
    render();
  }
  expl.addEventListener('input', () => { state.explode = expl.value / 100; root.dataset.explode = String(state.explode); apply(); fitView(); });
  root.dataset.explode = '0';

  function render() { if (!disposed && !contextLost) renderer.render(scene, camera); }
  const ro = new ResizeObserver(() => {
    const w = canvasBox.clientWidth || 1, h = canvasBox.clientHeight || 1;
    camera.aspect = w / h; camera.updateProjectionMatrix();
    renderer.setSize(w, h, false);
    fitView();
  });
  ro.observe(canvasBox);

  function fail(msg) {
    if (failed || disposed) return;
    failed = true;
    root.dataset.viewerState = 'error';
    status.textContent = msg + ' ';
    const btn = el('button', 'bv-btn', 'Muat ulang'); btn.type = 'button';
    btn.addEventListener('click', () => location.reload());
    status.appendChild(btn);
    document.querySelector('details.diagram2d')?.setAttribute('open', '');
  }
  const onLost = (e) => { e.preventDefault(); contextLost = true; fail('Konteks WebGL hilang — diagram 2D tetap tersedia.'); };
  cvs.addEventListener('webglcontextlost', onLost);

  // bfcache-safe: dispose only on a real unload, not a persisted pagehide.
  const onPageHide = (e) => { if (!e.persisted) teardown(); };
  const onPageShow = (e) => {
    if (!e.persisted || disposed) return;
    const w = canvasBox.clientWidth || 1, h = canvasBox.clientHeight || 1;
    renderer.setSize(w, h, false); render();
  };
  function teardown() {
    if (disposed) return;
    disposed = true;
    ro.disconnect();
    controls.dispose();
    renderer.dispose();
    model.dispose();
    window.removeEventListener('pagehide', onPageHide);
    window.removeEventListener('pageshow', onPageShow);
  }
  window.addEventListener('pagehide', onPageHide);
  window.addEventListener('pageshow', onPageShow);

  function screenPoint(id) {
    const g = model.componentGroups.get(id);
    if (!g || !g.visible) return null;
    const v = g.getWorldPosition(new THREE.Vector3()).project(camera);
    const r = canvasBox.getBoundingClientRect();
    return { x: r.left + (v.x + 1) / 2 * r.width, y: r.top + (1 - v.y) / 2 * r.height };
  }
  // Real-state snapshot for the regression script — reads live objects only.
  const snapshot = () => ({
    pos: camera.position.toArray(), tgt: controls.target.toArray(),
    aspect: camera.aspect, selected: state.selected, explode: state.explode,
    wires: model.wireCount(), wireEndpoints: model.wireEndpoints(),
    visible: getVisibleComponents(state).map((c) => c.id),
    highlightBounds: model.highlightBounds(), helperCount: model.helperCount(),
    worldPos: Object.fromEntries(getVisibleComponents(state).map((c) => [c.id, model.componentWorldPos(c.id)])),
  });

  rebuildList();
  model.update({ explode: 0, showTilt: false, showExpander: false, visibleWireTypes: state.layers });
  setSelected('cam');
  const w = canvasBox.clientWidth || 600, h = canvasBox.clientHeight || 500;
  renderer.setSize(w, h, false); camera.aspect = w / h; camera.updateProjectionMatrix();
  fitView();
  root.dataset.viewerState = 'ready';
  status.textContent = 'Seret untuk memutar · scroll/pinch untuk zoom · klik kanan geser · klik komponen untuk detail';
  return { cleanup: teardown, setSelected, screenPoint, snapshot, fitView };
}
