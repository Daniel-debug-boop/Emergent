/**
 * Tests for missions.mjs — the mission rules, with no game and no world.
 *
 * These are the rules a player loses a job to, so they are tested directly
 * rather than inferred from a delivery completing in the headless game. A bug
 * here is a job that cannot be finished or a job that finishes itself.
 *
 * Run: `npm run test:missions`
 */
import {
  ARCHETYPES, ARCHETYPE_IDS, STAGE_TYPES, buildMission, currentStage,
  timeRemaining, objectiveText, stageDistance, stageComplete, advanceMission,
  missionProgress, pickArchetype
} from './missions.mjs';

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

/** A deterministic stand-in for the world's random source. */
function seeded(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s ^= s >>> 17; s ^= s << 5;
    return (s >>> 0) / 4294967296;
  };
}

const AT = { x: 100, z: 200 };

/** A mission whose stages all sit at `x`, so distance is easy to reason about. */
function missionAt(archetype, x, count = 1) {
  return buildMission({
    archetype,
    rank: 1,
    resolve: (t, i) => ({ x, z: 0, businessId: 7, ...(i >= count ? {} : {}) })
  });
}

// ---------------------------------------------------------------------------

test('every archetype declares the fields the pipeline relies on', () => {
  assert(ARCHETYPE_IDS.length >= 4, 'there is more than one kind of job');
  for (const id of ARCHETYPE_IDS) {
    const a = ARCHETYPES[id];
    assertEqual(a.id, id, `${id} has a matching id`);
    assert(typeof a.label === 'string' && a.label.length, `${id} is labelled`);
    assert(typeof a.blurb === 'string' && a.blurb.length, `${id} explains itself`);
    assert(a.timeLimit > 0, `${id} has a time limit`);
    assert(typeof a.reward === 'function', `${id} computes a reward`);
    assert(Array.isArray(a.stages) && a.stages.length, `${id} has stages`);
    for (const s of a.stages) {
      assert(Object.values(STAGE_TYPES).includes(s.type), `${id}/${s.id} has a known stage type`);
      assert(typeof s.label === 'string' && s.label.length, `${id}/${s.id} is labelled`);
    }
  }
});

test('an unknown archetype is a clear error, not an empty job', () => {
  let msg = '';
  try { buildMission({ archetype: 'hauling' }); } catch (err) { msg = err.message; }
  assert(msg.includes('hauling'), `the error names the archetype (${msg})`);
});

test('a mission builds concrete stages from templates', () => {
  const m = buildMission({ archetype: 'delivery', rank: 3, resolve: t => ({ x: 10, z: 20, businessId: 1 }) });
  assertEqual(m.stages.length, 2, 'delivery has two stages');
  assertEqual(m.stages[0].type, STAGE_TYPES.interact, 'the first stage is an interaction');
  assertEqual(m.stages[0].businessId, 1, 'the resolver supplied the target');
  assertEqual(m.reward, 38 + 3 * 9, 'the reward follows the rank curve');
  assertEqual(m.stageIndex, 0, 'it starts at the first stage');
});

test('a repeated template produces one stage per leg', () => {
  const m = buildMission({ archetype: 'survey', rank: 1, resolve: (t, i) => ({ x: i * 100, z: 0 }) });
  assertEqual(m.stages.length, 3, 'the survey circuit has three points');
  assertEqual(m.stages[1].leg, '2/3', 'and each point knows which one it is');
  assert(objectiveText(m).includes('1/3'), 'the objective names the point being walked to');
  advanceMission(m, { x: 0, z: 0, dt: 0.1 });
  assert(objectiveText(m).includes('2/3'), 'and follows the player to the next one');
});

test('a stage the resolver drops is skipped, not left broken', () => {
  const m = buildMission({ archetype: 'delivery', rank: 1, resolve: (t, i) => (i === 0 ? { x: 0, z: 0, businessId: 2 } : null) });
  assert(m && m.stages.length === 1, 'the job degrades to the stage that resolved');
  assertEqual(m.stages[0].id, 'collect', 'and it is the one that survived');
  const nothing = buildMission({ archetype: 'delivery', rank: 1, resolve: () => null });
  assertEqual(nothing, null, 'a job with no stages at all is no job');
});

test('a goto stage completes on arrival and nowhere else', () => {
  const m = missionAt('survey', 100);
  const stage = currentStage(m);
  assert(!stageComplete(stage, { x: 0, z: 0, elapsedInStage: 0 }), 'not from 100 units away');
  assert(!stageComplete(stage, { x: 170, z: 0, elapsedInStage: 0 }), 'not just outside the radius');
  assert(stageComplete(stage, { x: 150, z: 0, elapsedInStage: 0 }), 'yes at the edge of the radius');
});

test('an interact stage needs the key press as well as the position', () => {
  const m = missionAt('delivery', 100);
  const stage = currentStage(m);
  assert(!stageComplete(stage, { x: 100, z: 0, interact: false, interactId: 7, elapsedInStage: 0 }),
    'standing on the spot is not the same as pressing the key');
  assert(!stageComplete(stage, { x: 500, z: 0, interact: true, interactId: 7, elapsedInStage: 0 }),
    'pressing the key on the wrong building does not count');
  assert(!stageComplete(stage, { x: 100, z: 0, interact: true, interactId: 9, elapsedInStage: 0 }),
    'and pressing it at the wrong business does not count');
  assert(stageComplete(stage, { x: 100, z: 0, interact: true, interactId: 7, elapsedInStage: 0 }),
    'the right press at the right place does');
});

test('a hold stage needs time inside the radius, not just arrival', () => {
  const m = buildMission({ archetype: 'respond', rank: 1, resolve: t => ({ x: 50, z: 0 }) });
  const arrive = currentStage(m);
  assertEqual(arrive.type, STAGE_TYPES.goto, 'the response starts by arriving');
  advanceMission(m, { x: 50, z: 0, dt: 0.1 });
  const hold = currentStage(m);
  assertEqual(hold.type, STAGE_TYPES.hold, 'and then holds');
  assert(!stageComplete(hold, { x: 50, z: 0, elapsedInStage: 2 }), 'two seconds is not six');
  assert(!stageComplete(hold, { x: 900, z: 0, elapsedInStage: 7 }), 'time spent outside does not count');
  assert(stageComplete(hold, { x: 50, z: 0, elapsedInStage: 6 }), 'six seconds inside does');
});

test('an evade stage completes by staying away', () => {
  const stage = { type: STAGE_TYPES.evade, x: 0, z: 0, radius: 50, duration: 10 };
  assert(!stageComplete(stage, { x: 10, z: 0, elapsedInStage: 12 }), 'standing on the spot is not evading');
  assert(stageComplete(stage, { x: 500, z: 0, elapsedInStage: 10 }), 'staying away for long enough is');
});

test('a mission runs its stages in order and reports each transition', () => {
  const m = missionAt('delivery', 100);
  const seen = [];
  let result;
  for (let i = 0; i < 4; i++) {
    result = advanceMission(m, { x: 100, z: 0, interact: true, interactId: 7, dt: 0.5 });
    seen.push(result.status);
  }
  assertEqual(seen[0], 'stage', 'the first interaction advances the stage');
  assertEqual(seen[1], 'complete', 'the second completes the mission');
  assertEqual(missionProgress(m), 1, 'progress reaches one');
  assertEqual(currentStage(m), null, 'there is no current stage once complete');
  assert(objectiveText(m).includes('complete'), 'and the objective says the job is done');
});

test('a mission expires rather than hanging forever', () => {
  const m = missionAt('delivery', 100);
  let result = null;
  for (let i = 0; i < 500; i++) result = advanceMission(m, { x: 0, z: 0, dt: 1 });
  assertEqual(result.status, 'expired', 'a job left alone runs out of time');
  assertEqual(timeRemaining(m), 0, 'and reports no time left');
  assert(m.elapsed > m.timeLimit, 'the clock ran past the limit rather than stopping on it');
});

test('an expired mission cannot be completed by a late arrival', () => {
  const m = missionAt('delivery', 100);
  for (let i = 0; i < 300; i++) advanceMission(m, { x: 0, z: 0, dt: 1 });
  const result = advanceMission(m, { x: 100, z: 0, interact: true, interactId: 7, dt: 0.1 });
  assertEqual(result.status, 'expired', 'expiry wins over the stage condition');
});

test('expiry is checked before the stage, not after', () => {
  // A single long step that both lands on the target and passes the limit must
  // report expiry. If the order were reversed, a player could bank a job they
  // were already too late for.
  const m = missionAt('delivery', 100);
  m.elapsed = m.timeLimit - 0.5;
  const result = advanceMission(m, { x: 100, z: 0, interact: true, interactId: 7, dt: 1 });
  assertEqual(result.status, 'expired', 'the deadline is absolute');
});

test('stage time resets between stages', () => {
  const m = buildMission({ archetype: 'respond', rank: 1, resolve: t => ({ x: 0, z: 0 }) });
  advanceMission(m, { x: 0, z: 0, dt: 4 });
  assertEqual(m.stageElapsed, 0, 'a new stage starts its clock again');
});

test('advanceMission tolerates no mission at all', () => {
  const r = advanceMission(null, { x: 0, z: 0, dt: 1 });
  assertEqual(r.status, 'progress', 'a null mission is not an error');
  assertEqual(currentStage(null), null, 'and has no stage');
  assertEqual(missionProgress(null), 0, 'and no progress');
});

test('a mission in the pre-pipeline save shape does not throw', () => {
  // A save written before stages existed restores as
  // `{type:'delivery', stage:'pickup', sourceId, targetId}`. Reading `stages`
  // off that is undefined, and the job would otherwise sit on the objective
  // line forever, unable to be completed.
  const legacy = { type: 'delivery', stage: 'pickup', sourceId: 3, targetId: 9, reward: 40 };
  assertEqual(currentStage(legacy), null, 'it has no current stage');
  assertEqual(missionProgress(legacy), 0, 'and no progress');
  const r = advanceMission(legacy, { x: 0, z: 0, dt: 1 });
  assertEqual(r.status, 'progress', 'and advancing it is not a crash');
  assertEqual(r.stage, null, 'and completes nothing');
});

test('the reward curve rises with rank and stays finite', () => {
  const costs = [];
  for (let rank = 1; rank <= 40; rank++) {
    const m = buildMission({ archetype: 'delivery', rank, resolve: t => ({ x: 0, z: 0 }) });
    assert(Number.isFinite(m.reward), `rank ${rank} produces a finite reward`);
    assert(m.reward > 0, `rank ${rank} is paid something`);
    costs.push(m.reward);
  }
  for (let i = 1; i < costs.length; i++) assert(costs[i] >= costs[i - 1], 'rewards never go down with rank');
});

test('stage distance is infinite for a stage with no position', () => {
  assertEqual(stageDistance(null, 0, 0), Infinity, 'no stage');
  assertEqual(stageDistance({ x: NaN, z: 0 }, 0, 0), Infinity, 'no position');
  assertEqual(stageDistance({ x: 3, z: 4 }, 0, 0), 5, 'a real position measures');
});

test('archetype selection only offers jobs the world can support', () => {
  const rand = seeded(99);
  assertEqual(pickArchetype({}, rand), null, 'nothing available means no job');
  assertEqual(pickArchetype({ delivery: false, survey: false }, rand), null, 'unavailable means unavailable');
  const delivery = pickArchetype({ delivery: true, survey: false, restock: false, respond: false }, rand);
  assertEqual(delivery, 'delivery', 'the only available job is offered');
  // And it never returns something that was not offered, over many draws.
  for (let i = 0; i < 200; i++) {
    const id = pickArchetype({ survey: true, restock: true }, rand);
    assert(id === 'survey' || id === 'restock', `drew an available archetype (${id})`);
  }
});

test('archetype selection is deterministic for a given seed', () => {
  const a = [], b = [];
  for (let i = 0; i < 20; i++) a.push(pickArchetype({ delivery: true, survey: true, restock: true, respond: true }, seeded(7)));
  for (let i = 0; i < 20; i++) b.push(pickArchetype({ delivery: true, survey: true, restock: true, respond: true }, seeded(7)));
  assertEqual(JSON.stringify(a), JSON.stringify(b), 'the same seed offers the same jobs');
});

process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
