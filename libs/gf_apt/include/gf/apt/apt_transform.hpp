#pragma once

#include <algorithm>
#include <utility>
#include <cmath>

namespace gf::apt {

// 2D affine transform matching the EA APT / Flash SWF placement matrix.
//
// The matrix represents:
//   x' = a*x + c*y + tx
//   y' = b*x + d*y + ty
//
// APT OutputPlaceObject binary layout (big-endian f32 at itemPtr+N):
//   +16  a  (scale_x,       cos-rotation term, m11 in Qt)
//   +20  b  (rotate_skew_0, sin-rotation term, m12 in Qt)
//   +24  c  (rotate_skew_1, -sin / skew term,  m21 in Qt)
//   +28  d  (scale_y,       cos-rotation term, m22 in Qt)
//   +32  tx (translation X,                    dx  in Qt)
//   +36  ty (translation Y,                    dy  in Qt)
//
// Parent-child composition rule (Flash display list):
//   worldTransform = childLocalTransform.composeWith(parentWorldTransform)
//
//   In column-vector matrix algebra: world = M_parent * M_child
//   In Qt row-vector operator* notation: world = childTransform * parentTransform
//
// !! Common bug: Qt's A*B applies A FIRST then B. So for correct Flash
//    composition use: worldT = localT * parentT  (NOT parentT * localT).

struct Transform2D {
  double a  = 1.0; // m11 in Qt — scale-X / cos component
  double b  = 0.0; // m12 in Qt — rotate-skew-0 / sin
  double c  = 0.0; // m21 in Qt — rotate-skew-1 / -sin
  double d  = 1.0; // m22 in Qt — scale-Y / cos component
  double tx = 0.0; // dx  in Qt — X translation
  double ty = 0.0; // dy  in Qt — Y translation

  static constexpr Transform2D identity() noexcept { return {}; }

  static constexpr Transform2D fromTranslation(double x, double y) noexcept {
    Transform2D t; t.tx = x; t.ty = y; return t;
  }

  static constexpr Transform2D fromScale(double sx, double sy) noexcept {
    Transform2D t; t.a = sx; t.d = sy; return t;
  }

  // Apply this transform to a point.
  constexpr std::pair<double,double> mapPoint(double x, double y) const noexcept {
    return { a*x + c*y + tx, b*x + d*y + ty };
  }

  // Returns the world-space transform for a child whose local transform is *this
  // and whose parent's accumulated world transform is `parent`.
  //
  // Math (M_parent * M_local in column-vector notation):
  //   result.a  = parent.a * a  + parent.c * b
  //   result.b  = parent.b * a  + parent.d * b
  //   result.c  = parent.a * c  + parent.c * d
  //   result.d  = parent.b * c  + parent.d * d
  //   result.tx = parent.a * tx + parent.c * ty + parent.tx
  //   result.ty = parent.b * tx + parent.d * ty + parent.ty
  constexpr Transform2D composeWith(const Transform2D& parent) const noexcept {
    Transform2D r;
    r.a  = parent.a * a  + parent.c * b;
    r.b  = parent.b * a  + parent.d * b;
    r.c  = parent.a * c  + parent.c * d;
    r.d  = parent.b * c  + parent.d * d;
    r.tx = parent.a * tx + parent.c * ty + parent.tx;
    r.ty = parent.b * tx + parent.d * ty + parent.ty;
    return r;
  }

  // Axis-aligned bounding box of a local rect's four transformed corners.
  struct AABB { double minX, minY, maxX, maxY; };
  constexpr AABB mapBounds(double l, double t, double r, double b) const noexcept {
    const auto [x0,y0] = mapPoint(l, t);
    const auto [x1,y1] = mapPoint(r, t);
    const auto [x2,y2] = mapPoint(r, b);
    const auto [x3,y3] = mapPoint(l, b);
    return {
      std::min({x0,x1,x2,x3}), std::min({y0,y1,y2,y3}),
      std::max({x0,x1,x2,x3}), std::max({y0,y1,y2,y3})
    };
  }

  constexpr bool isIdentity(double eps = 1e-9) const noexcept {
    return std::abs(a-1.0) < eps && std::abs(d-1.0) < eps
        && std::abs(b)     < eps && std::abs(c)     < eps
        && std::abs(tx)    < eps && std::abs(ty)    < eps;
  }

  constexpr bool operator==(const Transform2D& o) const noexcept {
    return a==o.a && b==o.b && c==o.c && d==o.d && tx==o.tx && ty==o.ty;
  }
};

} // namespace gf::apt
