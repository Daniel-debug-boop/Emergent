export const WORLD_SIZE = 9600;
export const REGION_SIZE = 1200;

export function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }

export function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s ^= s >>> 17; s ^= s << 5;
    return (s >>> 0) / 4294967296;
  };
}

export function hash2(x, y, seed = 1) {
  const n = Math.sin(x * 127.1 + y * 311.7 + seed * 0.00001) * 43758.5453123;
  return n - Math.floor(n);
}

export function fbm(x, y, seed = 1, octaves = 5) {
  let v = 0, a = 0.5;
  for (let i = 0; i < octaves; i++) {
    v += a * hash2(x, y, seed);
    x *= 2.03; y *= 2.01; a *= 0.5;
  }
  return v;
}

export function terrainHeight(x, z, seed = 1) {
  const macro = fbm(x * 0.0018, z * 0.0018, seed, 6);
  const detail = fbm(x * 0.007, z * 0.007, seed ^ 0x9e3779b9, 4);
  const ridge = Math.pow(Math.abs(fbm(x * 0.00075, z * 0.00075, seed ^ 0x45d9f3b, 4) * 2 - 1), 1.5);
  return (macro - 0.5) * 34 + (detail - 0.5) * 7 + ridge * 7;
}

function districtForRegion(reg, rand, riverX) {
  const x = reg.rx * REGION_SIZE + REGION_SIZE * 0.5;
  const z = reg.ry * REGION_SIZE + REGION_SIZE * 0.5;
  const radius = 280 + reg.density * 360;
  const typeRoll = hash2(reg.rx + 41, reg.ry - 23, reg.id + 1009);
  const type = typeRoll < 0.12 ? 'industrial' : typeRoll < 0.3 ? 'commercial' : typeRoll < 0.82 ? 'residential' : 'mixed';
  return { id: reg.id, x, z, r: radius, biome: reg.biome, density: reg.density, type, riverOffset: Math.abs(x - riverX) };
}

export function generateWorld(seed) {
  const rand = rng(seed);
  const w = {
    version: 3,
    seed: seed >>> 0,
    size: WORLD_SIZE,
    regionSize: REGION_SIZE,
    riverX: 4800 + (hash2(3, 7, seed) - 0.5) * 700,
    regions: [], districts: [], roads: [], buildings: [], trees: [], water: [],
    businesses: [], npcs: [], cars: [], events: [], discovered: {},
    time: 7.5, weather: 0, economy: 1, heat: 0, simTick: 0, eventSerial: 0
  };

  for (let ry = 0; ry < 8; ry++) for (let rx = 0; rx < 8; rx++) {
    const b = fbm(rx * 0.17, ry * 0.17, seed, 4);
    const density = 0.24 + 0.76 * hash2(rx + 9, ry + 13, seed);
    w.regions.push({ id: ry * 8 + rx, rx, ry, biome: b < 0.27 ? 'dry' : b > 0.72 ? 'lush' : 'temperate', density });
  }

  // Meandering river generated from deterministic control points.
  for (let z = -400; z < WORLD_SIZE + 400; z += 100) {
    const bend = Math.sin(z * 0.0015 + seed * 0.00011) * 170 + Math.sin(z * 0.00053) * 85;
    w.water.push({ x: w.riverX + bend, z, w: 190 + 45 * hash2(z * 0.01, 13, seed), d: 120 });
  }

  const center = WORLD_SIZE * 0.5;
  w.roads.push(
    { id: 0, x: center - 45, z: -240, w: 90, d: WORLD_SIZE + 480, main: 3 },
    { id: 1, x: -240, z: center - 45, w: WORLD_SIZE + 480, d: 90, main: 3 }
  );

  let roadId = 2;
  for (let x = 300; x < WORLD_SIZE; x += 600) {
    for (let z = 300; z < WORLD_SIZE; z += 600) {
      if (Math.abs(x - center) < 190 || Math.abs(z - center) < 190) continue;
      w.roads.push({ id: roadId++, x: x - 17, z: z - 300, w: 34, d: 600, main: 1 });
      w.roads.push({ id: roadId++, x: x - 300, z: z - 17, w: 600, d: 34, main: 1 });
      if (hash2(x * 0.01, z * 0.01, seed) > 0.68) {
        w.roads.push({ id: roadId++, x: x - 11, z: z - 190, w: 22, d: 380, main: 0 });
        w.roads.push({ id: roadId++, x: x - 190, z: z - 11, w: 380, d: 22, main: 0 });
      }
    }
  }

  for (const reg of w.regions) if (reg.density > 0.45) {
    w.districts.push(districtForRegion(reg, rand, w.riverX));
  }

  // Building placement is biased toward district cores and nearby roads, with anti-overlap.
  let buildingId = 0;
  const occupied = [];
  for (const d of w.districts) {
    const target = Math.floor(24 + d.density * 55);
    let made = 0, attempts = 0;
    while (made < target && attempts++ < target * 8) {
      const a = rand() * Math.PI * 2;
      const r = Math.sqrt(rand()) * d.r;
      const x = d.x + Math.cos(a) * r;
      const z = d.z + Math.sin(a) * r;
      if (x < 90 || x > WORLD_SIZE - 90 || z < 90 || z > WORLD_SIZE - 90) continue;
      if (Math.abs(x - w.riverX) < 170) continue;
      const road = nearestRoad(w.roads, x, z);
      const roadBias = road ? road.main >= 1 ? 0.82 : 0.52 : 0.18;
      if (rand() > 0.55 + roadBias * 0.35) continue;
      const bw = 28 + rand() * 72;
      const bd = 28 + rand() * 72;
      let overlap = false;
      for (const o of occupied) {
        if (Math.abs(x - o.x) < (bw + o.w) * 0.47 && Math.abs(z - o.z) < (bd + o.d) * 0.47) { overlap = true; break; }
      }
      if (overlap) continue;
      let floors = d.type === 'commercial' ? 3 + Math.floor(rand() * 9) : 1 + Math.floor(rand() * 5);
      if (d.type === 'industrial') floors = 1 + Math.floor(rand() * 3);
      const kindRoll = rand();
      const kind = kindRoll < (d.type === 'commercial' ? 0.33 : 0.12) ? 'shop' : kindRoll < 0.16 ? 'tower' : d.type === 'industrial' ? 'warehouse' : 'house';
      const b = {
        id: buildingId++, x, z, w: bw, d: bd, h: floors * 3.2 + 3 + rand() * 2,
        floors, kind, district: d.id, seed: rand(), facade: rand(), roof: rand(), sign: rand() < 0.28,
        doorSide: rand() < 0.5 ? 0 : 1
      };
      w.buildings.push(b); occupied.push(b); made++;
    }
  }

  // Vegetation density follows biome and settlement proximity.
  let treeId = 0;
  for (let i = 0; i < 5200; i++) {
    const x = rand() * WORLD_SIZE, z = rand() * WORLD_SIZE;
    if (Math.abs(x - w.riverX) < 150) continue;
    let nearDistrict = false, districtDensity = 0;
    for (const d of w.districts) {
      const dd = Math.hypot(x - d.x, z - d.z);
      if (dd < d.r) { nearDistrict = true; districtDensity = Math.max(districtDensity, d.density); }
    }
    const biome = w.regions[Math.min(63, Math.max(0, Math.floor(z / 1200) * 8 + Math.floor(x / 1200)))].biome;
    const chance = nearDistrict ? 0.16 + (1 - districtDensity) * 0.32 : biome === 'lush' ? 0.72 : biome === 'dry' ? 0.28 : 0.5;
    if (rand() > chance) continue;
    w.trees.push({ id: treeId++, x, z, s: 0.8 + rand() * 2.1, type: rand() < 0.17 ? 'pine' : rand() < 0.12 ? 'broadleaf' : 'tree', seed: rand() });
  }

  // Businesses emerge from commercial/ground-floor buildings.
  let businessId = 0;
  for (const b of w.buildings) {
    if (b.kind === 'shop' || (b.kind === 'warehouse' && rand() < 0.45) || rand() < 0.05) {
      const type = b.kind === 'warehouse' ? 'supply' : rand() < 0.35 ? 'market' : rand() < 0.6 ? 'cafe' : 'service';
      w.businesses.push({ id: businessId++, buildingId: b.id, type, stock: 18 + rand() * 82, price: 0.85 + rand() * 0.5, open: true, customers: 0, revenue: 0, reputation: 0.5 + rand() * 0.5 });
    }
  }

  // Persistent inhabitants with real linked destinations.
  let npcId = 0;
  for (let i = 0; i < 520; i++) {
    const homeD = w.districts[Math.floor(rand() * w.districts.length)] || { x: center, z: center, id: 0 };
    const workD = w.districts[Math.floor(rand() * w.districts.length)] || homeD;
    const jobRoll = rand();
    const job = jobRoll < 0.52 ? 'worker' : jobRoll < 0.72 ? 'merchant' : jobRoll < 0.87 ? 'service' : 'student';
    w.npcs.push({
      id: npcId++, name: npcName(i, rand), x: homeD.x + (rand() - 0.5) * 260, z: homeD.z + (rand() - 0.5) * 260,
      home: { x: homeD.x, z: homeD.z, district: homeD.id }, work: { x: workD.x, z: workD.z, district: workD.id },
      goal: null, job, speed: 16 + rand() * 14, energy: 70 + rand() * 30, money: 35 + rand() * 240,
      mood: 0.45 + rand() * 0.5, scheduleOffset: rand() * 0.9, state: 'home', activity: 'rest', memory: [],
      targetBusinessId: null, social: rand()
    });
  }

  // Traffic starts on the main road grid and keeps a deterministic lane offset.
  for (let i = 0; i < 150; i++) {
    const horizontal = rand() < 0.5;
    const lane = rand() < 0.5 ? -7 : 7;
    w.cars.push({
      id: i, horizontal, dir: rand() < 0.5 ? -1 : 1, lane,
      x: horizontal ? rand() * WORLD_SIZE : center + lane,
      z: horizontal ? center + lane : rand() * WORLD_SIZE,
      speed: 30 + rand() * 42, targetSpeed: 30 + rand() * 42, brake: 0, nearMiss: 0, routeSeed: rand(),
      color: [0.25 + rand() * 0.6, 0.25 + rand() * 0.6, 0.25 + rand() * 0.6]
    });
  }

  w.events.push({ id: w.eventSerial++, type: 'market', x: center + 350, z: center - 350, t: 140, life: 140, active: true, severity: 0.2, source: 'world' });
  return w;
}

function npcName(i, rand) {
  const first = ['Ari','Mika','Noor','Sam','Ira','Rafi','Leah','Omar','Zara','Kian','Maya','Sami','Ayan','Lina'];
  const last = ['Khan','Patel','Ali','Singh','Malik','Shah','Stone','Roy','Kerr','Das','Hayes','Iqbal'];
  return `${first[Math.floor(rand()*first.length)]} ${last[Math.floor(rand()*last.length)]} ${String(i+1).padStart(3,'0')}`;
}

export function nearestRoad(roads, x, z) {
  let best = null, bestD = Infinity;
  for (const r of roads) {
    const rx = clamp(x, r.x, r.x + r.w);
    const rz = clamp(z, r.z, r.z + r.d);
    const d = Math.hypot(x - rx, z - rz);
    if (d < bestD) { bestD = d; best = r; }
  }
  return best;
}

export function nearbyBusiness(world, x, z, maxDistance = 90) {
  let best = null, bestD = maxDistance;
  for (const b of world.businesses) {
    const building = world.buildings[b.buildingId];
    if (!building) continue;
    const d = Math.hypot(x - building.x, z - building.z);
    if (d < bestD) { bestD = d; best = b; }
  }
  return best;
}

export function districtAt(world, x, z) {
  let best = null, bestD = Infinity;
  for (const d of world.districts) {
    const dd = Math.hypot(x - d.x, z - d.z);
    if (dd < bestD && dd < d.r) { best = d; bestD = dd; }
  }
  return best;
}
