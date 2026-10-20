// SPDX-License-Identifier: GPL-3.0-or-later
// VR cockpit overlay geometry: what the synthetic wheel, handlebar and hands
// build in the seated frame, without a GPU.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "gfx/cockpit.hpp"

namespace {
using aurora::gfx::cockpit::V;
using aurora::gfx::cockpit::Vertex;

bool all_finite(const std::vector<Vertex>& vertices) {
  for (const auto& vertex : vertices) {
    for (float value : vertex.position) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      if ((bits & 0x7f800000u) == 0x7f800000u) {
        return false;
      }
    }
  }
  return true;
}

void set_identity(float (&matrix)[12], V translation) {
  const auto identity = aurora::gfx::cockpit::identity();
  std::memcpy(matrix, identity.data(), sizeof(matrix));
  matrix[3] = translation[0];
  matrix[7] = translation[1];
  matrix[11] = translation[2];
}

class CockpitGeometry : public ::testing::Test {
protected:
  void SetUp() override { clear_meshes(); }
  void TearDown() override { clear_meshes(); }
  static void clear_meshes() {
    std::lock_guard lock(aurora::gfx::cockpit::meshMutex);
    aurora::gfx::cockpit::meshes = {};
  }
};

TEST_F(CockpitGeometry, NativeWheelWithoutHandsDrawsNothing) {
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  EXPECT_TRUE(aurora::gfx::cockpit::geometry(cockpit).empty());
}

TEST_F(CockpitGeometry, SyntheticKartWheelSitsOnItsRim) {
  AuroraCockpit cockpit{};
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  // The rim, spokes and hub stay within the 0.18 m wheel plus its tube, around
  // the wheel centre the input side uses (steering_wheel.h).
  for (const auto& vertex : vertices) {
    const float x = vertex.position[0];
    const float y = vertex.position[1] + 0.30f;
    EXPECT_LE(std::hypot(x, y), 0.18f + 0.02f);
    EXPECT_NEAR(vertex.position[2], -0.42f, 0.04f);
  }
}

TEST_F(CockpitGeometry, SyntheticWheelTurnsWithTheAngle) {
  AuroraCockpit cockpit{};
  const auto straight = aurora::gfx::cockpit::geometry(cockpit);
  cockpit.wheelAngle = 0.5f;
  const auto turned = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_EQ(straight.size(), turned.size());
  bool moved = false;
  for (size_t i = 0; i < straight.size() && !moved; ++i) {
    moved = std::abs(straight[i].position[0] - turned[i].position[0]) > 1e-3f;
  }
  EXPECT_TRUE(moved);
}

TEST_F(CockpitGeometry, SyntheticHandlebarFollowsItsFrame) {
  AuroraCockpit cockpit{};
  cockpit.bike = true;
  cockpit.handlebarRadius = 0.25f;
  // Bar axis along seat +X, centred 0.3 m down and 0.42 m ahead.
  const float pose[12]{1, 0, 0, 0, 0, 0, 1, -0.3f, 0, -1, 0, -0.42f};
  std::memcpy(cockpit.seatFromHandlebar, pose, sizeof(pose));
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  float minX = 1e9f;
  float maxX = -1e9f;
  for (const auto& vertex : vertices) {
    minX = std::min(minX, vertex.position[0]);
    maxX = std::max(maxX, vertex.position[0]);
  }
  EXPECT_NEAR(minX, -0.25f, 0.03f);
  EXPECT_NEAR(maxX, 0.25f, 0.03f);
}

TEST_F(CockpitGeometry, TrackedHandDrawsAGloveAtItsGrip) {
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[1].tracked = true;
  cockpit.hands[1].squeeze = 1.0f;
  set_identity(cockpit.hands[1].seatFromGrip, {0.2f, -0.3f, -0.4f});
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  for (const auto& vertex : vertices) {
    EXPECT_LT(std::abs(vertex.position[0] - 0.2f), 0.15f);
    EXPECT_LT(std::abs(vertex.position[1] + 0.3f), 0.15f);
    EXPECT_LT(std::abs(vertex.position[2] + 0.4f), 0.15f);
  }
}

// The grip space OpenXR defines: -Z up the curled fingers' tube towards the
// thumb, +X out of the palm. So the fingers run along Y (+Y on the right hand,
// -Y on the left) and close towards +X, never out of the back of the hand.
TEST_F(CockpitGeometry, GloveFingersRunAlongTheHandAndCloseIntoThePalm) {
  // Both grips carry the same orientation when the hands hold a wheel symmetrically, so the fingers
  // run along -Y on both and it is the palm side that mirrors: +X on the left hand, -X on the right.
  // Building the right hand's fingers on +Y instead pointed them at the player (PC, 2026-09-23).
  for (int side = 0; side < 2; ++side) {
    const float palmSide = side == 0 ? 1.0f : -1.0f;
    const auto build = [&](float squeeze) {
      AuroraCockpit cockpit{};
      cockpit.nativeWheel = true;
      cockpit.hands[side].tracked = true;
      cockpit.hands[side].squeeze = squeeze;
      set_identity(cockpit.hands[side].seatFromGrip, {0.0f, 0.0f, 0.0f});
      return aurora::gfx::cockpit::geometry(cockpit);
    };
    struct Extent {
      float reach = 0.0f;   // furthest along the fingers
      float palm = 0.0f;    // furthest towards the palm's normal
      float back = 0.0f;    // furthest out of the back of the hand
      float across = 0.0f;  // furthest across the knuckles
    };
    const auto measure = [&](const std::vector<Vertex>& vertices) {
      Extent e{};
      for (const auto& vertex : vertices) {
        e.reach = std::max(e.reach, -vertex.position[1]);
        e.palm = std::max(e.palm, vertex.position[0] * palmSide);
        e.back = std::min(e.back, vertex.position[0] * palmSide);
        e.across = std::max(e.across, std::abs(vertex.position[2]));
      }
      return e;
    };
    const auto open = measure(build(0.0f));
    const auto closed = measure(build(1.0f));
    EXPECT_GT(open.reach, 0.09f) << "open fingers reach along the hand, side " << side;
    EXPECT_LT(open.palm, 0.05f) << "an open hand is flat, side " << side;
    EXPECT_LT(closed.reach, open.reach - 0.02f) << "closing shortens the reach, side " << side;
    EXPECT_GT(closed.palm, open.palm + 0.02f) << "closing moves the fingers into the palm, side " << side;
    EXPECT_GT(closed.back, -0.03f) << "fingers never bend out of the back of the hand, side " << side;
    EXPECT_LT(closed.across, 0.07f) << "fingers stay across the knuckles, side " << side;
  }
}

TEST_F(CockpitGeometry, RuntimeFingersCurlTowardPalmForSqueezeAndWheelGrab) {
  using namespace aurora::gfx::cockpit;
  // OpenXR joint space: -Z runs toward the fingertip, +Y out of the back
  // of the hand, for BOTH hands. Mirror positions, not the curl direction.
  for (int side = 0; side < 2; ++side) {
    SCOPED_TRACE(side);
    HandMesh mesh;
    mesh.parents.fill(1);
    mesh.parents[1] = -1;
    const float rootPose[7]{0, 0, 0.70710678f, 0.70710678f, 0.12f, -0.08f, 0.03f};
    const M root = from_pose(rootPose);
    mesh.bind.fill(root);
    const int bases[]{2, 6, 11, 16, 21};
    for (int finger = 0; finger < 5; ++finger) {
      const int base = bases[finger];
      const int count = finger == 0 ? 4 : 5;
      for (int bone = 0; bone < count; ++bone) {
        M bind = identity();
        bind[3] = (side == 0 ? -1.0f : 1.0f) * (finger - 2) * 0.018f;
        bind[11] = -0.025f * (bone + 1);
        mesh.bind[base + bone] = compose(root, bind);
        mesh.parents[base + bone] = bone == 0 ? 1 : base + bone - 1;
      }
    }
    for (int j = 0; j < 26; ++j) mesh.inverseBind[j] = inverse(mesh.bind[j]);
    // A tiny triangle rigidly weighted to each joint, including each fingertip.
    for (int j = 0; j < 26; ++j) {
      for (V offset : {V{0, 0, 0}, V{0.001f, 0, 0}, V{0, 0, 0.001f}}) {
        AuroraVRHandVertex vertex{};
        const V p = point(mesh.bind[j].data(), offset);
        std::memcpy(vertex.position, p.data(), sizeof(vertex.position));
        vertex.joints[0] = j;
        vertex.weights[0] = 1;
        mesh.indices.push_back(static_cast<uint16_t>(mesh.vertices.size()));
        mesh.vertices.push_back(vertex);
      }
    }
    AuroraCockpitHand hand{};
    set_identity(hand.seatFromGrip, {0, 0, 0});
    const auto build = [&](float squeeze, bool held) {
      hand.squeeze = squeeze;
      hand.held = held;
      std::vector<Vertex> vertices;
      runtime_hand(vertices, hand, mesh);
      return vertices;
    };
    const auto open = build(0, false);
    for (int j = 0; j < 26; ++j) {
      const V bind = point(mesh.inverseBind[1].data(), point(mesh.bind[j].data(), {0, 0, 0}));
      for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(open[j * 3].position[axis], bind[axis] + (axis == 2 ? 0.04f : 0), 1e-6f);
    }
    for (const auto& closed : {build(0.5f, false), build(1, false), build(0, true)}) {
      ASSERT_TRUE(all_finite(closed));
      for (int tip : {5, 10, 15, 20, 25}) {
        EXPECT_LT(closed[tip * 3].position[1], open[tip * 3].position[1] - 0.005f)
            << "fingertip must move toward palm (-Y), joint " << tip;
        EXPECT_GT(closed[tip * 3].position[2], open[tip * 3].position[2])
            << "curl must shorten finger reach, joint " << tip;
      }
      for (int rigid : {0, 1, 6, 11, 16, 21})
        for (int axis = 0; axis < 3; ++axis)
          EXPECT_NEAR(closed[rigid * 3].position[axis], open[rigid * 3].position[axis], 1e-6f);
    }
  }
}

TEST_F(CockpitGeometry, RuntimeHandMeshIsSkinnedWithoutNans) {
  using namespace aurora::gfx::cockpit;
  auto mesh = std::make_shared<HandMesh>();
  // A 26-joint chain, each joint 1 cm past its parent; one triangle on the tip.
  for (int j = 0; j < 26; ++j) {
    mesh->bind[j] = identity();
    mesh->bind[j][11] = -0.01f * float(j);
    mesh->inverseBind[j] = inverse(mesh->bind[j]);
    mesh->parents[j] = j - 1;
  }
  for (int i = 0; i < 3; ++i) {
    AuroraVRHandVertex vertex{};
    vertex.position[0] = 0.01f * float(i);
    vertex.position[2] = -0.25f;
    vertex.joints[0] = 25;
    vertex.joints[1] = vertex.joints[2] = vertex.joints[3] = -1;
    vertex.weights[0] = 1.0f;
    mesh->vertices.push_back(vertex);
    mesh->indices.push_back(uint16_t(i));
  }
  {
    std::lock_guard lock(meshMutex);
    meshes[0] = mesh;
  }
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[0].tracked = true;
  cockpit.hands[0].held = true;
  set_identity(cockpit.hands[0].seatFromGrip, {-0.2f, -0.3f, -0.4f});
  const auto vertices = geometry(cockpit);
  ASSERT_EQ(vertices.size(), 3u) << "the runtime mesh replaces the glove";
  EXPECT_TRUE(all_finite(vertices));
}

// A 26-joint mesh with its bind poses turned and spread out, and a tiny
// triangle rigidly on each joint, like the runtime's hand mesh.
std::shared_ptr<aurora::gfx::cockpit::HandMesh> jointed_mesh() {
  using namespace aurora::gfx::cockpit;
  auto mesh = std::make_shared<HandMesh>();
  for (int j = 0; j < 26; ++j) {
    const float angle = 0.2f * float(j);
    const float pose[7]{0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f), 0.01f * float(j % 5),
                        -0.02f * float(j / 5), -0.015f * float(j)};
    mesh->bind[j] = from_pose(pose);
    mesh->inverseBind[j] = inverse(mesh->bind[j]);
    mesh->parents[j] = j - 1;
    for (V offset : {V{0, 0, 0}, V{0.002f, 0, 0}, V{0, 0, 0.002f}}) {
      AuroraVRHandVertex vertex{};
      const V p = point(mesh->bind[j].data(), offset);
      std::memcpy(vertex.position, p.data(), sizeof(vertex.position));
      vertex.joints[0] = int16_t(j);
      vertex.joints[1] = vertex.joints[2] = vertex.joints[3] = -1;
      vertex.weights[0] = 1;
      mesh->indices.push_back(uint16_t(mesh->vertices.size()));
      mesh->vertices.push_back(vertex);
    }
  }
  return mesh;
}

// Tracked joints: each at `seatFromBind` times its bind pose, so the whole
// hand is that rigid motion of the mesh.
void set_tracked_joints(AuroraCockpitHand& hand, const aurora::gfx::cockpit::HandMesh& mesh,
                        const aurora::gfx::cockpit::M& seatFromBind) {
  using namespace aurora::gfx::cockpit;
  hand.tracked = true;
  hand.jointsValid = true;
  for (int j = 0; j < 26; ++j) {
    const M joint = compose(seatFromBind, mesh.bind[j]);
    std::memcpy(hand.seatFromJoint[j], joint.data(), sizeof(hand.seatFromJoint[j]));
    hand.jointRadii[j] = 0.008f;
  }
}

aurora::gfx::cockpit::M rigid_motion() {
  using namespace aurora::gfx::cockpit;
  const float pose[7]{0.2f, -0.3f, 0.1f, 0.9273618f, 0.15f, -0.25f, -0.35f};
  return from_pose(pose);
}

TEST_F(CockpitGeometry, TrackedJointsSkinTheRuntimeMeshRigidly) {
  using namespace aurora::gfx::cockpit;
  const auto mesh = jointed_mesh();
  {
    std::lock_guard lock(meshMutex);
    meshes[1] = mesh;
  }
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  const M motion = rigid_motion();
  set_tracked_joints(cockpit.hands[1], *mesh, motion);
  const auto vertices = geometry(cockpit);
  ASSERT_EQ(vertices.size(), mesh->vertices.size());
  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto& bindVertex = mesh->vertices[mesh->indices[i]].position;
    const V expected = point(motion.data(), {bindVertex[0], bindVertex[1], bindVertex[2]});
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_NEAR(vertices[i].position[axis], expected[axis], 1e-5f) << "vertex " << i << " axis " << axis;
  }
}

TEST_F(CockpitGeometry, TrackedJointsIgnoreSqueezeHeldAndGrip) {
  using namespace aurora::gfx::cockpit;
  const auto mesh = jointed_mesh();
  {
    std::lock_guard lock(meshMutex);
    meshes[0] = mesh;
  }
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  set_tracked_joints(cockpit.hands[0], *mesh, rigid_motion());
  set_identity(cockpit.hands[0].seatFromGrip, {0, 0, 0});
  const auto reference = geometry(cockpit);
  cockpit.hands[0].squeeze = 1.0f;
  cockpit.hands[0].held = true;
  set_identity(cockpit.hands[0].seatFromGrip, {0.5f, 0.5f, 0.5f});
  const auto moved = geometry(cockpit);
  ASSERT_EQ(moved.size(), reference.size());
  for (size_t i = 0; i < moved.size(); ++i)
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_EQ(moved[i].position[axis], reference[i].position[axis]);
}

float distance_to_segment(V p, V a, V b) {
  using namespace aurora::gfx::cockpit;
  const V ab = sub(b, a);
  const float length = dot(ab, ab);
  const float t = length > 0 ? std::clamp(dot(sub(p, a), ab) / length, 0.0f, 1.0f) : 0.0f;
  const V d = sub(p, add(a, mul(ab, t)));
  return std::sqrt(dot(d, d));
}

TEST_F(CockpitGeometry, TrackedJointsWithoutAMeshDrawASkeletonOnTheBones) {
  using namespace aurora::gfx::cockpit;
  const auto mesh = jointed_mesh(); // only for joint poses; no runtime mesh is set
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  set_tracked_joints(cockpit.hands[0], *mesh, rigid_motion());
  const auto& hand = cockpit.hands[0];
  const auto at = [&](int j) { return V{hand.seatFromJoint[j][3], hand.seatFromJoint[j][7], hand.seatFromJoint[j][11]}; };
  const auto vertices = geometry(cockpit);
  ASSERT_FALSE(vertices.empty());
  ASSERT_TRUE(all_finite(vertices));
  constexpr int metacarpal[5]{2, 6, 11, 16, 21}, tip[5]{5, 10, 15, 20, 25};
  for (const auto& vertex : vertices) {
    float nearest = 1e9f;
    for (int finger = 0; finger < 5; ++finger) {
      nearest = std::min(nearest, distance_to_segment(vertex.position, at(1), at(metacarpal[finger])));
      for (int j = metacarpal[finger]; j < tip[finger]; ++j)
        nearest = std::min(nearest, distance_to_segment(vertex.position, at(j), at(j + 1)));
    }
    const V fromPalm = sub(vertex.position, at(0));
    nearest = std::min(nearest, std::sqrt(dot(fromPalm, fromPalm)));
    EXPECT_LT(nearest, 0.031f) << "every skeleton vertex lies on a bone, a joint or the palm";
  }
}

TEST_F(CockpitGeometry, NonFiniteJointsFallBackToTheGrip) {
  using namespace aurora::gfx::cockpit;
  const auto mesh = jointed_mesh();
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  set_tracked_joints(cockpit.hands[1], *mesh, rigid_motion());
  set_identity(cockpit.hands[1].seatFromGrip, {0.2f, -0.3f, -0.4f});
  cockpit.hands[1].seatFromJoint[7][5] = std::numeric_limits<float>::quiet_NaN();
  const auto broken = geometry(cockpit);
  cockpit.hands[1].jointsValid = false;
  const auto grip = geometry(cockpit);
  ASSERT_TRUE(all_finite(broken));
  ASSERT_EQ(broken.size(), grip.size()) << "a non-finite joint draws the glove at the grip";
  for (size_t i = 0; i < broken.size(); ++i)
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_EQ(broken[i].position[axis], grip[i].position[axis]);
}

// The character body draws the hand with the game's own model: the overlay
// draws none there, whichever way it would have drawn it, but keeps the other
// hand and the wheel.
TEST_F(CockpitGeometry, CharacterHandDrawsNoHand) {
  using namespace aurora::gfx::cockpit;
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[1].tracked = true;
  cockpit.hands[1].squeeze = 1.0f;
  set_identity(cockpit.hands[1].seatFromGrip, {0.2f, -0.3f, -0.4f});
  ASSERT_FALSE(geometry(cockpit).empty()) << "a tracked hand draws its glove";
  cockpit.hands[1].characterHand = true;
  EXPECT_TRUE(geometry(cockpit).empty()) << "no glove where the character's hand is";

  // Nor the runtime mesh, from the grip or from tracked joints, nor the joint skeleton.
  const auto mesh = jointed_mesh();
  set_tracked_joints(cockpit.hands[1], *mesh, rigid_motion());
  EXPECT_TRUE(geometry(cockpit).empty()) << "no joint skeleton";
  {
    std::lock_guard lock(meshMutex);
    meshes[1] = mesh;
  }
  EXPECT_TRUE(geometry(cockpit).empty()) << "no tracked runtime mesh";
  cockpit.hands[1].jointsValid = false;
  EXPECT_TRUE(geometry(cockpit).empty()) << "no runtime mesh at the grip";
  cockpit.hands[1].characterHand = false;
  EXPECT_EQ(geometry(cockpit).size(), mesh->vertices.size()) << "the same hand drawn by the overlay";

  // The other hand is still the overlay's.
  cockpit.hands[1].characterHand = true;
  cockpit.hands[0].tracked = true;
  set_identity(cockpit.hands[0].seatFromGrip, {-0.2f, -0.3f, -0.4f});
  const auto left = geometry(cockpit);
  ASSERT_FALSE(left.empty());
  for (const auto& vertex : left) {
    EXPECT_LT(vertex.position[0], 0.0f) << "only the left glove";
  }
}

TEST_F(CockpitGeometry, CharacterHandsKeepTheSyntheticWheel) {
  AuroraCockpit wheel{};
  const auto expected = aurora::gfx::cockpit::geometry(wheel);
  ASSERT_FALSE(expected.empty());
  AuroraCockpit cockpit{};
  for (int side = 0; side < 2; ++side) {
    cockpit.hands[side].tracked = true;
    cockpit.hands[side].held = true;
    cockpit.hands[side].characterHand = true;
    set_identity(cockpit.hands[side].seatFromGrip, {side == 0 ? -0.18f : 0.18f, -0.3f, -0.42f});
  }
  const auto vertices = aurora::gfx::cockpit::geometry(cockpit);
  ASSERT_EQ(vertices.size(), expected.size()) << "the wheel and nothing else";
  for (size_t i = 0; i < vertices.size(); ++i)
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_EQ(vertices[i].position[axis], expected[i].position[axis]);
}

// A hand the character draws gets no glove from the overlay, which still runs
// for it, so what the hand carries is drawn on it (cockpit_item::render).
TEST_F(CockpitGeometry, CharacterHandGetsNoGloveButStillCarries) {
  using namespace aurora::gfx;
  AuroraCockpit cockpit{};
  cockpit.nativeWheel = true;
  cockpit.hands[0].tracked = true;
  set_identity(cockpit.hands[0].seatFromGrip, {-0.2f, -0.3f, -0.4f});
  const auto build = [&] {
    std::vector<cockpit::Vertex> vertices;
    cockpit::build_geometry(cockpit, vertices);
    return vertices.size();
  };
  EXPECT_GT(build(), 0u) << "the overlay's glove";
  cockpit.hands[0].characterHand = true;
  EXPECT_EQ(build(), 0u) << "no glove on the character's hand";
  EXPECT_TRUE(cockpit::carries_hand(cockpit)) << "the overlay still draws for the item's hand";
  cockpit.hands[0].tracked = false;
  EXPECT_FALSE(cockpit::carries_hand(cockpit));
}

// The held item grows into the hand over kItemAnimationSeconds and shrinks away
// when it goes; one turned round midway carries on from the size it had reached.
TEST(CockpitItemAnimation, GrowsInAndShrinksAway) {
  using namespace aurora::gfx::cockpit_item;
  using Clock = ItemAnimation::Clock;
  ItemAnimation state;
  const auto step = [&](const AuroraCockpitItem& current, double seconds, bool hasModel = true) {
    const auto at = Clock::time_point{} +
                    std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1000.0 + seconds));
    return step_item_animation(state, current, hasModel, at);
  };
  AuroraCockpitItem item{};
  item.valid = true;
  item.id = 16;
  item.count = 3;
  item.hand = 1;
  item.raceGeneration = 7;
  const AuroraCockpitItem none{};

  // Times as fractions of the animation, stepped well inside the 250 ms after
  // which a gap in drawing counts as first person having been off.
  const double d = kItemAnimationSeconds;
  auto shown = step(item, 0.0);
  ASSERT_TRUE(shown.draw);
  EXPECT_EQ(shown.id, 16);
  EXPECT_EQ(shown.hand, 1);
  EXPECT_EQ(shown.copies, 3u) << "a triple shows three";
  for (uint32_t copy = 0; copy < 3; ++copy) EXPECT_NEAR(shown.scale[copy], 0.0f, 1e-4f) << "it starts from nothing";
  step(item, 0.2 * d);
  EXPECT_NEAR(step(item, 0.5 * d).scale[0], 0.5f, 1e-3f) << "halfway at half the time";
  step(item, 0.8 * d);
  EXPECT_NEAR(step(item, d).scale[0], 1.0f, 1e-4f) << "full size when the time is up";
  EXPECT_NEAR(step(item, 1.2 * d).scale[0], 1.0f, 1e-4f);

  // Used: the same item keeps being drawn on the same hand, shrinking.
  const double used = 1.4 * d;
  shown = step(none, used);
  ASSERT_TRUE(shown.draw);
  EXPECT_EQ(shown.id, 16);
  EXPECT_EQ(shown.hand, 1);
  EXPECT_EQ(shown.copies, 3u) << "all three shrink";
  EXPECT_NEAR(shown.scale[2], 1.0f, 1e-4f) << "the shrink starts from full size";
  step(none, used + 0.2 * d);
  EXPECT_NEAR(step(none, used + 0.5 * d).scale[2], 0.5f, 1e-3f);
  step(none, used + 0.8 * d);
  EXPECT_FALSE(step(none, used + 1.1 * d).draw) << "gone once the time is up";

  // Gone at 40% of its growth: it shrinks from the size it had reached, and
  // back again it grows from where the shrink left it.
  item.id = 5;
  step(item, 2.0);
  step(item, 2.0 + 0.2 * d);
  const float reached = step(item, 2.0 + 0.4 * d).scale[0];
  EXPECT_NEAR(reached, item_ease(0.4f), 1e-3f);
  EXPECT_NEAR(step(none, 2.0 + 0.4 * d).scale[0], reached, 1e-3f) << "no jump when it starts to shrink";
  const float shrunk = step(none, 2.0 + 0.6 * d).scale[0];
  EXPECT_LT(shrunk, reached);
  EXPECT_NEAR(step(item, 2.0 + 0.6 * d).scale[0], shrunk, 1e-3f) << "no jump when it grows again";

  // Not drawn for over a second (the game's camera) while the item went: it is
  // not shrunk late when first person comes back.
  step(item, 3.0);
  EXPECT_FALSE(step(none, 4.5).draw);

  // A new race's item grows in again even when it is the same item.
  for (double t = 6.0; t <= 6.0 + 1.2 * d; t += 0.2 * d) step(item, t);
  EXPECT_NEAR(step(item, 6.0 + 1.5 * d).scale[0], 1.0f, 1e-4f);
  item.raceGeneration = 8;
  EXPECT_NEAR(step(item, 6.0 + 1.6 * d).scale[0], 0.0f, 1e-4f);

  ItemAnimation fresh;
  EXPECT_FALSE(step_item_animation(fresh, item, false, Clock::now()).draw) << "an item without a model draws nothing";
}

// A triple shows its items as copies half the item's size, three in a
// triangle; one used shrinks away where it stands while the others close up.
TEST(CockpitItemAnimation, TripleCopiesCloseUpAsTheyAreUsed) {
  using namespace aurora::gfx::cockpit_item;
  using Clock = ItemAnimation::Clock;
  ItemAnimation state;
  const auto step = [&](const AuroraCockpitItem& current, double seconds) {
    const auto at = Clock::time_point{} +
                    std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1000.0 + seconds));
    return step_item_animation(state, current, true, at);
  };
  const auto expect_at = [](const ItemDraw& shown, uint32_t copy, std::array<float, 3> where, const char* what) {
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(shown.position[copy][axis], where[axis], 1e-3f) << what;
  };
  const double d = kItemAnimationSeconds;
  AuroraCockpitItem item{};
  item.valid = true;
  item.id = 16;
  item.count = 3;
  for (double t = 0.0; t <= 1.2 * d; t += 0.2 * d) step(item, t);
  auto shown = step(item, 1.4 * d);
  ASSERT_EQ(shown.copies, 3u);
  EXPECT_FLOAT_EQ(shown.share, kTripleCopyShare);
  expect_at(shown, 0, {-0.5f, 0.12f, -0.3f}, "back left");
  expect_at(shown, 1, {0.5f, 0.12f, -0.3f}, "back right");
  expect_at(shown, 2, {0.0f, 0.0f, 0.3f}, "in front, nearest the player");
  for (uint32_t copy = 0; copy < 3; ++copy) EXPECT_NEAR(shown.scale[copy], 1.0f, 1e-4f);

  // One used: the front one shrinks where it is, the back two come forward
  // and down to stand side by side.
  item.count = 2;
  const double used = 1.6 * d;
  shown = step(item, used);
  ASSERT_EQ(shown.copies, 3u);
  expect_at(shown, 0, {-0.5f, 0.12f, -0.3f}, "no jump when it sets off");
  EXPECT_NEAR(shown.scale[2], 1.0f, 1e-4f);
  step(item, used + 0.2 * d);
  shown = step(item, used + 0.5 * d);
  ASSERT_EQ(shown.copies, 3u);
  expect_at(shown, 0, {-0.5f, 0.06f, -0.15f}, "halfway at half the time");
  expect_at(shown, 2, {0.0f, 0.0f, 0.3f}, "the used one stays put");
  EXPECT_NEAR(shown.scale[2], 0.5f, 1e-3f);
  EXPECT_NEAR(shown.scale[0], 1.0f, 1e-4f) << "the others keep their size";
  step(item, used + 0.8 * d);
  shown = step(item, used + 1.1 * d);
  ASSERT_EQ(shown.copies, 2u);
  expect_at(shown, 0, {-0.5f, 0.0f, 0.0f}, "two side by side");
  expect_at(shown, 1, {0.5f, 0.0f, 0.0f}, "two side by side");

  // Another: the right one goes, the left one comes to the middle.
  item.count = 1;
  const double again = used + 1.3 * d;
  step(item, again);
  step(item, again + 0.5 * d);
  shown = step(item, again + 1.1 * d);
  ASSERT_EQ(shown.copies, 1u);
  expect_at(shown, 0, {0.0f, 0.0f, 0.0f}, "the last one in the middle");
  EXPECT_FLOAT_EQ(shown.share, kTripleCopyShare) << "still one of the three, not a whole item";

  // A single item is the whole size, in the middle.
  AuroraCockpitItem single{};
  single.valid = true;
  single.id = 4;
  single.count = 1;
  ItemAnimation fresh;
  shown = step_item_animation(fresh, single, true, Clock::now());
  ASSERT_EQ(shown.copies, 1u);
  EXPECT_FLOAT_EQ(shown.share, 1.0f);
  expect_at(shown, 0, {0.0f, 0.0f, 0.0f}, "a single item");

  // Not drawn for a while (the game's camera) while one was used: when first
  // person comes back, the used one is gone and the others already moved.
  item.count = 3;
  item.raceGeneration = 2;
  for (double t = 10.0; t <= 10.0 + 1.2 * d; t += 0.2 * d) step(item, t);
  item.count = 2;
  shown = step(item, 12.0);
  ASSERT_EQ(shown.copies, 2u);
  expect_at(shown, 0, {-0.5f, 0.0f, 0.0f}, "no late glide");
  expect_at(shown, 1, {0.5f, 0.0f, 0.0f}, "no late glide");
  EXPECT_NEAR(shown.scale[1], 1.0f, 1e-4f);
}

// Each use of the golden mushroom squeezes it to three quarters and back; a
// use before that is done (the button mashed) snaps it back to full size and
// starts again.
TEST(CockpitItemAnimation, GoldenMushroomSqueezesOnEachUse) {
  using namespace aurora::gfx::cockpit_item;
  using Clock = ItemAnimation::Clock;
  ItemAnimation state;
  const auto step = [&](const AuroraCockpitItem& current, double seconds) {
    const auto at = Clock::time_point{} +
                    std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1000.0 + seconds));
    return step_item_animation(state, current, true, at).scale[0];
  };
  const double d = kItemAnimationSeconds;
  AuroraCockpitItem golden{};
  golden.valid = true;
  golden.id = 10;
  golden.count = 1;
  for (double t = 0.0; t <= 1.2 * d; t += 0.2 * d) step(golden, t);
  EXPECT_NEAR(step(golden, 1.4 * d), 1.0f, 1e-4f) << "grown, not used yet";

  golden.useMark = 438; // the first use
  const double use = 1.6 * d;
  EXPECT_NEAR(step(golden, use), 1.0f, 1e-4f) << "the squeeze starts from full size";
  step(golden, use + 0.2 * d);
  EXPECT_NEAR(step(golden, use + 0.5 * d), 0.875f, 1e-3f) << "halfway down at half the time";
  step(golden, use + 0.8 * d);
  EXPECT_NEAR(step(golden, use + d), kUsePulseScale, 1e-4f) << "three quarters";
  step(golden, use + 1.2 * d);
  EXPECT_NEAR(step(golden, use + 1.5 * d), 0.875f, 1e-3f) << "on its way back";
  step(golden, use + 1.8 * d);
  EXPECT_NEAR(step(golden, use + 2.1 * d), 1.0f, 1e-4f) << "full size again";

  golden.useMark = 401;
  const double mash = use + 2.4 * d;
  step(golden, mash);
  step(golden, mash + 0.2 * d);
  EXPECT_NEAR(step(golden, mash + 0.5 * d), 0.875f, 1e-3f);
  EXPECT_NEAR(step(golden, mash + 0.5 * d), 0.875f, 1e-3f) << "the same use seen again goes on";
  golden.useMark = 390;
  EXPECT_NEAR(step(golden, mash + 0.6 * d), 1.0f, 1e-4f) << "mashed: snapped back to full size";
  step(golden, mash + 0.8 * d);
  EXPECT_NEAR(step(golden, mash + 1.1 * d), 0.875f, 1e-3f) << "and squeezing again";

  // A use while first person was off (the game's camera) is not shown late.
  for (double t = mash + 1.2 * d; t <= mash + 3.0 * d; t += 0.2 * d) step(golden, t);
  golden.useMark = 300;
  EXPECT_NEAR(step(golden, mash + 3.0 * d + 1.0), 1.0f, 1e-4f) << "no late squeeze";
  EXPECT_NEAR(step(golden, mash + 3.5 * d + 1.0), 1.0f, 1e-4f);

  // A golden mushroom first seen already used grows in without a squeeze.
  state = {};
  golden.useMark = 250;
  for (double t = 20.0; t <= 20.0 + d; t += 0.2 * d) step(golden, t);
  EXPECT_NEAR(step(golden, 20.0 + 1.2 * d), 1.0f, 1e-4f) << "no squeeze for a use not seen";
}

// Where the renderer puts each copy: the triple's three at half the item size,
// spread by their own size around the item's origin above the palm.
TEST(CockpitItemPlacement, TripleCopiesStandAroundTheItemsOrigin) {
  using namespace aurora::gfx::cockpit_item;
  using Clock = ItemAnimation::Clock;
  auto archive = std::make_shared<data::Archive>();
  for (uint8_t id : {uint8_t{4}, uint8_t{16}}) {
    auto& model = archive->models[data::model_index(id)];
    model.parts.emplace_back();
    model.minimum = {-1.0f, 0.0f, -1.0f};
    model.maximum = {1.0f, 1.0f, 1.0f};
  }
  archive->loaded = 1;
  const auto previousArchive = gpuArchive;
  const auto previousAnimation = itemAnimation;
  gpuArchive = archive;
  aurora::gfx::StereoReplayFrame frame{};
  frame.cockpit.itemSize = 0.28f;
  frame.cockpit.hands[1].tracked = true;
  set_identity(frame.cockpit.hands[1].seatFromGrip, {0.2f, -0.3f, -0.4f});
  frame.cockpitItem.valid = true;
  frame.cockpitItem.id = 16;
  frame.cockpitItem.count = 3;
  frame.cockpitItem.hand = 1;
  // Grown long ago.
  itemAnimation = {};
  step_item_animation(itemAnimation, frame.cockpitItem, true, Clock::now() - std::chrono::seconds(1));
  const auto triple = place_item(frame);
  frame.cockpitItem.id = 4;
  frame.cockpitItem.count = 1;
  itemAnimation = {};
  step_item_animation(itemAnimation, frame.cockpitItem, true, Clock::now() - std::chrono::seconds(1));
  const auto single = place_item(frame);
  gpuArchive = previousArchive;
  itemAnimation = previousAnimation;

  // The item frame faces the player (+Z towards them) with its origin 5 cm
  // above the palm; each copy is 14 cm across, a 2-unit model at 0.07.
  ASSERT_EQ(triple.copies, 3u);
  const float origin[3]{0.2f, -0.25f, -0.4f};
  const float expected[3][3]{{-0.07f, 0.0168f, -0.042f}, {0.07f, 0.0168f, -0.042f}, {0.0f, 0.0f, 0.042f}};
  for (uint32_t copy = 0; copy < 3; ++copy) {
    EXPECT_NEAR(triple.modelScale[copy], 0.07f, 1e-5f);
    EXPECT_NEAR(triple.seatFromModel[copy][0], 0.07f, 1e-5f);
    for (int axis = 0; axis < 3; ++axis)
      EXPECT_NEAR(triple.seatFromModel[copy][axis * 4 + 3], origin[axis] + expected[copy][axis], 1e-5f)
          << "copy " << copy << " axis " << axis;
  }
  ASSERT_EQ(single.copies, 1u);
  EXPECT_NEAR(single.modelScale[0], 0.14f, 1e-5f) << "a single item is the whole 28 cm";
  for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(single.seatFromModel[0][axis * 4 + 3], origin[axis], 1e-5f);
}

} // namespace
