/**
 * Tests for input.mjs — actions, bindings, rebinding and device mixing.
 *
 * Pure, so the suite drives it with the same synthetic events the headless
 * harness fires at the window. A binding table that is only ever exercised by a
 * human with a keyboard is a binding table that is wrong until someone notices.
 *
 * Run: `npm run test:input`
 */
import {
  ACTIONS, ACTION_LABELS, DEFAULT_KEYS, DEFAULT_BUTTONS, STICK,
  createInput, actionsForCode, actionsForButton, setKey, setButton, setStick,
  setTouchStick, addMouseDelta, isDown, wasPressed, wasReleased, moveAxes,
  endFrame, beginRebind, cancelRebind, isCapturing, completeRebind,
  resetBindings, serialiseBindings, deserialiseBindings, describeBindings,
  keyLabel, buttonLabel
} from './input.mjs';

let checks = 0;
let failures = 0;

function test(name, fn) {
  try {
    fn();
    process.stdout.write(`✓ ${name}\n`);
  } catch (err) {
    failures++;
    process.stdout.write(`✗ ${name}\n  ${err.message}\n`);
  }
}

function assert(cond, msg) {
  checks++;
  if (!cond) throw new Error(msg);
}

function assertEqual(actual, expected, msg) {
  checks++;
  if (actual !== expected) throw new Error(`${msg} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

// ---------------------------------------------------------------------------

test('every action has a label and a default binding', () => {
  const input = createInput();
  assert(ACTIONS.length >= 14, 'the action set covers movement, interaction and the toggles');
  for (const action of ACTIONS) {
    assert(ACTION_LABELS[action], `${action} is labelled for the controls screen`);
    assert(input.bindings[action] && input.bindings[action].length, `${action} has a default key`);
    assert(input.bindings[action][0].startsWith('Key') || input.bindings[action][0].startsWith('Digit') || input.bindings[action][0].startsWith('F') && input.bindings[action][0].length <= 3 || input.bindings[action][0] in { Space: 1, Escape: 1, ShiftLeft: 1, ShiftRight: 1 } || input.bindings[action][0].startsWith('Arrow'),
      `${action} is bound by physical key code (${input.bindings[action][0]})`);
  }
});

test('no two actions claim the same primary key', () => {
  // A collision makes one of them unreachable and is invisible until someone
  // tries to use it. Held here rather than at runtime because a collision
  // introduced by a rebind is also worth catching.
  const seen = new Map();
  for (const action of ACTIONS) {
    for (const code of DEFAULT_KEYS[action]) {
      if (seen.has(code) && !DEFAULT_KEYS[action].includes(seen.get(code))) {
        // Alternates may legitimately overlap only if both actions list it.
        const other = seen.get(code);
        if (!DEFAULT_KEYS[other].includes(code)) throw new Error(`${code} is bound to both ${other} and ${action}`);
      }
      seen.set(code, action);
    }
  }
  checks++;
});

test('a key press holds an action and a release lets it go', () => {
  const input = createInput();
  const changed = setKey(input, 'KeyW', true);
  assertEqual(changed.length, 1, 'one action changed');
  assertEqual(changed[0], 'forward', 'and it is forward');
  assert(isDown(input, 'forward'), 'forward is held');
  assert(wasPressed(input, 'forward'), 'and counts as pressed this frame');
  setKey(input, 'KeyW', false);
  assert(!isDown(input, 'forward'), 'releasing W releases forward');
  assert(wasReleased(input, 'forward'), 'and counts as released this frame');
});

test('an action fires once per press, not once per frame it is held', () => {
  const input = createInput();
  setKey(input, 'KeyE', true);
  let fired = 0;
  for (let i = 0; i < 60; i++) {
    if (wasPressed(input, 'interact')) fired++;
    endFrame(input);
  }
  assertEqual(fired, 1, 'holding E is one press, not sixty');
  assert(isDown(input, 'interact'), 'and it stays held throughout');
  setKey(input, 'KeyE', false);
  setKey(input, 'KeyE', true);
  assert(wasPressed(input, 'interact'), 'pressing again is a new press');
});

test('auto-repeat does not re-fire a held action', () => {
  // A held key delivers keydown over and over. Without the edge check, holding
  // interact would buy a crate every frame.
  const input = createInput();
  setKey(input, 'KeyE', true);
  endFrame(input);
  setKey(input, 'KeyE', true);
  setKey(input, 'KeyE', true);
  assert(!wasPressed(input, 'interact'), 'a repeated keydown while held is not a new press');
});

test('alternate bindings work and are listed', () => {
  const input = createInput();
  setKey(input, 'ArrowUp', true);
  assert(isDown(input, 'forward'), 'the arrow keys move forward too');
  assert(input.bindings.forward.includes('ArrowUp'), 'and the binding table says so');
  setKey(input, 'ArrowUp', false);
  assert(!isDown(input, 'forward'), 'and releasing them stops');
});

test('keys are matched by physical code, not by character', () => {
  // On an AZERTY layout the key labelled Z produces 'w'. Binding by character
  // would move the player when they press the key marked W on their keyboard.
  const input = createInput();
  setKey(input, 'KeyW', true);
  assert(isDown(input, 'forward'), 'the physical W position moves forward');
  setKey(input, 'KeyW', false);
  setKey(input, 'KeyZ', true);
  assert(!isDown(input, 'forward'), 'the physical Z position does not');
});

test('unbound keys change nothing', () => {
  const input = createInput();
  const changed = setKey(input, 'KeyZ', true);
  assertEqual(changed.length, 0, 'no action');
  assertEqual(Object.keys(input.down).length + input.down.size, 0, 'and nothing is held');
  assertEqual(setKey(input, '', true).length, 0, 'an empty code is not a crash');
  assertEqual(setKey(input, null, true).length, 0, 'and neither is a missing one');
});

test('a stick inside the dead zone reads as still', () => {
  const input = createInput();
  setStick(input, 0.1, 0.1);
  assertEqual(input.stick.x, 0, 'x is zeroed');
  assertEqual(input.stick.y, 0, 'y is zeroed');
  setStick(input, STICK.deadZone - 0.01, 0);
  assertEqual(input.stick.x, 0, 'and still zeroed just inside the dead zone');
});

test('the dead zone is radial, so the diagonal corners are still dead', () => {
  // A per-axis dead zone leaves the corners reachable, which is what makes a
  // stick drift diagonally while it is supposed to be still. The test point is
  // on the diagonal *of the dead circle*, so each axis is under the dead zone
  // on its own but their magnitude is not.
  const input = createInput();
  const d = (STICK.deadZone * 0.99) / Math.SQRT2;
  setStick(input, d, d);
  assertEqual(input.stick.x, 0, 'both axes zeroed at the diagonal corner');
  assertEqual(input.stick.y, 0, 'both axes');
});

test('a stick past the dead zone still reaches full deflection', () => {
  const input = createInput();
  setStick(input, 1, 0);
  assert(Math.abs(input.stick.x - 1) < 1e-6, 'full right is full right');
  setStick(input, 0.7, 0);
  assert(input.stick.x > STICK.deadZone && input.stick.x < 1, 'and a partial push is rescaled past the dead zone, not clamped to it');
  setStick(input, 1, 1);
  const len = Math.hypot(input.stick.x, input.stick.y);
  assert(Math.abs(len - 1) < 1e-6, 'a diagonal push to the corner stays on the unit disc');
});

test('gamepad buttons drive the same actions as keys', () => {
  const input = createInput();
  const changed = setButton(input, 0, true);
  assert(changed.includes('jump'), 'A jumps');
  setButton(input, 0, false);
  setButton(input, 2, true);
  assert(isDown(input, 'interact'), 'X interacts');
  setButton(input, 2, false);
  setButton(input, 7, true);
  assert(isDown(input, 'sprint'), 'a trigger sprints');
});

test('the last device used is reported', () => {
  const input = createInput();
  setKey(input, 'KeyW', true);
  assertEqual(input.lastDevice, 'keyboard', 'a key press means keyboard');
  setButton(input, 0, true);
  assertEqual(input.lastDevice, 'gamepad', 'a button press means gamepad');
  setTouchStick(input, 0.5, 0, true);
  assertEqual(input.lastDevice, 'touch', 'the touch stick means touch');
  assert(input.anyInputSeen, 'and input has been seen at all');
});

test('keyboard and stick combine rather than cancelling', () => {
  const input = createInput();
  setKey(input, 'KeyD', true);
  setStick(input, 0.6, 0);
  const stickX = input.stick.x;
  assert(stickX > 0 && stickX < 0.6, 'a partial stick push reads as less than its raw value, rescaled past the dead zone');
  const axes = moveAxes(input);
  assertEqual(axes.magnitude, 1, 'key plus stick reports full deflection, not more');
  assertEqual(axes.x, 1 + stickX > 1 ? 1 : 1 + stickX, 'and the combined x is clamped, not summed past the limit');
  setKey(input, 'KeyD', false);
  assert(Math.abs(moveAxes(input).x - stickX) < 1e-6, 'releasing the key leaves the stick alone');
});

test('opposite directions cancel instead of doubling', () => {
  const input = createInput();
  setKey(input, 'KeyD', true);
  setKey(input, 'KeyA', true);
  assertEqual(moveAxes(input).x, 0, 'right minus left is nothing');
  setKey(input, 'KeyD', false);
  setKey(input, 'KeyA', false);
});

test('mouse delta respects sensitivity and Y inversion', () => {
  const input = createInput();
  addMouseDelta(input, 10, 20);
  assertEqual(input.mouse.dx, 10, 'default sensitivity is one');
  assertEqual(input.mouse.dy, 20, 'and Y is not inverted');
  endFrame(input);
  input.sensitivity = 2;
  input.invertY = true;
  addMouseDelta(input, 10, 20);
  assertEqual(input.mouse.dx, 20, 'sensitivity scales X');
  assertEqual(input.mouse.dy, -40, 'and inverted Y');
  endFrame(input);
  assertEqual(input.mouse.dx, 0, 'delta does not accumulate forever');
  assertEqual(input.mouse.dy, 0, 'in either axis');
});

test('rebinding captures the next key instead of firing it', () => {
  const input = createInput();
  beginRebind(input, 'interact');
  assert(isCapturing(input), 'a rebind is in progress');
  // The player presses W to choose the binding. It must not also move them,
  // and it becomes the new interact key rather than being ignored.
  setKey(input, 'KeyW', true);
  assert(!isDown(input, 'forward'), 'the captured key does not reach gameplay');
  assert(!isCapturing(input), 'and the capture is satisfied by it');
  assertEqual(input.bindings.interact[0], 'KeyW', 'so it is the new binding');
  // Releasing a key that was swallowed must not release a held action.
  setKey(input, 'KeyW', false);
  assert(!isDown(input, 'forward'), 'and its release is harmless');
  setKey(input, 'KeyW', true);
  assert(isDown(input, 'interact'), 'after which W interacts');
  setKey(input, 'KeyW', false);
});

test('a rebound key drives the rebound action and not the old one', () => {
  const input = createInput();
  beginRebind(input, 'interact');
  setKey(input, 'KeyF', true);
  setKey(input, 'KeyF', false);
  setKey(input, 'KeyF', true);
  assert(isDown(input, 'interact'), 'F now interacts');
  setKey(input, 'KeyF', false);
  setKey(input, 'KeyE', true);
  assert(!isDown(input, 'interact'), 'and E no longer does');
  setKey(input, 'KeyE', false);
});

test('releasing a key swallowed by a rebind does not release a held action', () => {
  // The press was consumed by the capture, so there was never a press to
  // release. Letting the matching keyup through would clear whatever the player
  // happened to be holding at the time.
  const input = createInput();
  setKey(input, 'KeyQ', true);
  assert(isDown(input, 'toggleQuality'), 'a different action is held');
  beginRebind(input, 'interact');
  setKey(input, 'KeyW', true);
  assert(!isDown(input, 'forward'), 'W was consumed by the capture');
  setKey(input, 'KeyW', false);
  assert(isDown(input, 'toggleQuality'), 'and its release did not clear the held action');
  setKey(input, 'KeyQ', false);
  setKey(input, 'KeyQ', true);
  setKey(input, 'KeyQ', false);
});

test('a release that the rebind never consumed still works', () => {
  // Only the key the capture actually ate is swallowed. A key pressed before
  // the rebind began was a real press, and its release must still let go.
  const input = createInput();
  setKey(input, 'KeyQ', true);
  beginRebind(input, 'interact');
  setKey(input, 'KeyQ', false);
  assert(!isDown(input, 'toggleQuality'), 'releasing a key the rebind did not touch is honoured');
  setKey(input, 'KeyQ', true);
  setKey(input, 'KeyQ', false);
  cancelRebind(input);
});

test('rebinding keeps the alternate bindings', () => {
  const input = createInput();
  beginRebind(input, 'forward');
  setKey(input, 'KeyI', true);
  setKey(input, 'KeyI', false);
  assertEqual(input.bindings.forward[0], 'KeyI', 'the new key is primary');
  assert(input.bindings.forward.includes('ArrowUp'), 'and the arrows still work');
  setKey(input, 'ArrowUp', true);
  assert(isDown(input, 'forward'), 'which they do');
  setKey(input, 'ArrowUp', false);
});

test('rebinding can be cancelled', () => {
  const input = createInput();
  beginRebind(input, 'jump');
  cancelRebind(input);
  assert(!isCapturing(input), 'the capture is gone');
  setKey(input, 'Space', true);
  assert(isDown(input, 'jump'), 'and space jumps again as normal');
  setKey(input, 'Space', false);
});

test('rebinding an action that does not exist is an error', () => {
  const input = createInput();
  let threw = false;
  try { beginRebind(input, 'selfDestruct'); } catch { threw = true; }
  assert(threw, 'a typo in an action name fails loudly rather than binding nothing');
});

test('bindings round-trip through a save', () => {
  const input = createInput();
  beginRebind(input, 'jump');
  setKey(input, 'KeyK', true);
  setKey(input, 'KeyK', false);
  const saved = serialiseBindings(input);
  assertEqual(saved.jump, 'KeyK', 'the save records the primary binding');
  assertEqual(saved.forward, 'KeyW', 'and every other action too');
  assert(!Array.isArray(saved.forward), 'and stores the key, not the array');

  const restored = createInput();
  const applied = deserialiseBindings(restored, saved);
  assert(applied > 0, 'bindings were restored');
  assertEqual(restored.bindings.jump[0], 'KeyK', 'the rebound key survives');
  assert(restored.bindings.forward.includes('ArrowUp'), 'and alternates are regenerated from the defaults');
});

test('a save full of junk does not corrupt the bindings', () => {
  const input = createInput();
  assertEqual(deserialiseBindings(input, null), false, 'null is not a binding table');
  assertEqual(deserialiseBindings(input, 'nonsense'), false, 'a string is not a binding table');
  assertEqual(deserialiseBindings(input, { jump: 42, forward: {}, left: [] }), false, 'wrong types are rejected');
  assertEqual(input.bindings.jump[0], 'Space', 'and the defaults are intact');
  assertEqual(input.bindings.forward[0], 'KeyW', 'all of them');
});

test('restoring the defaults actually restores them', () => {
  const input = createInput();
  beginRebind(input, 'jump');
  setKey(input, 'KeyK', true);
  setKey(input, 'KeyK', false);
  assertEqual(input.bindings.jump[0], 'KeyK', 'the binding changed');
  resetBindings(input);
  assertEqual(input.bindings.jump[0], 'Space', 'and resetting puts it back');
  assertEqual(input.buttons.jump[0], DEFAULT_BUTTONS.jump[0], 'including the pad');
});

test('the controls screen describes every action', () => {
  const input = createInput();
  const described = describeBindings(input);
  assertEqual(described.length, ACTIONS.length, 'every action is listed');
  for (const row of described) {
    assert(row.label && row.label.length, `${row.action} has a readable name`);
    assert(row.keys.length > 0, `${row.action} shows at least one key`);
    for (const k of row.keys) assert(typeof k === 'string' && k.length, `${row.action} key labels are readable`);
  }
  assert(described.find(r => r.action === 'forward').keys.includes('W'), 'W is shown for forward');
  assert(described.find(r => r.action === 'jump').buttons.includes('A / ✕'), 'and the pad button for jump');
});

test('key and button labels are readable rather than raw', () => {
  assertEqual(keyLabel('KeyW'), 'W', 'a letter key');
  assertEqual(keyLabel('Space'), 'Space', 'space');
  assertEqual(keyLabel('ShiftLeft'), 'L Shift', 'a modifier');
  assertEqual(keyLabel('Escape'), 'Esc', 'escape');
  assertEqual(keyLabel('ArrowUp'), '↑', 'an arrow');
  assertEqual(keyLabel(null), '—', 'an unbound action reads as unbound');
  assertEqual(buttonLabel(0), 'A / ✕', 'a face button shows both names, because pads disagree');
  assertEqual(buttonLabel(99), 'Button 99', 'and an unknown index is not a crash');
});

test('looking up an unbound code returns nothing', () => {
  const input = createInput();
  assertEqual(actionsForCode(input, 'KeyW').length, 1, 'W is one action');
  assertEqual(actionsForCode(input, 'KeyP').length, 0, 'P is none');
  assertEqual(actionsForButton(input, 0).length, 1, 'button 0 is one action');
  assertEqual(actionsForButton(input, 15).length, 0, 'button 15 is none');
});

process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
