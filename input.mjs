/**
 * EMERGENT input — actions, bindings, and the devices behind them.
 *
 * The game used to ask `keys['d']` directly, in three places, and the HUD
 * listed the controls in a README. That is fine until someone rebinds a key,
 * plugs in a controller, or plays on a layout where WASD is not WASD — at which
 * point the binding table, the code that reads it and the documentation are
 * three separate places that have to be kept in step by hand.
 *
 * This module is the one place a control is named. Gameplay asks for
 * `input.down('forward')`; what that means for a keyboard, a gamepad or a touch
 * stick is decided here and nowhere else.
 *
 * Like `missions.mjs` and `culling.mjs`, it is pure: no DOM, no GL. The headless
 * harness feeds it synthetic events in the test suite, so the binding table,
 * rebinding, persistence and stick dead zones are all covered without a browser.
 */

/** Every action the game can ask about. Bindings are looked up by this id. */
export const ACTIONS = [
  'forward', 'back', 'left', 'right',
  'sprint', 'jump', 'interact',
  'toggleRenderer', 'toggleQuality', 'toggleCamera', 'toggleTelemetry',
  'save', 'load', 'newWorld', 'releasePointer'
];

/** Human-readable names, for the controls screen. */
export const ACTION_LABELS = {
  forward: 'Move forward',
  back: 'Move back',
  left: 'Strafe left',
  right: 'Strafe right',
  sprint: 'Sprint',
  jump: 'Jump',
  interact: 'Interact',
  toggleRenderer: 'Renderer mode',
  toggleQuality: 'Quality level',
  toggleCamera: 'Camera view',
  toggleTelemetry: 'Developer telemetry',
  save: 'Save',
  load: 'Load',
  newWorld: 'New world',
  releasePointer: 'Release mouse'
};

/**
 * Default keyboard bindings, as canonical `KeyboardEvent.code` values.
 *
 * `code` rather than `key`: `key` is the character the layout produces, so on an
 * AZERTY keyboard `w` is physically the key labelled `z`, and a binding table
 * written in `key` silently means something different on every layout. `code`
 * names the physical key, which is what a player rebinding expects.
 */
export const DEFAULT_KEYS = {
  forward: ['KeyW', 'ArrowUp'],
  back: ['KeyS', 'ArrowDown'],
  left: ['KeyA', 'ArrowLeft'],
  right: ['KeyD', 'ArrowRight'],
  sprint: ['ShiftLeft', 'ShiftRight'],
  jump: ['Space'],
  interact: ['KeyE'],
  toggleRenderer: ['KeyT'],
  toggleQuality: ['KeyQ'],
  toggleCamera: ['KeyV'],
  // F1, not D. EMERGENT's first release bound the telemetry overlay to D, which
  // is also "strafe right"; a player holding D to strafe therefore opened the
  // overlay underneath themselves. Two actions on one key is a binding table
  // error, not a feature.
  toggleTelemetry: ['F1'],
  save: ['F2'],
  load: ['F3'],
  newWorld: ['KeyN'],
  releasePointer: ['Escape']
};

/**
 * Default gamepad bindings, as standard-gamepad button indices.
 *
 * The layout is the W3C "standard" mapping, which is what browsers expose for
 * the overwhelming majority of pads. It is a table rather than a constant
 * because a pad whose A/B/X/Y are swapped is a real and common thing, and
 * because rebinding has to be able to write here too.
 */
export const DEFAULT_BUTTONS = {
  jump: [0],        // A / cross
  interact: [2],    // X / square
  sprint: [6, 7],   // triggers
  save: [8],        // select
  load: [9],        // start
  toggleCamera: [3] // Y / triangle
};

/** Axes on the left stick, and the sign each one drives. */
export const STICK = { axisX: 0, axisY: 1, deadZone: 0.18 };

/** Pretty-print a `KeyboardEvent.code` for the controls screen. */
export function keyLabel(code) {
  if (!code) return '—';
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  if (code.startsWith('Arrow')) return `${code.slice(5).toUpperCase()} ↑`.replace('UP ↑', '↑').replace('DOWN ↑', '↓').replace('LEFT ↑', '←').replace('RIGHT ↑', '→');
  const named = { Space: 'Space', ShiftLeft: 'L Shift', ShiftRight: 'R Shift', Escape: 'Esc', ControlLeft: 'L Ctrl', ControlRight: 'R Ctrl', AltLeft: 'L Alt', AltRight: 'R Alt', Enter: 'Enter', Tab: 'Tab' };
  return named[code] || code;
}

/** Pretty-print a standard-gamepad button index. */
export function buttonLabel(index) {
  const named = { 0: 'A / ✕', 1: 'B / ◯', 2: 'X / □', 3: 'Y / △', 4: 'LB', 5: 'RB', 6: 'LT', 7: 'RT', 8: 'Select', 9: 'Start', 10: 'L3', 11: 'R3', 12: 'D-pad ↑', 13: 'D-pad ↓', 14: 'D-pad ←', 15: 'D-pad →' };
  return named[index] || `Button ${index}`;
}

/**
 * The live input state.
 *
 * Constructed with no DOM dependency so the test suite can drive it directly.
 */
export function createInput(options = {}) {
  const state = {
    keys: Object.create(null),
    // Actions currently held, and the frame each was first pressed on. A
    // separate `pressed` set is what makes an action fire once per press
    // instead of once per frame it is held.
    down: new Set(),
    pressed: new Set(),
    released: new Set(),
    bindings: cloneBindings(DEFAULT_KEYS),
    buttons: cloneBindings(DEFAULT_BUTTONS),
    // Touch stick, already dead-zoned and clamped to the unit disc.
    stick: { x: 0, y: 0, active: false },
    sprint: false,
    mouse: { dx: 0, dy: 0 },
    pointerLocked: false,
    // Set while a rebind is waiting for a key, so the next key press is
    // captured rather than acted on, and `captured` remembers which key that
    // was so its release can be swallowed too.
    capture: null,
    captured: null,
    sensitivity: 1,
    invertY: false,
    lastDevice: 'keyboard',
    anyInputSeen: false
  };
  if (options.bindings) state.bindings = cloneBindings(options.bindings);
  return state;
}

function cloneBindings(map) {
  const out = Object.create(null);
  for (const k of Object.keys(map)) out[k] = [...map[k]];
  return out;
}

/** Actions bound to a keyboard code. */
export function actionsForCode(state, code) {
  const out = [];
  for (const action of Object.keys(state.bindings)) {
    if (state.bindings[action].includes(code)) out.push(action);
  }
  return out;
}

/** Actions bound to a gamepad button index. */
export function actionsForButton(state, index) {
  const out = [];
  for (const action of Object.keys(state.buttons)) {
    if (state.buttons[action].includes(index)) out.push(action);
  }
  return out;
}

/**
 * Feed a key press or release.
 *
 * @param {object} state
 * @param {string} code `KeyboardEvent.code`.
 * @param {boolean} isDown
 * @returns {string[]} Actions whose held-state changed.
 */
export function setKey(state, code, isDown) {
  if (!code) return [];
  // A rebind in progress swallows the next key: the key the player presses to
  // choose a binding must not also fire the action it is being bound to, and
  // must not move the player while the menu is open.
  if (state.capture && isDown) {
    state.captured = code;
    completeRebind(state, code);
    return [];
  }
  // The matching release has to be swallowed too. The press was consumed, so
  // there was never a press to release; letting the keyup through would clear
  // whatever the player happened to be holding at the time.
  if (!isDown && state.captured === code) {
    delete state.captured;
    return [];
  }
  const changed = [];
  for (const action of actionsForCode(state, code)) {
    const held = state.down.has(action);
    if (isDown && !held) { state.down.add(action); state.pressed.add(action); changed.push(action); }
    else if (!isDown && held) { state.down.delete(action); state.released.add(action); changed.push(action); }
  }
  if (isDown) { state.keys[code] = true; state.lastDevice = 'keyboard'; state.anyInputSeen = true; }
  else delete state.keys[code];
  return changed;
}

/** Feed a standard-gamepad button press or release. */
export function setButton(state, index, isDown) {
  const changed = [];
  for (const action of actionsForButton(state, index)) {
    const held = state.down.has(action);
    if (isDown && !held) { state.down.add(action); state.pressed.add(action); changed.push(action); }
    else if (!isDown && held) { state.down.delete(action); state.released.add(action); changed.push(action); }
  }
  if (isDown) { state.lastDevice = 'gamepad'; state.anyInputSeen = true; }
  return changed;
}

/**
 * Feed a raw stick reading.
 *
 * The dead zone is radial, not per-axis: a per-axis dead zone leaves the
 * diagonal corners of the dead region reachable, which is what makes a stick
 * drift diagonally when it is supposed to be still.
 */
export function setStick(state, rawX, rawY) {
  const len = Math.hypot(rawX, rawY);
  if (len < STICK.deadZone) { state.stick.x = 0; state.stick.y = 0; return; }
  // Rescale past the dead zone so the usable range still reaches 1, rather than
  // jumping from 0 to 0.18 the instant the stick moves.
  const scaled = Math.min(1, (len - STICK.deadZone) / (1 - STICK.deadZone)) / len;
  state.stick.x = rawX * scaled;
  state.stick.y = rawY * scaled;
  state.lastDevice = 'gamepad';
  state.anyInputSeen = true;
}

/** Feed the touch stick, already in the -1..1 range the DOM reports. */
export function setTouchStick(state, x, y, active = true) {
  const len = Math.hypot(x, y);
  if (!active || len < 1e-4) { state.stick.x = 0; state.stick.y = 0; state.stick.active = false; return; }
  const k = Math.min(1, len) / len;
  state.stick.x = x * k;
  state.stick.y = y * k;
  state.stick.active = true;
  state.lastDevice = 'touch';
  state.anyInputSeen = true;
}

/** Accumulate mouse movement, scaled by sensitivity and Y inversion. */
export function addMouseDelta(state, dx, dy) {
  state.mouse.dx += dx * state.sensitivity;
  state.mouse.dy += dy * state.sensitivity * (state.invertY ? -1 : 1);
}

/** Is an action held right now? */
export function isDown(state, action) {
  return state.down.has(action);
}

/** Did an action go down this frame? */
export function wasPressed(state, action) {
  return state.pressed.has(action);
}

/** Did an action come up this frame? */
export function wasReleased(state, action) {
  return state.released.has(action);
}

/**
 * Movement axes, from whichever device is being used.
 *
 * Keyboard and stick combine rather than override: holding W while nudging the
 * stick should not cancel either out, and a player switching devices mid-
 * movement should not have to release the key first.
 */
export function moveAxes(state) {
  let x = (isDown(state, 'right') ? 1 : 0) - (isDown(state, 'left') ? 1 : 0);
  let y = (isDown(state, 'forward') ? 1 : 0) - (isDown(state, 'back') ? 1 : 0);
  x += state.stick.x;
  y += state.stick.y;
  const len = Math.hypot(x, y);
  // A stick pushed to its edge and a key held should not make the player faster
  // than sprinting, so the combined vector is clamped rather than each part.
  if (len > 1) { x /= len; y /= len; }
  return { x, y, magnitude: Math.min(1, len) };
}

/** Clear the per-frame edges. Call once at the end of each frame. */
export function endFrame(state) {
  state.pressed.clear();
  state.released.clear();
  state.mouse.dx = 0;
  state.mouse.dy = 0;
}

// ---------------------------------------------------------------------------
// Rebinding
// ---------------------------------------------------------------------------

/** Begin capturing the next key press for an action. */
export function beginRebind(state, action) {
  if (!ACTIONS.includes(action)) throw new Error(`cannot rebind unknown action: ${action}`);
  state.capture = action;
  return action;
}

/** Abandon a rebind in progress. */
export function cancelRebind(state) {
  state.capture = null;
}

/** Whether a rebind is waiting for a key. */
export function isCapturing(state) {
  return state.capture !== null;
}

/**
 * Point an action at a new primary key.
 *
 * The previous primary is dropped and the *intentional* alternates are kept, so
 * rebinding `forward` off W onto I leaves the arrow keys working but rebinding
 * `interact` off E onto F actually stops E from interacting. Keeping the old
 * primary is the obvious implementation and it silently makes a rebind a no-op
 * for every action that has only one binding.
 */
function rebind(state, action, code, alternates) {
  const previousPrimary = state.bindings[action][0];
  state.bindings[action] = [code, ...alternates.filter(c => c !== code && c !== previousPrimary)];
  return action;
}

/** Assign a code to the action being captured and stop capturing. */
export function completeRebind(state, code) {
  const action = state.capture;
  state.capture = null;
  if (!action || !code) return null;
  return rebind(state, action, code, state.bindings[action]);
}

/** Restore the shipped defaults. */
export function resetBindings(state) {
  state.bindings = cloneBindings(DEFAULT_KEYS);
  state.buttons = cloneBindings(DEFAULT_BUTTONS);
  return true;
}

/**
 * Serialise bindings for the save file.
 *
 * Only the primary binding is stored: alternates are regenerated from the
 * defaults, so a save written by a build with different alternates still loads.
 */
export function serialiseBindings(state) {
  const out = {};
  for (const action of Object.keys(state.bindings)) out[action] = state.bindings[action][0];
  return out;
}

/**
 * Restore bindings from a save, ignoring anything unrecognised.
 *
 * A code is accepted on shape alone. Validating it by asking whether it is
 * already bound would be circular: the whole point of a rebind is a key that
 * is *not* currently bound, and that check silently discarded every rebinding
 * the player had ever made.
 */
export function deserialiseBindings(state, saved) {
  if (!saved || typeof saved !== 'object') return false;
  let applied = 0;
  for (const action of Object.keys(state.bindings)) {
    const code = saved[action];
    if (typeof code === 'string' && /^[A-Za-z][A-Za-z0-9]*$/.test(code)) {
      rebind(state, action, code, DEFAULT_KEYS[action]);
      applied++;
    }
  }
  return applied > 0;
}

/**
 * Every action's current keyboard and pad bindings, for a controls screen.
 * @returns {Array<{action:string, label:string, keys:string[], buttons:number[]}>}
 */
export function describeBindings(state) {
  return ACTIONS.map(action => ({
    action,
    label: ACTION_LABELS[action] || action,
    keys: state.bindings[action].map(keyLabel),
    buttons: (state.buttons[action] || []).map(buttonLabel)
  }));
}
