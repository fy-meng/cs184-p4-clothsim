#include <iostream>
#include <math.h>
#include <random>
#include <vector>

#include "cloth.h"
#include "collision/plane.h"
#include "collision/sphere.h"

using namespace std;

Cloth::Cloth(double width, double height, int num_width_points,
             int num_height_points, float thickness) {
  this->width = width;
  this->height = height;
  this->num_width_points = num_width_points;
  this->num_height_points = num_height_points;
  this->thickness = thickness;

  buildGrid();
  buildClothMesh();
}

Cloth::~Cloth() {
  point_masses.clear();
  springs.clear();

  if (clothMesh) {
    delete clothMesh;
  }
}

/**
 * Attempt adding a spring between (x, y) and (x + dx, y + dy). Assuming (x, y)
 * is a valid coordinate, and add a spring iff (x + dx, y + dy) is also a valid
 * coordinate.
 */
void Cloth::tryAddSpring(int x, int y, int dx, int dy, e_spring_type spring_type) {
  if (0 <= x + dx && x + dx < num_width_points
      && 0 <= y + dy && y + dy < num_height_points) {
    springs.emplace_back(Spring(
            &point_masses[x * num_width_points + y],
            &point_masses[(x + dx) * num_width_points + (y + dy)],
            spring_type));
  }
}

void Cloth::buildGrid() {
  // (Part 1): Build a grid of masses and springs.

  // add the point masses in row-major order
  for (int j = 0; j < num_height_points; ++j)
    for (int i = 0; i < num_width_points; ++i) {
      Vector3D position;
      if (orientation == HORIZONTAL) {
        position = {width / num_width_points * i, 1, height / num_height_points * j};
      } else if (orientation == VERTICAL) {
        double z = ((double) rand() / RAND_MAX - 0.5) / 500;
        position = {width / num_width_points * i, height / num_height_points * j, z};
      }

      point_masses.emplace_back(PointMass(position, false));
    }

  // pin the pinned point masses
  for (vector<int> anchor : pinned)
    point_masses[anchor[0] * num_width_points + anchor[1]].pinned = true;

  // add the springs
  for (int i = 0; i < num_width_points; ++i)
    for (int j = 0; j < num_height_points; ++j) {
      // structural constraints
      tryAddSpring(i, j, -1, 0, STRUCTURAL);
      tryAddSpring(i, j, 0, -1, STRUCTURAL);

      // shearing constraints
      tryAddSpring(i, j, -1, -1, SHEARING);
      tryAddSpring(i, j, -1, 1, SHEARING);

      // bending constraints
      tryAddSpring(i, j, -2, 0, BENDING);
      tryAddSpring(i, j, -0, -2, BENDING);
    }
}

void Cloth::simulate(double frames_per_sec, double simulation_steps, ClothParameters *cp,
                     vector<Vector3D> external_accelerations,
                     vector<CollisionObject *> *collision_objects) {
  double mass = width * height * cp->density / num_width_points / num_height_points;
  double delta_t = 1.0f / frames_per_sec / simulation_steps;

  // (Part 2): Compute total force acting on each point mass.

  Vector3D externalForce = {};
  for (Vector3D &a : external_accelerations)
    externalForce += a;
  externalForce *= mass;

  // apply external forces
  for (PointMass &pm : point_masses)
    pm.forces = externalForce;

  // apply spring correction forces
  for (Spring &spring : springs) {
    // check if the given spring type is enabled
    switch (spring.spring_type) {
      case STRUCTURAL:
        if (!cp->enable_structural_constraints)
          continue;
      case SHEARING:
        if (!cp->enable_shearing_constraints)
          continue;
      case BENDING:
        if (!cp->enable_bending_constraints)
          continue;
    }

    // apply spring forces by Hooke's Law
    Vector3D dirA2B = spring.pm_b->position - spring.pm_a->position;
    double springForce = 0.2 * cp->ks * (dirA2B.norm() - spring.rest_length);
    spring.pm_a->forces += dirA2B.unit() * springForce;
    spring.pm_b->forces -= dirA2B.unit() * springForce;
  }

  // (Part 2): Use Verlet integration to compute new point mass positions

  for (PointMass &pm : point_masses)
    if (!pm.pinned) {
      Vector3D newPosition = pm.position + (1 - cp->damping / 100) * (pm.position - pm.last_position)
                             + pm.forces / mass * delta_t * delta_t;
      pm.last_position = pm.position;
      pm.position = newPosition;
    }

  // TODO (Part 4): Handle self-collisions.

  build_spatial_map();
  for (PointMass &pm : point_masses)
    self_collide(pm, simulation_steps);

  // (Part 3): Handle collisions with other primitives.

  for (PointMass &pm : point_masses)
    for (CollisionObject *obj : *collision_objects)
      obj->collide(pm);

  // (Part 2): Constrain the changes to be such that the spring does not change
  // in length more than 10% per timestep [Provot 1995].

  for (Spring &spring : springs) {
    // check if the given spring type is enabled
    switch (spring.spring_type) {
      case STRUCTURAL:
        if (!cp->enable_structural_constraints)
          continue;
      case SHEARING:
        if (!cp->enable_shearing_constraints)
          continue;
      case BENDING:
        if (!cp->enable_bending_constraints)
          continue;
    }

    // constraint position updates
    Vector3D dirA2B = spring.pm_b->position - spring.pm_a->position;
    double length = dirA2B.norm();
    if (length > 1.1 * spring.rest_length) {
      if (!spring.pm_a->pinned && !spring.pm_b->pinned) {
        spring.pm_a->position += 0.5 * dirA2B.unit() * (length - 1.1 * spring.rest_length);
        spring.pm_b->position -= 0.5 * dirA2B.unit() * (length - 1.1 * spring.rest_length);
      } else if (!spring.pm_a->pinned && spring.pm_b->pinned) {
        spring.pm_a->position += dirA2B.unit() * (length - 1.1 * spring.rest_length);
      } else if (spring.pm_a->pinned && !spring.pm_b->pinned) {
        spring.pm_b->position -= dirA2B.unit() * (length - 1.1 * spring.rest_length);
      }
    }
  }
}

void Cloth::build_spatial_map() {
  for (const auto &entry : map)
    entry.second->clear();

  // (Part 4): Build a spatial map out of all of the point masses.

  for (PointMass &pm : point_masses) {
    float hash = hash_position(pm.position);
    if (map.find(hash) == map.end())
      map.emplace(hash, new vector<PointMass *>());

    map[hash]->push_back(&pm);
  }
}

void Cloth::self_collide(PointMass &pm, double simulation_steps) {
  // (Part 4): Handle self-collision for a given point mass.

  if (pm.pinned)
    return;

  Vector3D correction = {};
  int count = 0;
  float hash = hash_position(pm.position);
  for (PointMass *candidate : *map[hash]) {
    Vector3D dir = pm.position - candidate->position;
    if (candidate != &pm && dir.norm() <= 2 * thickness) {
      correction += dir.unit() * (2 * thickness - dir.norm());
      count++;
    }
  }

  if (count)
    pm.position += correction / count / simulation_steps;
}

#define HASH_PRIME 997

float Cloth::hash_position(Vector3D pos) {
  // (Part 4): Hash a 3D position into a unique float identifier that represents membership in some 3D box volume.

  if (box == NULL) {
    if (orientation == HORIZONTAL) {
      box.x = 3 * (float) width / num_width_points;
      box.z = 3 * (float) height / num_height_points;
      box.y = fmax(box.x, box.z);
    } else if (orientation == VERTICAL) {
      box.x = 3 * (float) width / num_width_points;
      box.y = 3 * (float) height / num_height_points;
      box.z = fmax(box.x, box.y);
    }
  }

  double x = std::floor(pos.x / box.x);
  double y = std::floor(pos.y / box.y);
  double z = std::floor(pos.z / box.z);

  return (float) (x + y * HASH_PRIME + z * HASH_PRIME * HASH_PRIME);
}

///////////////////////////////////////////////////////
/// YOU DO NOT NEED TO REFER TO ANY CODE BELOW THIS ///
///////////////////////////////////////////////////////

void Cloth::reset() {
  PointMass *pm = &point_masses[0];
  for (int i = 0; i < point_masses.size(); i++) {
    pm->position = pm->start_position;
    pm->last_position = pm->start_position;
    pm++;
  }
}

void Cloth::buildClothMesh() {
  if (point_masses.size() == 0) return;

  ClothMesh *clothMesh = new ClothMesh();
  vector<Triangle *> triangles;

  // Create vector of triangles
  for (int y = 0; y < num_height_points - 1; y++) {
    for (int x = 0; x < num_width_points - 1; x++) {
      PointMass *pm = &point_masses[y * num_width_points + x];
      // Get neighboring point masses:
      /*                      *
       * pm_A -------- pm_B   *
       *             /        *
       *  |         /   |     *
       *  |        /    |     *
       *  |       /     |     *
       *  |      /      |     *
       *  |     /       |     *
       *  |    /        |     *
       *      /               *
       * pm_C -------- pm_D   *
       *                      *
       */

      float u_min = x;
      u_min /= num_width_points - 1;
      float u_max = x + 1;
      u_max /= num_width_points - 1;
      float v_min = y;
      v_min /= num_height_points - 1;
      float v_max = y + 1;
      v_max /= num_height_points - 1;

      PointMass *pm_A = pm;
      PointMass *pm_B = pm + 1;
      PointMass *pm_C = pm + num_width_points;
      PointMass *pm_D = pm + num_width_points + 1;

      Vector3D uv_A = Vector3D(u_min, v_min, 0);
      Vector3D uv_B = Vector3D(u_max, v_min, 0);
      Vector3D uv_C = Vector3D(u_min, v_max, 0);
      Vector3D uv_D = Vector3D(u_max, v_max, 0);


      // Both triangles defined by vertices in counter-clockwise orientation
      triangles.push_back(new Triangle(pm_A, pm_C, pm_B,
                                       uv_A, uv_C, uv_B));
      triangles.push_back(new Triangle(pm_B, pm_C, pm_D,
                                       uv_B, uv_C, uv_D));
    }
  }

  // For each triangle in row-order, create 3 edges and 3 internal halfedges
  for (int i = 0; i < triangles.size(); i++) {
    Triangle *t = triangles[i];

    // Allocate new halfedges on heap
    Halfedge *h1 = new Halfedge();
    Halfedge *h2 = new Halfedge();
    Halfedge *h3 = new Halfedge();

    // Allocate new edges on heap
    Edge *e1 = new Edge();
    Edge *e2 = new Edge();
    Edge *e3 = new Edge();

    // Assign a halfedge pointer to the triangle
    t->halfedge = h1;

    // Assign halfedge pointers to point masses
    t->pm1->halfedge = h1;
    t->pm2->halfedge = h2;
    t->pm3->halfedge = h3;

    // Update all halfedge pointers
    h1->edge = e1;
    h1->next = h2;
    h1->pm = t->pm1;
    h1->triangle = t;

    h2->edge = e2;
    h2->next = h3;
    h2->pm = t->pm2;
    h2->triangle = t;

    h3->edge = e3;
    h3->next = h1;
    h3->pm = t->pm3;
    h3->triangle = t;
  }

  // Go back through the cloth mesh and link triangles together using halfedge
  // twin pointers

  // Convenient variables for math
  int num_height_tris = (num_height_points - 1) * 2;
  int num_width_tris = (num_width_points - 1) * 2;

  bool topLeft = true;
  for (int i = 0; i < triangles.size(); i++) {
    Triangle *t = triangles[i];

    if (topLeft) {
      // Get left triangle, if it exists
      if (i % num_width_tris != 0) { // Not a left-most triangle
        Triangle *temp = triangles[i - 1];
        t->pm1->halfedge->twin = temp->pm3->halfedge;
      } else {
        t->pm1->halfedge->twin = nullptr;
      }

      // Get triangle above, if it exists
      if (i >= num_width_tris) { // Not a top-most triangle
        Triangle *temp = triangles[i - num_width_tris + 1];
        t->pm3->halfedge->twin = temp->pm2->halfedge;
      } else {
        t->pm3->halfedge->twin = nullptr;
      }

      // Get triangle to bottom right; guaranteed to exist
      Triangle *temp = triangles[i + 1];
      t->pm2->halfedge->twin = temp->pm1->halfedge;
    } else {
      // Get right triangle, if it exists
      if (i % num_width_tris != num_width_tris - 1) { // Not a right-most triangle
        Triangle *temp = triangles[i + 1];
        t->pm3->halfedge->twin = temp->pm1->halfedge;
      } else {
        t->pm3->halfedge->twin = nullptr;
      }

      // Get triangle below, if it exists
      if (i + num_width_tris - 1 < 1.0f * num_width_tris * num_height_tris / 2.0f) { // Not a bottom-most triangle
        Triangle *temp = triangles[i + num_width_tris - 1];
        t->pm2->halfedge->twin = temp->pm3->halfedge;
      } else {
        t->pm2->halfedge->twin = nullptr;
      }

      // Get triangle to top left; guaranteed to exist
      Triangle *temp = triangles[i - 1];
      t->pm1->halfedge->twin = temp->pm2->halfedge;
    }

    topLeft = !topLeft;
  }

  clothMesh->triangles = triangles;
  this->clothMesh = clothMesh;
}
