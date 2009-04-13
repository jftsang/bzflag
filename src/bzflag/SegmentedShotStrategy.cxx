/* bzflag
 * Copyright (c) 1993 - 2009 Tim Riker
 *
 * This package is free software;  you can redistribute it and/or
 * modify it under the terms of the license found in the file
 * named COPYING that should have accompanied this file.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/* interface header */
#include "SegmentedShotStrategy.h"

/* system implementation headers */
#include <assert.h>

/* common implementation headers */
#include "TextureManager.h"
#include "Intersect.h"
#include "BZDBCache.h"
#include "WallObstacle.h"
#include "EventHandler.h"

/* local implementation headers */
#include "sound.h"
#include "LocalPlayer.h"
#include "World.h"
#include "EffectsRenderer.h"
#include "playing.h"


SegmentedShotStrategy::SegmentedShotStrategy(ShotPath* _path,
                                             bool useSuperTexture, bool faint)
: ShotStrategy(_path)
{
  // initialize times
  prevTime = getPath().getStartTime();
  lastTime = currentTime = prevTime;

  // start at first segment
  lastSegment = segment = 0;

  // get team
  if (_path->getPlayer() == ServerPlayer) {
    TeamColor tmpTeam = _path->getFiringInfo().shot.team;
    team = (tmpTeam < RogueTeam) ? RogueTeam :
	   (tmpTeam > HunterTeam) ? RogueTeam : tmpTeam;
  } else {
    Player* p = lookupPlayer(_path->getPlayer());
    team = p ? p->getTeam() : RogueTeam;
  }

  // initialize scene nodes
  if (!headless) {
    boltSceneNode = new BoltSceneNode(_path->getPosition(),_path->getVelocity());

    const fvec4& c = Team::getRadarColor(team);
    if (faint) {
      boltSceneNode->setColor(c[0], c[1], c[2], 0.2f);
      boltSceneNode->setTextureColor(1.0f, 1.0f, 1.0f, 0.3f);
    } else {
      boltSceneNode->setColor(c[0], c[1], c[2], 1.0f);
    }
    if ((_path->getShotType() == CloakedShot) &&
        (LocalPlayer::getMyTank()->getFlag() != Flags::Seer)) {
      boltSceneNode->setInvisible(true);
    }

    TextureManager &tm = TextureManager::instance();
    std::string imageName = Team::getImagePrefix(team);
    if (useSuperTexture)
      imageName += BZDB.get("superPrefix");
    imageName += BZDB.get("boltTexture");

    boltSceneNode->phasingShot = useSuperTexture;

    int texture = tm.getTextureID(imageName.c_str());
    if (texture >= 0)
      boltSceneNode->setTexture(texture);
  }
}


SegmentedShotStrategy::~SegmentedShotStrategy()
{
  // free scene nodes
  if (!headless) {
    delete boltSceneNode;
  }
}


void SegmentedShotStrategy::update(float dt)
{
  prevTime = currentTime;
  currentTime += dt;

  // see if we've moved to another segment
  const int numSegments = (const int)segments.size();
  if (segment < numSegments && segments[segment].end <= currentTime) {
    lastSegment = segment;
    while (segment < numSegments && segments[segment].end <= currentTime) {
      if (++segment < numSegments) {
	switch (segments[segment].reason) {
	  case ShotPathSegment::Ricochet: {
            // play ricochet sound.  ricochet of local player's shots
            // are important, others are not.
            const PlayerId myTankId = LocalPlayer::getMyTank()->getId();
            const bool important = (getPath().getPlayer() == myTankId);
            const fvec3& pos = segments[segment].ray.getOrigin();
            SOUNDSYSTEM.play(SFX_RICOCHET, pos, important, false);

            // this is fugly but it's what we do
            const fvec3& newDir = segments[segment].ray.getDirection();
            const fvec3& oldDir = segments[segment - 1].ray.getDirection();
            const fvec3 normal = (newDir - oldDir).normalize();
            eventHandler.ShotRicochet(getPath(), pos, normal);

            EFFECTS.addRicoEffect(pos, normal);
            break;
          }
	  case ShotPathSegment::Boundary: {
	    break;
          }
	  default: {
            // this is fugly but it's what we do
            float dir[3];
            dir[0] = segments[segment].ray.getDirection()[0];
            dir[1] = segments[segment].ray.getDirection()[1];
            dir[2] = segments[segment].ray.getDirection()[2];

            float rots[2];
            const float horiz = sqrtf((dir[0]*dir[0]) + (dir[1]*dir[1]));
            rots[0] = atan2f(dir[1], dir[0]);
            rots[1] = atan2f(dir[2], horiz);

            const fvec3& pos = segments[segment].ray.getOrigin();
            EFFECTS.addShotTeleportEffect(pos, rots);
	    break;
          }
	}
      }
    }
  }

  // if ran out of segments then expire shot on next update
  if (segment == numSegments) {
    setExpiring();

    if (numSegments > 0) {
      ShotPathSegment &segm = segments[numSegments - 1];
      const float     *dir  = segm.ray.getDirection();
      const float speed = hypotf(dir[0], hypotf(dir[1], dir[2]));
      fvec3 pos;
      segm.ray.getPoint(float(segm.end - segm.start - 1.0 / speed), pos);
      /* NOTE -- comment out to not explode when shot expires */
      addShotExplosion(pos);
    }
  } else {
    // otherwise update position and velocity
    fvec3 p;
    segments[segment].ray.getPoint(float(currentTime - segments[segment].start), p);
    setPosition(p);
    setVelocity(segments[segment].ray.getDirection());
  }
}


bool SegmentedShotStrategy::predictPosition(float dt, fvec3& p) const
{
  float ctime = (float)currentTime + dt;
  int cur=0;
  // see if we've moved to another segment
  const int numSegments = (const int)segments.size();
  while (cur < numSegments && segments[cur].end < ctime) cur++;
  if (cur >= numSegments) return false;

  segments[segment].ray.getPoint(float(ctime - segments[segment].start), p);

  return true;
}


bool SegmentedShotStrategy::predictVelocity(float dt, fvec3& p) const
{
  float ctime = (float)currentTime + dt;
  int cur = 0;
  // see if we've moved to another segment
  const int numSegments = (const int)segments.size();
  while (cur < numSegments && segments[cur].end < ctime) { cur++; }
  if (cur >= numSegments) { return false; }

  p = segments[segment].ray.getDirection();

  return true;
}


void SegmentedShotStrategy::setCurrentTime(const double _currentTime)
{
  currentTime = _currentTime;
}


double	SegmentedShotStrategy::getLastTime() const
{
  return lastTime;
}


void SegmentedShotStrategy::setCurrentSegment(int _segment)
{
  segment = _segment;
}


float SegmentedShotStrategy::checkHit(const ShotCollider& tank,
                                      fvec3& position) const
{
  float minTime = Infinity;
  // expired shot can't hit anything
  if (getPath().isExpired()) return minTime;

  // get tank radius
  const float radius2 = tank.radius * tank.radius;

  // tank is positioned from it's bottom so shift position up by
  // half a tank height.
  const float tankHeight = tank.size[2];
  fvec3 lastTankPositionRaw = tank.motion.getOrigin();
  lastTankPositionRaw.z += 0.5f * tankHeight;
  Ray tankLastMotion(lastTankPositionRaw, tank.motion.getDirection());

  // if bounding box of tank and entire shot doesn't overlap then no hit
  const Extents& tankBBox = tank.bbox;
  if (!bbox.touches(tankBBox)) { return minTime; }

  float shotRadius = BZDB.eval(StateDatabase::BZDB_SHOTRADIUS);

  // check each segment in interval (prevTime,currentTime]
  const float dt = float(currentTime - prevTime);
  const int numSegments = (const int)segments.size();
  for (int i = lastSegment; i <= segment && i < numSegments; i++) {
    // can never hit your own first laser segment
    if (i == 0 && getPath().getShotType() == LaserShot && tank.testLastSegment)
      continue;

/*
    // skip segments that don't overlap in time with current interval
    if (segments[i].end <= prevTime) continue;
    if (currentTime <= segments[i].start) break;
*/

    // if shot segment and tank bboxes don't overlap then no hit, or if it's a shot that is out of the world boundry
    const ShotPathSegment& s = segments[i];
    if (!s.bbox.touches(tankBBox) || (s.reason == ShotPathSegment::Boundary)) {
      continue;
    }

    // construct relative shot ray:  origin and velocity relative to
    // my tank as a function of time (t=0 is start of the interval).
    Ray relativeRay(Intersect::rayMinusRay(s.ray, float(prevTime - s.start), tankLastMotion, 0.0f));

    // get hit time
    float t;
    if (tank.test2D) {
      // find closest approach to narrow box around tank.  width of box
      // is shell radius so you can actually hit narrow tank head on.
      static fvec3 tankBase(0.0f, 0.0f, -0.5f * tankHeight);
      t = Intersect::timeRayHitsBlock(relativeRay, tankBase, tank.angle,
			0.5f * tank.length, shotRadius, tankHeight);
    } else {
      // find time when shot hits sphere around tank
      t = Intersect::rayAtDistanceFromOrigin(relativeRay, 0.99f * tank.radius);
    }

    // short circuit if time is greater then smallest time so far
    if (t > minTime) continue;

    // make sure time falls within segment
    if (t < 0.0f || t > dt) continue;
    if (t > s.end - prevTime) continue;

    // check if shot hits tank -- get position at time t, see if in radius
    fvec3 closestPos;
    relativeRay.getPoint(t, closestPos);
    if (closestPos.lengthSq() < radius2) {
      // save best time so far
      minTime = t;

      // compute location of tank at time of hit
      fvec3 tankPos;
      tank.motion.getPoint(t, tankPos);

      // compute position of intersection
      position = tankPos + closestPos;
      //printf("%u:%u %u:%u\n", tank->getId().port, tank->getId().number, getPath().getPlayer().port, getPath().getPlayer().number);
    }
  }
  return minTime;
}


void SegmentedShotStrategy::addShot(SceneDatabase* scene, bool colorblind)
{
  const ShotPath& shotPath = getPath();
  boltSceneNode->move(shotPath.getPosition(), shotPath.getVelocity());
  if (boltSceneNode->getColorblind() != colorblind) {
    boltSceneNode->setColorblind(colorblind);
    TeamColor currentTeam = colorblind ? RogueTeam : team;

    const fvec4& c = Team::getRadarColor(currentTeam);
    boltSceneNode->setColor(c[0], c[1], c[2]);

    TextureManager &tm = TextureManager::instance();
    std::string imageName = Team::getImagePrefix(currentTeam);
    imageName += BZDB.get("boltTexture");
    int texture = tm.getTextureID(imageName.c_str());
    if (texture >= 0)
      boltSceneNode->setTexture(texture);
  }
  scene->addDynamicNode(boltSceneNode);
}


void SegmentedShotStrategy::radarRender() const
{
  const fvec3& orig = getPath().getPosition();
  const int length = BZDBCache::linedRadarShots;
  const int size   = BZDBCache::sizedRadarShots;

  float shotTailLength = BZDB.eval(StateDatabase::BZDB_SHOTTAILLENGTH);

  // Display leading lines
  if (length > 0) {
    const fvec3& vel = getPath().getVelocity();
    const fvec3 dir = vel.normalize() * shotTailLength * length;
    glBegin(GL_LINES);
    glVertex2fv(orig);
    if (BZDBCache::leadingShotLine) {
      glVertex2fv(orig.xy() + dir.xy());
    } else {
      glVertex2fv(orig.xy() - dir.xy());
    }
    glEnd();

    // draw a "bright" bullet tip
    if (size > 0) {
      glColor3f(0.75, 0.75, 0.75);
      glPointSize((float)size);
      glBegin(GL_POINTS);
      glVertex2fv(orig);
      glEnd();
      glPointSize(1.0f);
    }
  } else {
    if (size > 0) {
      // draw a sized bullet
      glPointSize((float)size);
      glBegin(GL_POINTS);
      glVertex2fv(orig);
      glEnd();
      glPointSize(1.0f);

    } else {
      // draw the tiny little bullet
      glBegin(GL_POINTS);
      glVertex2fv(orig);
      glEnd();
    }
  }

}


void SegmentedShotStrategy::makeSegments(ObstacleEffect e)
{
  // compute segments of shot until total length of segments exceeds the
  // lifetime of the shot.
  const ShotPath &shotPath = getPath();
  const fvec3&   v        = shotPath.getVelocity();
  double         startTime = shotPath.getStartTime();
  float          timeLeft  = shotPath.getLifetime();
  float          minTime   = BZDB.eval(StateDatabase::BZDB_MUZZLEFRONT) /
                             hypotf(v[0], hypotf(v[1], v[2]));
  World          *world    = World::getWorld();

  if (!world) {
    return; /* no world, no shots */
  }

  // if all shots ricochet and obstacle effect is stop, then make it ricochet
  if ((e == Stop) && world->allShotsRicochet()) {
    e = Reflect;
  }

  // prepare first segment
  fvec3 o, d;
  d = v; // use v[2] to have jumping affect shot velocity
  o = shotPath.getPosition();

  segments.clear();
  ShotPathSegment::Reason reason = ShotPathSegment::Initial;
  int i;
  const int maxSegment = 100;
  float worldSize = BZDBCache::worldSize / 2.0f - 0.01f;
  for (i = 0; (i < maxSegment) && (timeLeft > Epsilon); i++) {
    // construct ray and find the first building, teleporter, or outer wall
    fvec3 o2 = o - (minTime * d);

    // Sometime shot start outside world
    if (o2.x <= -worldSize) { o2.x = -worldSize; }
    if (o2.x >= +worldSize) { o2.x = +worldSize; }
    if (o2.y <= -worldSize) { o2.y = -worldSize; }
    if (o2.y >= +worldSize) { o2.y = +worldSize; }

    Ray r(o2, d);
    Ray rs(o, d);
    float t = timeLeft + minTime;
    int face;
    bool hitGround = getGround(r, Epsilon, t);
    Obstacle* building =
      (Obstacle*)((e == Through) ? NULL : getFirstBuilding(r, Epsilon, t));
    const Teleporter* teleporter = getFirstTeleporter(r, Epsilon, t, face);
    t -= minTime;
    minTime = 0.0f;
    bool ignoreHit = false;

    // if hit outer wall with ricochet and hit is above top of wall
    // then ignore hit.
    if (!teleporter && building && (e == Reflect) &&
	(building->getType() == WallObstacle::getClassName()) &&
	((o[2] + t * d[2]) > building->getHeight())) {
      ignoreHit = true;
    }

    // construct next shot segment and add it to list
    double endTime(startTime);

    if (t < 0.0f) {
      endTime += Epsilon;
    } else {
      endTime += t;
    }

    ShotPathSegment segm(startTime, endTime, rs, reason);
    segments.push_back(segm);
    startTime = endTime;

    // used up this much time in segment
    if (t < 0.0f) {
      timeLeft -= Epsilon;
    } else {
      timeLeft -= t;
    }

    // check in reverse order to see what we hit first
    reason = ShotPathSegment::Through;
    if (ignoreHit) {
      // uh...ignore this.  usually used if you shoot over the boundary wall.
      // just move the point of origin and build the next segment
      o += (t * d);
      reason = ShotPathSegment::Boundary;
    }
    else if (teleporter) {
      // entered teleporter -- teleport it
      unsigned int seed = shotPath.getShotId() + i;
      int source = world->getTeleporter(teleporter, face);
      int target = world->getTeleportTarget(source, seed);

      int outFace;
      const Teleporter* outTeleporter = world->getTeleporter(target, outFace);
      o += (t * d);
      teleporter->getPointWRT(*outTeleporter, face, outFace,
                              o, &d, 0.0f, o, &d, NULL);
      reason = ShotPathSegment::Teleport;
    }
    else if (building) {
      // hit building -- can bounce off or stop, buildings ignored for Through
      switch (e) {
	case Stop: {
	  if (!building->canRicochet()) {
            timeLeft = 0.0f;
            break;
          } else {
            // pass-through to the Reflect case
          }
        }
        case Reflect: {
          // move origin to point of reflection
          o += (t * d);

          // reflect direction about normal to building
          fvec3 normal;
          building->get3DNormal(o, normal);
          reflect(d, normal);
          reason = ShotPathSegment::Ricochet;
          break;
        }
        case Through: {
          assert(0);
          break;
        }
      }
    }
    else if (hitGround)	{ // we hit the ground
      switch (e) {
	case Stop:
	case Through: {
	  timeLeft = 0.0f;
	  break;
        }
	case Reflect: {
	  // move origin to point of reflection
          o += (t * d);

	  // reflect direction about normal to ground
	  fvec3 normal(0.0f, 0.0f, 0.0f);
	  reflect(d, normal);
	  reason = ShotPathSegment::Ricochet;
	  break;
	}
      }
    }
  }
  lastTime = startTime;

  // make bounding box for entire path
  const size_t numSegments = segments.size();
  if (numSegments > 0) {
    const ShotPathSegment& firstSeg = segments[0];
    bbox = firstSeg.bbox;
    for (size_t j = 1; j < numSegments; ++j) {
      const ShotPathSegment& segm = segments[j];
      bbox.expandToBox(segm.bbox);
    }
  } else {
    bbox.reset();
  }
}


const std::vector<ShotPathSegment>& SegmentedShotStrategy::getSegments() const
{
  return segments;
}


//
// NormalShotStrategy
//

NormalShotStrategy::NormalShotStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false)
{
  // make segments
  makeSegments(Stop);
}


NormalShotStrategy::~NormalShotStrategy()
{
  // do nothing
}


//
// RapidFireStrategy
//

RapidFireStrategy::RapidFireStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false)
{
  // speed up shell and decrease lifetime
  FiringInfo& f = getFiringInfo(_path);
  f.lifetime *= BZDB.eval(StateDatabase::BZDB_RFIREADLIFE);
  float fireAdVel = BZDB.eval(StateDatabase::BZDB_RFIREADVEL);
  f.shot.vel *= fireAdVel;
  setReloadTime(_path->getReloadTime()
		/ BZDB.eval(StateDatabase::BZDB_RFIREADRATE));

  // make segments
  makeSegments(Stop);
}


RapidFireStrategy::~RapidFireStrategy()
{
  // do nothing
}


//
// ThiefStrategy
//

ThiefStrategy::ThiefStrategy(ShotPath *_path) :
  SegmentedShotStrategy(_path, false),cumTime(0.0f)
{
  // speed up shell and decrease lifetime
  FiringInfo& f = getFiringInfo(_path);
  f.lifetime *= BZDB.eval(StateDatabase::BZDB_THIEFADLIFE);
  float thiefAdVel = BZDB.eval(StateDatabase::BZDB_THIEFADSHOTVEL);
  f.shot.vel *= thiefAdVel;
  setReloadTime(_path->getReloadTime()
		/ BZDB.eval(StateDatabase::BZDB_THIEFADRATE));

  // make segments
  makeSegments(Stop);
  setCurrentTime(getLastTime());
  endTime = f.lifetime;

  // make thief scene nodes
  const int numSegments = (const int)(getSegments().size());
  thiefNodes = new LaserSceneNode*[numSegments];

  TextureManager &tm = TextureManager::instance();
  int texture = tm.getTextureID("thief");

  for (int i = 0; i < numSegments; i++) {
    const ShotPathSegment& segm = getSegments()[i];
    const float t = float(segm.end - segm.start);
    const Ray& ray = segm.ray;
    const fvec3& rawdir = ray.getDirection();
    const fvec3 dir = t * rawdir;
    thiefNodes[i] = new LaserSceneNode(ray.getOrigin(), dir);
    if (texture >= 0) {
      thiefNodes[i]->setTexture(texture);
    }

    if (i == 0) {
      thiefNodes[i]->setFirst();
    }

    thiefNodes[i]->setColor(0,1,1);
    thiefNodes[i]->setCenterColor(0,0,0);
  }
  setCurrentSegment(numSegments - 1);
}


ThiefStrategy::~ThiefStrategy()
{
  const size_t numSegments = (getSegments().size());
  for (size_t i = 0; i < numSegments; i++) {
    delete thiefNodes[i];
  }
  delete[] thiefNodes;
}


void ThiefStrategy::update(float dt)
{
  cumTime += dt;
  if (cumTime >= endTime) {
    setExpired();
  }
}


void ThiefStrategy::addShot(SceneDatabase* scene, bool)
{
  // thief is so fast we always show every segment
  const size_t numSegments = (getSegments().size());
  for (size_t i = 0; i < numSegments; i++) {
    scene->addDynamicNode(thiefNodes[i]);
  }
}


void ThiefStrategy::radarRender() const
{
  // draw all segments
  const std::vector<ShotPathSegment>& segmts = getSegments();
  const size_t numSegments = segmts.size();
  glBegin(GL_LINES);
    for (size_t i = 0; i < numSegments; i++) {
      const ShotPathSegment& segm = segmts[i];
      const fvec3& origin = segm.ray.getOrigin();
      const fvec3& direction = segm.ray.getDirection();
      const float dt = float(segm.end - segm.start);
      glVertex2fv(origin);
      glVertex2f(origin[0] + dt * direction[0], origin[1] + dt * direction[1]);
    }
  glEnd();
}


bool ThiefStrategy::isStoppedByHit() const
{
  return false;
}


//
// MachineGunStrategy
//

MachineGunStrategy::MachineGunStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false)
{
  // speed up shell and decrease lifetime
  FiringInfo& f = getFiringInfo(_path);
  f.lifetime *= BZDB.eval(StateDatabase::BZDB_MGUNADLIFE);
  float mgunAdVel = BZDB.eval(StateDatabase::BZDB_MGUNADVEL);
  f.shot.vel[0] *= mgunAdVel;
  f.shot.vel[1] *= mgunAdVel;
  f.shot.vel[2] *= mgunAdVel;
  setReloadTime(_path->getReloadTime()
		/ BZDB.eval(StateDatabase::BZDB_MGUNADRATE));

  // make segments
  makeSegments(Stop);
}


MachineGunStrategy::~MachineGunStrategy()
{
  // do nothing
}


//
// RicochetStrategy
//

RicochetStrategy::RicochetStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false)
{
  // make segments that bounce
  makeSegments(Reflect);
}


RicochetStrategy::~RicochetStrategy()
{
  // do nothing
}


//
// SuperBulletStrategy
//

SuperBulletStrategy::SuperBulletStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, true)
{
  // make segments that go through buildings
  makeSegments(Through);
}

SuperBulletStrategy::~SuperBulletStrategy()
{
  // do nothing
}


//
// PhantomBulletStrategy
//

PhantomBulletStrategy::PhantomBulletStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false,true)
{
  // make segments that go through buildings
  makeSegments(Through);
}

PhantomBulletStrategy::~PhantomBulletStrategy()
{
  // do nothing
}


//
// LaserStrategy
//

LaserStrategy::LaserStrategy(ShotPath* _path) :
  SegmentedShotStrategy(_path, false), cumTime(0.0f)
{
  // speed up shell and decrease lifetime
  FiringInfo& f = getFiringInfo(_path);
  f.lifetime *= BZDB.eval(StateDatabase::BZDB_LASERADLIFE);
  float laserAdVel = BZDB.eval(StateDatabase::BZDB_LASERADVEL);
  f.shot.vel *= laserAdVel;
  setReloadTime(_path->getReloadTime()
		/ BZDB.eval(StateDatabase::BZDB_LASERADRATE));

  // make segments
  makeSegments(Stop);
  setCurrentTime(getLastTime());
  endTime = f.lifetime;

  // make laser scene nodes
  const int numSegments = (const int)(getSegments().size());
  laserNodes = new LaserSceneNode*[numSegments];
  const LocalPlayer* myTank = LocalPlayer::getMyTank();
  TeamColor tmpTeam = (myTank->getFlag() == Flags::Colorblindness) ? RogueTeam : team;

  TextureManager &tm = TextureManager::instance();
  std::string imageName = Team::getImagePrefix(tmpTeam);
  imageName += BZDB.get("laserTexture");
  int texture = tm.getTextureID(imageName.c_str());

  for (int i = 0; i < numSegments; i++) {
    const ShotPathSegment& segm = getSegments()[i];
    const float t = float(segm.end - segm.start);
    const Ray& ray = segm.ray;
    const fvec3& rawdir = ray.getDirection();
    const fvec3 dir = t * rawdir;
    laserNodes[i] = new LaserSceneNode(ray.getOrigin(), dir);
    if (texture >= 0)
      laserNodes[i]->setTexture(texture);

      const fvec4& color = Team::getRadarColor(tmpTeam);
      laserNodes[i]->setColor(color[0], color[1], color[2]);

      if (i == 0) {
        laserNodes[i]->setFirst();
      }
  }
  setCurrentSegment(numSegments - 1);
}


LaserStrategy::~LaserStrategy()
{
  const size_t numSegments = getSegments().size();
  for (size_t i = 0; i < numSegments; i++) {
    delete laserNodes[i];
  }
  delete[] laserNodes;
}


void LaserStrategy::update(float dt)
{
  cumTime += dt;
  if (cumTime >= endTime) {
    setExpired();
  }
}


void LaserStrategy::addShot(SceneDatabase* scene, bool)
{
  // laser is so fast we always show every segment
  const size_t numSegments = getSegments().size();
  for (size_t i = 0; i < numSegments; i++) {
    scene->addDynamicNode(laserNodes[i]);
  }
}


void LaserStrategy::radarRender() const
{
  // draw all segments
  const std::vector<ShotPathSegment>& segmts = getSegments();
  const size_t numSegments = segmts.size();
  glBegin(GL_LINES);
  for (size_t i = 0; i < numSegments; i++) {
    const ShotPathSegment& segm = segmts[i];
    const fvec3& origin = segm.ray.getOrigin();
    const fvec3& direction = segm.ray.getDirection();
    const float dt = float(segm.end - segm.start);
    glVertex2fv(origin);
    glVertex2f(origin[0] + dt * direction[0], origin[1] + dt * direction[1]);
  }
  glEnd();
}


bool LaserStrategy::isStoppedByHit() const
{
  return false;
}


// Local Variables: ***
// mode: C++ ***
// tab-width: 8 ***
// c-basic-offset: 2 ***
// indent-tabs-mode: t ***
// End: ***
// ex: shiftwidth=2 tabstop=8
