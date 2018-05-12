#pragma once

#include "convdiff.h"

namespace solver {

template <class M_>
class ConvectionDiffusion : public UnsteadyIterativeSolver {
  using M = M_;
  using Scal = typename M::Scal;
  using Vect = typename M::Vect;
  static constexpr size_t dim = M::dim;
  using Expr = Expression<Scal, IdxCell, 1 + dim * 2>;

 protected:
  const FieldCell<Scal>* fcr_;  // density
  const FieldFace<Scal>* ffd_;  // dynamic viscosity
  const FieldCell<Vect>* fcs_;  // force
  const FieldFace<Scal>* ffv_;  // volume flux

 public:
  ConvectionDiffusion(
      double t, double dt,
      const FieldCell<Scal>* fcr /*density*/,
      const FieldFace<Scal>* ffd /*dynamic viscosity*/,
      const FieldCell<Vect>* fcs /*force*/,
      const FieldFace<Scal>* ffv /*volume flux*/)
      : UnsteadyIterativeSolver(t, dt)
      , fcr_(fcr), ffd_(ffd), fcs_(fcs), ffv_(ffv)
  {}
  virtual const FieldCell<Vect>& GetVelocity() const = 0;
  virtual const FieldCell<Vect>& GetVelocity(Layers) const = 0;
  virtual void CorrectVelocity(Layers, const FieldCell<Vect>&) = 0;
  virtual const FieldCell<Expr>& GetVelocityEquations(size_t /*d*/) const = 0;
};


} // namespace solver
