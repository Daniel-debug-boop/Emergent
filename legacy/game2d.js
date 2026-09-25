'use strict';
const C=document.getElementById('game'),ctx=C.getContext('2d',{alpha:false});
let W=innerWidth,H=innerHeight,DPR=Math.min(devicePixelRatio||1,2);
function resize(){W=innerWidth;H=innerHeight;C.width=W*DPR;C.height=H*DPR;ctx.setTransform(DPR,0,0,DPR,0,0)} addEventListener('resize',resize);resize();

// ---------- deterministic primitives ----------
function rng(seed){let s=seed>>>0;return()=>{s^=s<<13;s^=s>>>17;s^=s<<5;return(s>>>0)/4294967296}}
const clamp=(v,a,b)=>Math.max(a,Math.min(b,v));
const dist=(a,b)=>Math.hypot(a.x-b.x,a.y-b.y);
let seed=173927,rand=rng(seed),world,player,keys={},last=performance.now(),rendererMode=0;
let frames=0,secFrames=0,secTime=0,benchFrames=0,benchDraw=0,benchStart=0,benchDone=false;
const BENCH=new URLSearchParams(location.search).has('benchmark');
addEventListener('keydown',e=>{keys[e.key.toLowerCase()]=1;if(e.key.toLowerCase()==='r')reset((Date.now()>>>0));if(e.key.toLowerCase()==='t')rendererMode=(rendererMode+1)%4;if(e.key.toLowerCase()==='e')interact()});
addEventListener('keyup',e=>keys[e.key.toLowerCase()]=0);
function hash2(x,y,s=seed){let n=Math.sin(x*127.1+y*311.7+s*.00001)*43758.5453;return n-Math.floor(n)}
function fbm(x,y){let v=0,a=.5;for(let i=0;i<5;i++){v+=a*hash2(x,y);x*=2.03;y*=2.01;a*=.5}return v}

// ---------- hierarchical deterministic world ----------
function reset(s){seed=s>>>0;rand=rng(seed);world=genWorld();player={x:world.spawn.x,y:world.spawn.y,money:100,energy:100,discoveries:0};cache.clear();if(!BENCH)localStorage.setItem('emergent.save',JSON.stringify(saveState()));}
function genWorld(){
 const w={size:9600,regionSize:1200,roads:[],buildings:[],trees:[],water:[],npcs:[],cars:[],businesses:[],districts:[],regions:[],events:[],discovered:{},time:8,weather:0,economy:1,heat:0,simTick:0,spawn:{x:4800,y:4800}};
 // WORLD -> REGIONS -> BIOMES
 for(let ry=0;ry<8;ry++)for(let rx=0;rx<8;rx++){let b=fbm(rx*.17,ry*.17);w.regions.push({id:ry*8+rx,rx,ry,biome:b<.3?'dry':b>.72?'lush':'temperate',density:.35+.65*hash2(rx+9,ry+13)});}
 const riverX=4800+Math.sin(seed)*320;for(let y=-300;y<w.size+300;y+=120)w.water.push({x:riverX+Math.sin(y*.0017)*150,y,w:220+70*hash2(y*.01,seed),h:150});
 const cx=4800,cy=4800;
 // primary + secondary road hierarchy
 w.roads.push({x:cx-55,y:-200,w:110,h:w.size+400,main:2},{x:-200,y:cy-55,w:w.size+400,h:110,main:2});
 for(let x=300;x<w.size;x+=600)for(let y=300;y<w.size;y+=600){if(Math.abs(x-cx)<180||Math.abs(y-cy)<180)continue;w.roads.push({x:x-22,y:y-300,w:44,h:600,main:1});w.roads.push({x:x-300,y:y-22,w:600,h:44,main:1});}
 // settlements emerge from region density; districts follow road grid
 for(const reg of w.regions){let sx=reg.rx*w.regionSize+600,sy=reg.ry*w.regionSize+600;if(reg.density<.5)continue;w.districts.push({id:reg.id,x:sx,y:sy,r:260+reg.density*300,biome:reg.biome,density:reg.density});}
 // buildings are biased toward districts + roads
 for(const d of w.districts){let count=Math.floor(18+d.density*34);for(let i=0;i<count;i++){let a=rand()*Math.PI*2,r=Math.sqrt(rand())*d.r,x=d.x+Math.cos(a)*r,y=d.y+Math.sin(a)*r;if(Math.abs(x-riverX)<150)continue;let z=8+rand()*55,kind=rand()<.12?'tower':rand()<.2?'shop':'house';w.buildings.push({x,y,w:35+rand()*60,h:35+rand()*60,z,kind,seed:rand(),district:d.id});}}
 for(let i=0;i<4200;i++){let x=rand()*w.size,y=rand()*w.size;if(Math.abs(x-riverX)<150)continue;let near=0;for(const d of w.districts){if(Math.hypot(x-d.x,y-d.y)<d.r)near=1}if(near&&rand()<.75)continue;w.trees.push({x,y,s:.55+rand()*1.5,type:rand()<.15?'pine':'tree',seed:rand()});}
 // businesses derive from buildings
 for(const b of w.buildings)if(b.kind==='shop'||rand()<.08)w.businesses.push({id:w.businesses.length,building:b,stock:20+rand()*80,price:1,open:true,customers:0});
 // persistent agents with home/work linked to districts
 for(let i=0;i<520;i++){let homeD=w.districts[Math.floor(rand()*w.districts.length)]||{x:cx,y:cy,id:0};let workD=w.districts[Math.floor(rand()*w.districts.length)]||homeD;w.npcs.push({id:i,x:homeD.x+(rand()-.5)*300,y:homeD.y+(rand()-.5)*300,home:{x:homeD.x,y:homeD.y},work:{x:workD.x,y:workD.y},goal:null,t:rand()*24,job:rand()<.55?'worker':rand()<.75?'merchant':'resident',needs:{energy:60+rand()*40,money:30+rand()*180},memory:[],state:'home',speed:18+rand()*24,mood:rand()});}
 for(let i=0;i<150;i++){let horizontal=rand()<.5;w.cars.push({x:horizontal?rand()*w.size:cx+(rand()-.5)*100,y:horizontal?cy+(rand()-.5)*100:rand()*w.size,dir:rand()<.5?-1:1,horizontal,speed:40+rand()*55,nearMiss:0});}
 w.events.push({type:'market',x:cx+350,y:cy-350,t:90,life:90,active:true});return w;
}
function saveState(){return {seed, time:world.time,economy:world.economy,weather:world.weather,heat:world.heat,discovered:world.discovered,events:world.events.slice(-30),npc:world.npcs.filter(n=>n.id<30).map(n=>({id:n.id,x:n.x,y:n.y,money:n.needs.money,state:n.state,memory:n.memory.slice(-5)}))}}
function restoreState(){try{let s=JSON.parse(localStorage.getItem('emergent.save')||'null');if(s&&s.seed===seed){world.time=s.time;world.economy=s.economy;world.weather=s.weather;world.heat=s.heat;world.discovered=s.discovered||{};}}catch(e){}}
function roadAt(x,y){for(const r of world.roads)if(x>=r.x-4&&x<=r.x+r.w+4&&y>=r.y-4&&y<=r.y+r.h+4)return r;return null}
function blocked(x,y){for(const b of world.buildings)if(x>b.x-12&&x<b.x+b.w+12&&y>b.y-12&&y<b.y+b.h+12)return true;return false}
function nearbyBusiness(){let best=null,bd=1e9;for(const b of world.businesses){let d=dist(player,b.building);if(d<bd){bd=d;best=b}}return bd<90?best:null}
function interact(){let b=nearbyBusiness();if(b&&b.open){let cost=3*world.economy;if(player.money>=cost){player.money-=cost;b.stock--;b.customers++;player.energy=clamp(player.energy+8,0,100);world.heat+=.01;world.events.push({type:'trade',x:b.building.x,y:b.building.y,t:15,life:15,active:true})}}}

// ---------- simulation: simple rules create interacting outcomes ----------
function update(dt){
 world.time=(world.time+dt*.12)%24;world.simTick+=dt; if(rand()<dt*.0015)world.weather=(world.weather+1)%3;
 let dx=(keys.d||keys.arrowright?1:0)-(keys.a||keys.arrowleft?1:0),dy=(keys.s||keys.arrowdown?1:0)-(keys.w||keys.arrowup?1:0),l=Math.hypot(dx,dy)||1;dx/=l;dy/=l;let moving=l>0,sp=keys.shift?330:210;let nx=clamp(player.x+dx*sp*dt,40,world.size-40),ny=clamp(player.y+dy*sp*dt,40,world.size-40);if(!blocked(nx,ny)){player.x=nx;player.y=ny}player.speed=moving?sp:0;player.energy=clamp(player.energy+(moving?-3:5)*dt,0,100);
 const hour=world.time;
 // Discover districts as the player explores; discovery persists in the world save.
 for(const d of world.districts)if(Math.hypot(player.x-d.x,player.y-d.y)<d.r*.55&&!world.discovered[d.id]){world.discovered[d.id]={time:world.time,seed:seed};player.discoveries++;world.events.push({type:'discovery',x:d.x,y:d.y,t:18,life:18,active:true});}
 for(const n of world.npcs){n.t+=dt;n.needs.energy-=dt*.08;n.needs.money+=n.job==='merchant'?dt*.05:0;
   if(!n.goal||n.t>12){n.t=0;let target=(n.job==='worker'&&hour>=8&&hour<17)?n.work:(hour>=18?n.home:(rand()<.35?{x:n.work.x,y:n.work.y}:{x:n.home.x,y:n.home.y}));n.goal={x:target.x+(rand()-.5)*80,y:target.y+(rand()-.5)*80};n.state=target===n.work?'work':target===n.home?'home':'travel';}
   let vx=n.goal.x-n.x,vy=n.goal.y-n.y,d=Math.hypot(vx,vy);if(d>5){n.x+=vx/d*n.speed*dt;n.y+=vy/d*n.speed*dt;}else{n.state='idle';}
   if(n.needs.energy<15){n.state='rest';n.goal={x:n.home.x,y:n.home.y};n.needs.energy=55;}
 }
 for(const c of world.cars){if(c.horizontal){c.x+=c.speed*c.dir*dt;if(c.x<-120)c.x=world.size+120;if(c.x>world.size+120)c.x=-120}else{c.y+=c.speed*c.dir*dt;if(c.y<-120)c.y=world.size+120;if(c.y>world.size+120)c.y=-120}}
 // economy feedback: population, stock, weather and player trades alter price pressure
 let localPop=0;for(const n of world.npcs)if(Math.hypot(n.x-player.x,n.y-player.y)<500)localPop++;let stock=0;for(const b of world.businesses)stock+=b.stock;
 world.economy=clamp(world.economy+dt*((localPop/1800)-.0008*world.businesses.length-(world.weather===2?.002:0)),.65,1.6);
 for(const b of world.businesses){if(b.stock<5){b.open=false;world.events.push({type:'shortage',x:b.building.x,y:b.building.y,t:25,life:25,active:true});}else if(b.stock<25&&rand()<dt*.02)b.stock+=5;if(b.open&&rand()<dt*.03)b.stock-=1;}
 world.heat=Math.max(0,world.heat-dt*.004);
 for(const e of world.events){e.t-=dt;if(e.t<=0)e.active=false}world.events=world.events.filter(e=>e.active).slice(-80);
 if(world.simTick>10){world.simTick=0;localStorage.setItem('emergent.save',JSON.stringify(saveState()));}
}

// ---------- adaptive renderer ----------
const cache=new Map(), stats={reused:0,recomputed:0,visible:0,importance:0};
function chunkKey(x,y){return x+','+y}
function importance(xx,yy){let cx=xx*256+128,cy=yy*256+128,d=Math.hypot(cx-player.x,cy-player.y);let speed=player.energy<20?.2:player.speed||0;let motion=clamp(speed/330,0,1);let weather=world.weather===2?.2:0;let event=world.events.some(e=>Math.hypot(e.x-cx,e.y-cy)<350)?.35:0;let predicted=(hash2(xx*.73,yy*1.17,Math.floor(world.time*4)+seed)>.72)?.25:0;return clamp((1-d/2200)*.45+motion*.1+weather+event+predicted,.03,1)}
function project(x,y){return{x:W/2+(x-player.x),y:H/2+(y-player.y)}}
function visible(o,pad=100){return o.x>player.x-W/2-pad&&o.x<player.x+W/2+pad&&o.y>player.y-H/2-pad&&o.y<player.y+H/2+pad}
function drawWorld(){
 stats.reused=stats.recomputed=stats.visible=0;let day=.5+.5*Math.sin((world.time-6)/24*Math.PI*2),night=1-day;ctx.fillStyle=`rgb(${20+28*day},${34+42*day},${43+60*day})`;ctx.fillRect(0,0,W,H);
 const cx=Math.floor(player.x/256),cy=Math.floor(player.y/256),rx=Math.ceil(W/512)+1,ry=Math.ceil(H/512)+1;
 let budget=rendererMode===3?Math.max(4,Math.floor((rx*2+3)*(ry*2+3)*.28)):rendererMode===2?Math.max(7,Math.floor((rx*2+3)*(ry*2+3)*.5)):9999;
 let candidates=[];for(let yy=cy-ry;yy<=cy+ry;yy++)for(let xx=cx-rx;xx<=cx+rx;xx++)candidates.push({xx,yy,k:importance(xx,yy)});candidates.sort((a,b)=>b.k-a.k);
 for(const q of candidates){let k=chunkKey(q.xx,q.yy),c=cache.get(k),bucket=Math.floor(world.time*4),needs=!c||rendererMode===0||c.bucket!==bucket&&q.k>.42;if(rendererMode>=2&&q.k<.16&&c)needs=false;if(rendererMode===3&&q.k<.34&&c)needs=false;if(needs&&stats.recomputed<budget){c=renderChunk(q.xx,q.yy,day,q.k);c.bucket=bucket;cache.set(k,c);stats.recomputed++;}else if(c){stats.reused++;}else{c=renderChunk(q.xx,q.yy,day,q.k);c.bucket=bucket;cache.set(k,c);stats.recomputed++;}ctx.drawImage(c.canvas,q.xx*256-player.x+W/2,q.yy*256-player.y+H/2);}
 // water and dynamics
 ctx.globalAlpha=.68;for(const q of world.water){if(visible(q,220)){let p=project(q.x,q.y);ctx.fillStyle=`rgba(35,105,150,.58)`;ctx.fillRect(p.x,p.y,q.w,q.h)}}ctx.globalAlpha=1;
 for(const c of world.cars){if(!visible(c,80))continue;stats.visible++;let p=project(c.x,c.y);ctx.save();ctx.translate(p.x,p.y);if(!c.horizontal)ctx.rotate(Math.PI/2);ctx.fillStyle='#c9d4df';ctx.fillRect(-11,-5,22,10);ctx.fillStyle='#17212c';ctx.fillRect(-5,-4,8,3);ctx.restore()}
 for(const n of world.npcs){if(!visible(n,50))continue;stats.visible++;let p=project(n.x,n.y);ctx.fillStyle=n.job==='worker'?'#f1c27d':n.job==='merchant'?'#9bd1ff':'#d9a6ff';ctx.beginPath();ctx.arc(p.x,p.y,3,0,Math.PI*2);ctx.fill()}
 let pp=project(player.x,player.y);ctx.fillStyle='#fff';ctx.beginPath();ctx.arc(pp.x,pp.y,7,0,Math.PI*2);ctx.fill();
 ctx.fillStyle=`rgba(5,10,25,${night*.48})`;ctx.fillRect(0,0,W,H);if(world.weather===1){ctx.fillStyle='rgba(180,210,230,.10)';ctx.fillRect(0,0,W,H)}if(world.weather===2){ctx.strokeStyle='rgba(150,190,220,.20)';for(let i=0;i<Math.floor(W/18);i++){let x=(i*73+frames*18)%W;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x-20,H);ctx.stroke()}}
 return {chunks:candidates.length,reused:stats.reused,recomputed:stats.recomputed,visible:stats.visible};
}
function renderChunk(xx,yy,day,imp){let c=document.createElement('canvas');c.width=c.height=256;let g=c.getContext('2d'),ox=xx*256,oy=yy*256;let biome=world.regions[Math.max(0,Math.min(63,Math.floor(oy/1200)*8+Math.floor(ox/1200)))]?.biome||'temperate';g.fillStyle=biome==='dry'?'#5a5134':biome==='lush'?'#25452e':day>.35?'#26372d':'#18251f';g.fillRect(0,0,256,256);
 for(const r of world.roads){if(r.x+r.w<ox||r.x>ox+256||r.y+r.h<oy||r.y>oy+256)continue;g.fillStyle=r.main===2?'#20252a':'#292e32';g.fillRect(r.x-ox,r.y-oy,r.w,r.h);}
 for(const b of world.buildings){if(b.x+b.w<ox||b.x>ox+256||b.y+b.h<oy||b.y>oy+256)continue;let shade=.72+.28*b.seed;g.fillStyle=b.kind==='tower'?`rgb(${70+40*shade},${72+35*shade},${78+45*shade})`:`rgb(${85+35*shade},${75+30*shade},${62+25*shade})`;g.fillRect(b.x-ox,b.y-oy,b.w,b.h);if(imp>.35){g.fillStyle='rgba(35,55,70,.7)';for(let wx=b.x+8;wx<b.x+b.w-4;wx+=14)for(let wy=b.y+8;wy<b.y+b.h-4;wy+=15)g.fillRect(wx-ox,wy-oy,5,6)}}
 const step=imp<.25?3:1;for(let i=0;i<world.trees.length;i+=step){let t=world.trees[i];if(t.x<ox-20||t.x>ox+276||t.y<oy-20||t.y>oy+276)continue;g.fillStyle=t.type==='pine'?'#183d2a':'#285331';g.beginPath();g.arc(t.x-ox,t.y-oy,5*t.s,0,7);g.fill();g.fillStyle='#4b3624';g.fillRect(t.x-ox-1,t.y-oy+3,2,7*t.s)}return c}
function marker(){let target={x:world.spawn.x+350,y:world.spawn.y-350},p=project(target.x,target.y);if(p.x>-50&&p.x<W+50&&p.y>-50&&p.y<H+50){ctx.strokeStyle='#ffd56b';ctx.lineWidth=2;ctx.beginPath();ctx.arc(p.x,p.y,16+Math.sin(performance.now()*.004)*3,0,7);ctx.stroke()}}
function frame(now){let dt=Math.min(.05,(now-last)/1000);last=now;update(dt);let t0=performance.now(),r=drawWorld();marker();let drawMs=performance.now()-t0;frames++;secFrames++;secTime+=dt;if(BENCH){benchFrames++;benchDraw+=drawMs;if(!benchStart)benchStart=now;if(now-benchStart>3500&&!benchDone){benchDone=true;document.body.dataset.benchmark=JSON.stringify({seed,mode:['BASELINE','CACHE','TEMPORAL','ADAPTIVE'][rendererMode],fps:+(benchFrames/(now-benchStart)*1000).toFixed(2),avgDrawMs:+(benchDraw/benchFrames).toFixed(3),recomputed:stats.recomputed,reused:stats.reused,chunks:r.chunks,visible:r.visible,npcs:world.npcs.length,cars:world.cars.length,buildings:world.buildings.length,trees:world.trees.length,businesses:world.businesses.length,regions:world.regions.length});}}
 if(secTime>=1&&!BENCH){let fps=secFrames/secTime;document.getElementById('stats').innerHTML=`FPS ${fps.toFixed(0)} · draw ${drawMs.toFixed(1)}ms · visible ${r.visible}<br>NPC ${world.npcs.length} · cars ${world.cars.length} · buildings ${world.buildings.length}<br><span class="badge">reused ${r.reused}/${r.chunks} · recomputed ${r.recomputed}</span>`;document.getElementById('mission').textContent=`Market Run · $${player.money.toFixed(0)} · ${world.businesses.filter(b=>b.open).length} businesses open`;document.getElementById('clock').textContent=`${world.time.toFixed(1)}:00 · ${['clear','mist','rain'][world.weather]} · economy ${world.economy.toFixed(2)}`;document.getElementById('mode').textContent=['BASELINE','CACHE','TEMPORAL','ADAPTIVE'][rendererMode];secTime=secFrames=0;}
 requestAnimationFrame(frame)}
reset(seed);restoreState();requestAnimationFrame(frame);
