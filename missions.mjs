/**
 * EMERGENT missions — a data-driven mission pipeline.
 *
 * A mission is plain data: a list of stages, each with a type, a position, a
 * radius and a label. Nothing in this file knows what a business is, what the
 * renderer is, or that a player exists beyond a position and a clock. The game
 * builds the stage list from world state; this file decides when a stage is
 * done, what happens next, and whether the whole thing succeeded or ran out of
 * time.
 *
 * That split is the point. The previous mission system was a single hard-coded
 * delivery, wired through `chooseMission`, `updateMission` and three branches
 * inside `interact`, which is why adding a second kind of job would have meant
 * editing all three. Here a new archetype is a data entry, and the evaluation,
 * expiry, reward and failure rules are written and tested once.
 *
 * Like `culling.mjs`, this module is deliberately pure — no GL, no DOM, no
 * world generation — so the rules most likely to be subtly wrong are the ones
 * with no excuse for being wrong.
 */

/** How a stage is completed. Each is a different verb, not a different script. */
export const STAGE_TYPES = {
  /** Reach a position. Covers delivery legs, survey points, arrivals. */
  goto: 'goto',
  /** Press the interact key at a position. Covers pickups, handovers, repairs. */
  interact: 'interact',
  /** Stay within a radius for a duration. Covers waiting out a shift, a delivery window. */
  hold: 'hold',
  /** Survive for a duration away from a position. Covers pursuits. */
  evade: 'evade'
};

/**
 * Mission archetypes.
 *
 * `stages` is a list of stage *templates*; `expand` turns a template and a
 * world query into concrete stages with real positions. A template may be
 * repeated (`repeat`) to produce a multi-leg job without the archetype needing
 * to know how many legs a job has.
 */
export const ARCHETYPES = {
  /**
   * Fetch from a business that has stock, deliver to one that is short.
   * The original emergent job, now expressed as two stages rather than a
   * `stage: 'pickup' | 'delivery'` string that every call site had to switch on.
   */
  delivery: {
    id: 'delivery',
    label: 'Delivery',
    blurb: 'Collect crates from a stocked business and deliver them to one that is short.',
    timeLimit: 210,
    reward: ctx => 38 + ctx.rank * 9,
    stages: [
      { id: 'collect', type: STAGE_TYPES.interact, label: 'Collect the crates', role: 'source', radius: 70 },
      { id: 'deliver', type: STAGE_TYPES.interact, label: 'Deliver the crates', role: 'target', radius: 70 }
    ]
  },

  /**
   * Restock a business that has run dry. One stage, no pickup leg, paid less —
   * and it is the archetype the economy actually wants, because it moves stock
   * from somewhere with surplus to somewhere with none.
   */
  restock: {
    id: 'restock',
    label: 'Restock',
    blurb: 'A local business has run dry. Bring it supplies.',
    timeLimit: 180,
    reward: ctx => 26 + ctx.rank * 7,
    stages: [
      { id: 'supply', type: STAGE_TYPES.interact, label: 'Hand over supplies', role: 'target', radius: 70 }
    ]
  },

  /**
   * Walk a survey circuit: three points in sequence. Pure traversal, so it
   * exercises the world rather than the economy, and it is the archetype that
   * pays for exploring districts you have not seen.
   */
  survey: {
    id: 'survey',
    label: 'Survey',
    blurb: 'Walk three survey points and log each one.',
    timeLimit: 300,
    reward: ctx => 30 + ctx.rank * 6,
    stages: [
      { id: 'waypoint', type: STAGE_TYPES.goto, label: 'Reach the survey point', role: 'waypoint', radius: 60, repeat: 3 }
    ]
  },

  /**
   * Get to somewhere that is already interesting — an active world event — and
   * stay there while the situation plays out. This is what makes the event
   * system a source of jobs rather than decoration.
   */
  respond: {
    id: 'respond',
    label: 'Respond',
    blurb: 'Something is happening. Get there and hold the scene.',
    timeLimit: 150,
    reward: ctx => 34 + ctx.rank * 8,
    stages: [
      { id: 'arrive', type: STAGE_TYPES.goto, label: 'Reach the incident', role: 'event', radius: 80 },
      { id: 'hold', type: STAGE_TYPES.hold, label: 'Hold the scene', role: 'event', radius: 80, duration: 6 }
    ]
  }
};

/** Archetype ids, for a caller that wants to pick without importing the table. */
export const ARCHETYPE_IDS = Object.keys(ARCHETYPES);

/**
 * Build a concrete mission from an archetype and a set of resolved roles.
 *
 * @param {object} spec
 * @param {string} spec.archetype Archetype id.
 * @param {(template:object, index:number)=>object|null} [spec.resolve]
 *        Turns a stage template into a concrete stage — this is the seam where
 *        the game supplies positions, business ids and labels. Returning null
 *        for a template drops it, which is how a delivery with no valid target
 *        degrades into a shorter job rather than a broken one.
 * @param {number} [spec.rank] Player rank, for the reward curve.
 * @param {number} [spec.timeLimit] Overrides the archetype default.
 * @returns {object|null} A mission, or null if nothing survived resolution.
 */
export function buildMission(spec) {
  const arch = ARCHETYPES[spec.archetype];
  if (!arch) throw new Error(`unknown mission archetype: ${spec.archetype}`);
  const resolve = spec.resolve || (t => t);
  const stages = [];
  for (const template of arch.stages) {
    const copies = template.repeat || 1;
    for (let i = 0; i < copies; i++) {
      const stage = resolve(template, stages.length);
      if (!stage) continue;
      stages.push({
        archetype: arch.id,
        label: stage.label || template.label,
        // Index within the repeated leg, so three survey points can each say
        // which one they are rather than all saying the same thing.
        leg: copies > 1 ? `${i + 1}/${copies}` : null,
        ...template,
        ...stage
      });
    }
  }
  if (!stages.length) return null;
  return {
    archetype: arch.id,
    label: arch.label,
    stages,
    stageIndex: 0,
    reward: Math.round(arch.reward({ rank: spec.rank || 1 })),
    timeLimit: spec.timeLimit ?? arch.timeLimit,
    elapsed: 0
  };
}

/**
 * The stage the player is currently on, or null if there is none.
 *
 * Tolerates a mission with no `stages` array, which is what a save written
 * before this pipeline restores to. Returning null makes such a job report as
 * finished rather than throwing on the first tick after a load.
 */
export function currentStage(mission) {
  if (!mission || !Array.isArray(mission.stages)) return null;
  return mission.stages[mission.stageIndex] || null;
}

/** Seconds left, or 0 once expired. */
export function timeRemaining(mission) {
  if (!mission) return 0;
  return Math.max(0, mission.timeLimit - mission.elapsed);
}

/** Human-readable objective for the HUD. Never empty while a mission exists. */
export function objectiveText(mission) {
  const stage = currentStage(mission);
  if (!stage) return 'Job complete.';
  const leg = stage.leg ? ` (${stage.leg})` : '';
  return `${stage.label}${leg} · ${Math.round(timeRemaining(mission))}s · $${mission.reward}`;
}

/** Distance from a point to a stage's target, or Infinity if it has none. */
export function stageDistance(stage, x, z) {
  if (!stage || !Number.isFinite(stage.x) || !Number.isFinite(stage.z)) return Infinity;
  return Math.hypot(x - stage.x, z - stage.z);
}

/**
 * Whether a stage is complete right now.
 *
 * @param {object} stage
 * @param {object} ctx { x, z, interact, elapsedInStage }
 * @returns {boolean}
 */
export function stageComplete(stage, ctx) {
  if (!stage) return true;
  const radius = stage.radius || 60;
  const inside = stageDistance(stage, ctx.x, ctx.z) <= radius;
  switch (stage.type) {
    case STAGE_TYPES.goto:
      return inside;
    case STAGE_TYPES.interact:
      // Interact stages need the key press *and* proximity. A press on the
      // wrong building must not advance the job, which is the bug a proximity
      // check alone would reintroduce every time the interact radius changed.
      return inside && !!ctx.interact && (stage.businessId === undefined || ctx.interactId === stage.businessId);
    case STAGE_TYPES.hold:
      return inside && ctx.elapsedInStage >= (stage.duration || 0);
    case STAGE_TYPES.evade:
      return ctx.elapsedInStage >= (stage.duration || 0) && !inside;
    default:
      return false;
  }
}

/**
 * Advance a mission by one simulation step.
 *
 * Returns a result object rather than mutating a status field, so the caller
 * cannot forget to check one: a caller that ignored the return value would
 * still be correct for every stage except completion.
 *
 * @param {object} mission Mutated in place (it is the game's live mission).
 * @param {object} ctx { x, z, interact, interactId, dt }
 * @returns {{status:'progress'|'stage'|'complete'|'expired', stage:object|null, index:number}}
 */
export function advanceMission(mission, ctx) {
  if (!mission || !Array.isArray(mission.stages) || !mission.stages.length) {
    return { status: 'progress', stage: null, index: 0 };
  }
  const dt = ctx.dt || 0;
  mission.elapsed += dt;
  if (mission.elapsed >= mission.timeLimit) {
    return { status: 'expired', stage: null, index: mission.stageIndex };
  }
  const stage = currentStage(mission);
  if (!stage) return { status: 'complete', stage: null, index: mission.stageIndex };

  // Time spent on this stage, so hold/evade stages have a duration to measure
  // without the mission object having to carry a per-stage clock that would
  // need resetting by hand on every advance.
  const previous = mission.stageElapsed || 0;
  mission.stageElapsed = previous + dt;
  if (!stageComplete(stage, { ...ctx, elapsedInStage: mission.stageElapsed })) {
    return { status: 'progress', stage, index: mission.stageIndex };
  }

  mission.stageIndex++;
  mission.stageElapsed = 0;
  if (mission.stageIndex >= mission.stages.length) {
    return { status: 'complete', stage: null, index: mission.stageIndex };
  }
  return { status: 'stage', stage: currentStage(mission), index: mission.stageIndex };
}

/** Fraction of the mission completed, for a progress bar. 0 when there is none. */
export function missionProgress(mission) {
  if (!mission || !Array.isArray(mission.stages) || !mission.stages.length) return 0;
  return Math.min(1, mission.stageIndex / mission.stages.length);
}

/**
 * Pick an archetype that the world can currently support.
 *
 * Availability is a property of world state, not of the archetype table: a
 * delivery needs a stocked source and a short target, a restock needs a dry
 * business, a survey needs somewhere to walk, a response needs an incident.
 * @param {object} availability Map of archetype id to boolean.
 * @param {() => number} rand
 */
export function pickArchetype(availability, rand) {
  const options = ARCHETYPE_IDS.filter(id => availability[id]);
  if (!options.length) return null;
  return options[Math.floor(rand() * options.length) % options.length];
}
