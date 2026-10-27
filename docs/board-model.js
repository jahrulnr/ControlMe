// Procedural THREE.js model for the carrier board — illustrative geometry only,
// not a CAD model and not dimensional claims. Owns meshes/geometry; the viewer
// owns DOM/camera/renderer.
import * as THREE from 'three';
import { COMPONENTS, WIRES } from './board-components.js';

const COLORS = {
  carrier: 0xf3ead8, silk: 0xd9cfae, hole: 0x2c2c2c, brass: 0xc9a227,
  pcbBlack: 0x1d1d22, pcbGreen: 0x1e5c2e, gold: 0xd4af37, shield: 0x8f959c,
  ledWhite: 0xf5f5f0, lens: 0x0a0a0c, glass: 0x23303a, copper: 0xb87333,
  metal: 0xb8bcc2, servo: 0x2456a8, horn: 0xf2f2f2, terminal: 0x2e7d3f,
  capBlack: 0x16161a, capTop: 0xc8ccd0, fuse: 0x3a3a3f, card: 0x9fb4c0,
  headerStrip: 0x14141a,
  wire5V: 0xb97d10, wireGND: 0x7d7d7d, wireSignal: 0x1f6f6b, wireOnboard: 0xb87333,
};
const WIRE_COLOR = { '5V': COLORS.wire5V, GND: COLORS.wireGND, signal: COLORS.wireSignal, onboard: COLORS.wireOnboard };

const mat = (color, opts = {}) =>
  new THREE.MeshStandardMaterial({ color, roughness: 0.65, metalness: 0.15, ...opts });
const box = (w, h, d, color, opts) => new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat(color, opts));
const cyl = (r, h, color, opts) => new THREE.Mesh(new THREE.CylinderGeometry(r, r, h, 20), mat(color, opts));
const place = (m, x, y, z) => { m.position.set(x, y, z); return m; };
const tag = (g, id) => { g.userData.componentId = id; g.traverse((o) => { o.userData.componentId = id; }); };

// Silkscreen label baked to a canvas texture — no downloaded assets.
function boardMark(text, w, h, track) {
  const cv = document.createElement('canvas');
  cv.width = 256; cv.height = 48;
  const ctx = cv.getContext('2d');
  ctx.fillStyle = '#d8cfae'; ctx.font = '30px monospace';
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillText(text, 128, 24);
  const tex = new THREE.CanvasTexture(cv);
  track(tex);
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ map: tex, transparent: true }));
  mesh.rotation.x = -Math.PI / 2;
  return mesh;
}

const BUILDERS = {
  carrier(g) {
    g.add(box(8, 0.12, 5, COLORS.carrier));
    for (const [x, z] of [[-3.6, -2.2], [3.6, -2.2], [-3.6, 2.2], [3.6, 2.2]]) {
      g.add(place(cyl(0.14, 0.16, COLORS.brass, { metalness: 0.7, roughness: 0.3 }), x, 0.06, z));
      g.add(place(cyl(0.07, 0.2, COLORS.hole), x, 0.06, z));
    }
    g.add(place(box(5.4, 0.02, 0.12, COLORS.silk), 0, 0.08, 1.55));
    g.add(place(box(0.12, 0.02, 3.2, COLORS.silk), 3.1, 0.08, 0));
  },
  cam(g, track) {
    g.add(box(2.4, 0.1, 3.4, COLORS.pcbBlack));
    g.add(place(boardMark('ESP32-CAM', 1.5, 0.28, track), -0.2, 0.06, 0.2));
    g.add(place(box(1.1, 0.16, 0.9, COLORS.shield, { metalness: 0.7, roughness: 0.4 }), 0.3, 0.14, 0.7));
    g.add(place(box(0.35, 0.12, 0.35, COLORS.ledWhite, { emissive: 0x555548 }), 0.7, 0.12, -1.1)); // flash LED GPIO4
    g.add(place(box(2.2, 0.05, 0.5, 0x2a2a30), 0, 0.1, -1.55)); // antenna edge zone
    g.add(place(box(0.7, 0.08, 0.3, COLORS.ledWhite), -0.6, 0.12, -0.7)); // FFC socket
  },
  headers(g) {
    for (const x of [-1.28, 1.28]) {
      g.add(place(box(0.3, 0.22, 3.2, COLORS.headerStrip), x, -0.1, 0));
      for (let i = 0; i < 8; i++) {
        g.add(place(cyl(0.05, 0.34, COLORS.gold, { metalness: 0.85, roughness: 0.3 }), x, -0.18, -1.4 + i * 0.4));
      }
    }
  },
  camera(g) {
    g.add(box(1.1, 0.08, 0.9, 0x11161c));
    g.add(place(cyl(0.22, 0.3, COLORS.lens), 0, 0.2, 0)); // barrel stands vertical (+Y)
    g.add(place(cyl(0.16, 0.02, COLORS.glass, { roughness: 0.15, metalness: 0.4 }), 0, 0.37, 0)); // glossy lens face on top
    g.add(place(box(0.5, 0.015, 0.5, COLORS.copper, { metalness: 0.5 }), 0, -0.03, 0.55)); // FFC ribbon toward module
  },
  sd(g) {
    g.add(box(1.3, 0.22, 1.1, COLORS.metal, { metalness: 0.8, roughness: 0.35 }));
    g.add(place(box(0.8, 0.1, 0.35, COLORS.card), 0, 0.12, 0.55)); // card tongue out
  },
  programmer(g) {
    g.add(box(2.65, 0.1, 3.65, 0x24242a));
    g.add(place(box(0.5, 0.2, 0.35, COLORS.metal, { metalness: 0.8, roughness: 0.3 }), 0, 0.15, -1.7)); // microUSB
    g.add(place(box(0.55, 0.08, 0.45, 0x0d0d10), -0.7, 0.09, -1.0)); // serial bridge chip
    for (const x of [-0.9, 0.9]) g.add(place(cyl(0.12, 0.12, 0x303038), x, 0.12, 1.3)); // RST/IO0 buttons
  },
  'servo-pan': servoBody,
  'servo-tilt': servoBody,
  'power-input'(g) {
    g.add(box(0.9, 0.5, 0.7, COLORS.terminal));
    for (const z of [-0.18, 0.18]) g.add(place(cyl(0.09, 0.15, COLORS.metal, { metalness: 0.8 }), 0, 0.32, z));
  },
  fuse(g) {
    g.add(box(0.7, 0.45, 0.5, COLORS.fuse));
    const body = cyl(0.1, 0.72, 0xa83232); body.rotation.z = Math.PI / 2;
    g.add(place(body, 0, 0.32, 0));
  },
  'bulk-cap'(g) {
    g.add(place(cyl(0.26, 0.85, COLORS.capBlack), 0, 0.15, 0));
    g.add(place(cyl(0.24, 0.05, COLORS.capTop, { metalness: 0.7 }), 0, 0.58, 0));
    for (const x of [-0.1, 0.1]) g.add(place(cyl(0.03, 0.3, COLORS.metal, { metalness: 0.8 }), x, -0.4, 0)); // cap legs
  },
  pca(g) {
    g.add(box(1.6, 0.08, 1.1, COLORS.pcbGreen));
    for (let i = 0; i < 8; i++) g.add(place(cyl(0.04, 0.28, COLORS.gold, { metalness: 0.8 }), -0.7 + i * 0.2, 0.16, -0.4));
    g.add(place(box(0.4, 0.3, 0.5, COLORS.terminal), 0.5, 0.2, 0.35));
  },
};

function servoBody(g) {
  g.add(box(1.1, 0.7, 0.75, COLORS.servo));
  g.add(place(box(1.3, 0.08, 0.75, COLORS.servo), 0, 0.3, 0));
  g.add(place(cyl(0.12, 0.25, COLORS.horn), 0, 0.48, 0));
  g.add(place(cyl(0.3, 0.05, COLORS.horn), 0, 0.62, 0));
}

export function createBoardModel() {
  const group = new THREE.Group();
  const componentGroups = new Map();
  const selectableMeshes = [];
  const wiresGroup = new THREE.Group();
  const disposables = [];
  let explodeT = 0;
  let helper = null;
  let highlightedId = null;
  const vis = { tilt: false, expander: false };

  for (const c of COMPONENTS) {
    const b = BUILDERS[c.id];
    if (!b) throw new Error(`board-model: no builder for component "${c.id}"`);
    const cg = new THREE.Group();
    cg.position.fromArray(c.position);
    b(cg, (d) => disposables.push(d));
    tag(cg, c.id);
    cg.visible = !c.optional;
    componentGroups.set(c.id, cg);
    cg.traverse((o) => { if (o.isMesh) { selectableMeshes.push(o); disposables.push(o.geometry, o.material); } });
    group.add(cg);
  }
  group.add(wiresGroup);

  const wireMeshes = new Map(); // id -> {mesh, a, b} so tests can read real endpoints
  function rebuildWires(visibleKinds, showWires) {
    for (const [, e] of wireMeshes) { wiresGroup.remove(e.mesh); e.mesh.geometry.dispose(); e.mesh.material.dispose(); }
    wireMeshes.clear();
    if (!showWires) return;
    for (const w of WIRES) {
      if (!visibleKinds.has(w.kind)) continue;
      const fc = componentGroups.get(w.from.c), tc = componentGroups.get(w.to.c);
      if (!fc.visible || !tc.visible) continue;
      if (w.requires && !componentGroups.get(w.requires).visible) continue;
      const a = fc.localToWorld(new THREE.Vector3(...w.from.p));
      const b = tc.localToWorld(new THREE.Vector3(...w.to.p));
      const mid = a.clone().lerp(b, 0.5); mid.y += Math.max(0.15, a.distanceTo(b) * 0.12);
      const curve = new THREE.CatmullRomCurve3([a, mid, b]);
      const mesh = new THREE.Mesh(new THREE.TubeGeometry(curve, 12, 0.035, 6, false),
        mat(WIRE_COLOR[w.kind] || 0x888888));
      mesh.userData.wire = true;
      wiresGroup.add(mesh);
      wireMeshes.set(w.id, { mesh, a: a.toArray(), b: b.toArray() });
    }
  }

  function refreshHelper() {
    if (!helper || !highlightedId) return;
    const g = componentGroups.get(highlightedId);
    if (g && g.visible) { helper.visible = true; helper.setFromObject(g); }
    else helper.visible = false;
  }

  return {
    group,
    selectableMeshes,
    componentGroups,
    wireCount: () => wireMeshes.size,
    wireEndpoints: () => [...wireMeshes.entries()].map(([id, e]) => ({ id, a: e.a, b: e.b })),
    componentWorldPos: (id) => componentGroups.get(id)?.getWorldPosition(new THREE.Vector3()).toArray() ?? null,
    highlightBounds: () => {
      if (!helper || !helper.visible || !highlightedId) return null;
      const b = new THREE.Box3().setFromObject(componentGroups.get(highlightedId));
      return { min: b.min.toArray(), max: b.max.toArray() };
    },
    helperCount: () => (helper ? 1 : 0),
    update(o = {}) {
      if (o.showTilt !== undefined) vis.tilt = o.showTilt;
      if (o.showExpander !== undefined) vis.expander = o.showExpander;
      for (const c of COMPONENTS) componentGroups.get(c.id).visible = !c.optional || !!vis[c.optionKey];
      if (o.explode !== undefined) explodeT = o.explode;
      for (const c of COMPONENTS) {
        const g = componentGroups.get(c.id);
        g.position.set(c.position[0] + c.explodeOffset[0] * explodeT,
                       c.position[1] + c.explodeOffset[1] * explodeT,
                       c.position[2] + c.explodeOffset[2] * explodeT);
      }
      group.updateMatrixWorld(true);
      rebuildWires(o.visibleWireTypes ?? new Set(['5V', 'GND', 'signal', 'onboard']), o.showWires !== false);
      group.updateMatrixWorld(true);
      refreshHelper(); // outline follows exploded transforms
    },
    highlight(id) {
      highlightedId = id;
      const g = componentGroups.get(id);
      if (!helper) { helper = new THREE.BoxHelper(g, 0xd2604c); group.add(helper); }
      refreshHelper();
    },
    dispose() {
      for (const [, e] of wireMeshes) { e.mesh.geometry.dispose(); e.mesh.material.dispose(); }
      wireMeshes.clear();
      if (helper) { helper.geometry.dispose(); helper.material.dispose(); helper = null; }
      for (const d of disposables) d.dispose();
    },
  };
}
