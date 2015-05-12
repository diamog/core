#include "phFilterMatching.h"
#include <apfGeometry.h>
#include <gmi.h>
#include <map>
#include <set>
#include <apf.h>

namespace ph {

void saveMatches(apf::Mesh* m, int dim, SavedMatches& sm)
{
  sm.resize(m->count(dim));
  apf::MeshIterator* it = m->begin(dim);
  apf::MeshEntity* e;
  unsigned i = 0;
  while ((e = m->iterate(it))) {
    m->getMatches(m, e, sm[i]);
    ++i;
  }
  m->end(it);
}

void restoreMatches(apf::Mesh2* m, int dim, SavedMatches& sm)
{
  apf::MeshIterator* it = m->begin(dim);
  apf::MeshEntity* e;
  unsigned i = 0;
  while ((e = m->iterate(it))) {
    if (sm[i].getSize()) {
      m->clearMatches(e);
      APF_ITERATE(apf::Matches, sm[i], mit)
        m->addMatch(e, mit->peer, mit->entity);
    }
    ++i;
  }
  m->end(it);
}

static double const tolerance = 1e-9;

typedef std::set<gmi_ent*> ModelSet;
typedef std::map<gmi_ent*, ModelSet> ModelMatching;

static void completeEntMatching(gmi_ent* e, ModelMatching& mm, ModelSet& visited)
{
  if (visited.count(e))
    return;
  visited.insert(e);
  if (!mm.count(e))
    return;
  ModelSet& adj = mm[e];
  APF_ITERATE(ModelSet, adj, it)
    completeEntMatching(*it, mm, visited);
}

static void completeMatching(ModelMatching& mm)
{
  ModelMatching mm2;
  APF_ITERATE(ModelMatching, mm, it) {
    ModelSet reachable;
    completeEntMatching(it->first, mm, reachable);
    mm2[it->first] = reachable;
  }
  mm = mm2;
}

static void addMatch(gmi_ent* a, gmi_ent* b, ModelMatching& mm)
{
  /* the hinge edge in axisymmetry can show up matching itself */
  if (a == b)
    return;
  mm[a].insert(b);
  mm[b].insert(a);
}

static void getAttributeMatching(gmi_model* gm, BCs& bcs, ModelMatching& mm)
{
  std::string name = "periodic slave";
  if (!haveBC(bcs, name))
    return;
  FieldBCs& fbcs = bcs.fields[name];
  APF_ITERATE(FieldBCs::Set, fbcs.bcs, it) {
    BC* bc = *it;
    gmi_ent* e = gmi_find(gm, bc->dim, bc->tag);
    double* val = bc->eval(apf::Vector3(0,0,0));
    int otherTag = *val;
    gmi_ent* oe = gmi_find(gm, bc->dim, otherTag);
    addMatch(e, oe, mm);
  }
}

static apf::Plane getFacePlane(gmi_model* gm, gmi_ent* f)
{
  double r[2];
  double p[2];
  gmi_range(gm, f, 0, r);
  p[0] = r[0];
  gmi_range(gm, f, 1, r);
  p[1] = r[0];
  apf::Vector3 origin;
  gmi_eval(gm, f, p, &origin[0]);
  apf::Vector3 normal;
  gmi_normal(gm, f, p, &normal[0]);
  return apf::Plane(normal, normal * origin);
}

static apf::Vector3 getVertexCenter(gmi_model* gm, gmi_ent* v)
{
  double p[2] = {0,0};
  apf::Vector3 center;
  gmi_eval(gm, v, p, &center[0]);
  return center;
}

static apf::Vector3 getEdgeCenter(gmi_model* gm, gmi_ent* e)
{
  double r[2];
  gmi_range(gm, e, 0, r);
  double p[2];
  p[0] = (r[1] + r[0]) / 2;
  apf::Vector3 center;
  gmi_eval(gm, e, p, &center[0]);
  return center;
}

static apf::Vector3 getCenter(gmi_model* gm, gmi_ent* e)
{
  switch (gmi_dim(gm, e)) {
    case 0:
      return getVertexCenter(gm, e);
    case 1:
      return getEdgeCenter(gm, e);
  };
  abort();
}

static double getDistanceWithFrame(gmi_model* gm, gmi_ent* e, gmi_ent* oe,
    apf::Frame const& frame)
{
  apf::Vector3 pt = getCenter(gm, e);
  apf::Vector3 opt = getCenter(gm, oe);
  return (frame * pt - opt).getLength();
}

static void closeFaceMatchingWithFrame(gmi_model* gm, gmi_ent* f, gmi_ent* of,
    ModelMatching& mm, apf::Frame const& frame)
{
  for (int dim = 0; dim < 2; ++dim) {
    gmi_set* s = gmi_adjacent(gm, f, dim);
    gmi_set* os = gmi_adjacent(gm, of, dim);
    /* these faces are matched, they should have the
       same layout of bounding entities with only geometric differences */
    assert(s->n == os->n);
    if (!s->n)
      continue;
    /* warning! this is an O(N^2) comparison.
       if it takes up a large part of your runtime,
       you should rethink your life. */
    for (int i = 0; i < s->n; ++i) {
      double minDist = getDistanceWithFrame(
          gm, s->e[i], os->e[0], frame);
      gmi_ent* closest = os->e[0];
      for (int j = 1; j < os->n; ++j) {
        double dist = getDistanceWithFrame(
            gm, s->e[i], os->e[j], frame);
        if (dist < minDist) {
          minDist = dist;
          closest = os->e[j];
        }
      }
      addMatch(s->e[i], closest, mm);
    }
    gmi_free_set(s);
    gmi_free_set(os);
  }
}

static apf::Vector3 getAnyPointOnFace(gmi_model* gm, gmi_ent* f)
{
  gmi_set* s = gmi_adjacent(gm, f, 1);
  assert(s);
  assert(s->n >= 1);
  apf::Vector3 p = getEdgeCenter(gm, s->e[0]);
  gmi_free_set(s);
  return p;
}

static void closeFaceMatching(gmi_model* gm, gmi_ent* f, ModelMatching& mm)
{
  assert(mm[f].size() == 1);
  gmi_ent* of = *(mm[f].begin());
  if (f < of)
    return;
  assert(f != of);
  /* the key assumptions are these:
     1) both faces are planar
     2) the periodic mapping between them is either:
        a) translation along the normal between their
           parallel planes
        b) rotation about the line of intersection of
           their planes
     3) model edges are well represented by the point
        at the center of their parametric range
     note that these are quite restrictive and hard to
     check for violations, so use this code with great care. */
  apf::Plane p = getFacePlane(gm, f);
  apf::Plane op = getFacePlane(gm, of);
  assert( ! apf::areClose(p, op, tolerance));
  apf::Frame frame;
  if (apf::areParallel(p, op, tolerance)) {
    apf::Vector3 o = p.normal * p.radius;
    apf::Vector3 oo = op.normal * op.radius;
    frame = apf::Frame::forTranslation(oo - o);
  } else {
    apf::Line line = apf::intersect(p, op);
    /* we need a couple points to know which of the four
       quadrants between the planes we are dealing with */
    apf::Vector3 pt = getAnyPointOnFace(gm, f);
    apf::Vector3 opt = getAnyPointOnFace(gm, of);
    apf::Vector3 v = apf::reject(pt - line.origin, line.direction);
    apf::Vector3 ov = apf::reject(opt - line.origin, line.direction);
    double angle = apf::getAngle(v, ov);
    if (apf::cross(v, ov) * line.direction < 0)
      angle = -angle;
    apf::Frame toline = apf::Frame::forTranslation(line.origin * -1);
    apf::Frame rotation = apf::Frame::forRotation(line.direction, angle);
    apf::Frame fromline = apf::Frame::forTranslation(line.origin);
    frame = fromline * rotation * toline;
  }
  closeFaceMatchingWithFrame(gm, f, of, mm, frame);
}

static void closeAttributeMatching(gmi_model* m, ModelMatching& mm)
{
  APF_ITERATE(ModelMatching, mm, it) {
    if (gmi_dim(m, it->first) == 2)
      closeFaceMatching(m, it->first, mm);
  }
}

static void getFullAttributeMatching(gmi_model* m, BCs& bcs, ModelMatching& mm)
{
  getAttributeMatching(m, bcs, mm);
  closeAttributeMatching(m, mm);
  completeMatching(mm);
}

void filterMatching(apf::Mesh2* m, BCs& bcs, int dim)
{
  ModelMatching mm;
  getFullAttributeMatching(m->getModel(), bcs, mm);
  apf::MeshIterator* it = m->begin(dim);
  m->end(it);
  /* todo */
}

}
