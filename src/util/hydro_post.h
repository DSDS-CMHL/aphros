// Created by Petr Karnakov on 20.03.2021
// Copyright 2021 ETH Zurich

#pragma once

#include "kernel/hydro.h"

template <class M>
struct HydroPost {
  using Scal = typename M::Scal;
  using Vect = typename M::Vect;

  static bool GetFieldByName(
      const Hydro<M>* hydro, std::string name, FieldCell<Scal>& fc_out,
      const M& m);

  static FieldCell<Scal> GetField(
      const Hydro<M>* hydro, std::string name, const M& m);

  static void DumpFields(Hydro<M>* hydro, M& m);

  struct Imp;
};
