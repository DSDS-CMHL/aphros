// Created by Petr Karnakov on 26.06.2020
// Copyright 2020 ETH Zurich

#pragma once

#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "approx.h"
#include "approx2.h"
#include "approx_eb.h"
#include "util/format.h"

#include "dump/dump.h"
#include "dump/raw.h"

#include "particles.h"

template <class EB_>
struct Particles<EB_>::Imp {
  using Owner = Particles<EB_>;
  using UEB = UEmbed<M>;

  struct State {
    // See description of attributes in ParticlesView.
    std::vector<Vect> x;
    std::vector<bool> is_inner;
    std::vector<Vect> v;
    std::vector<Scal> r;
    std::vector<Scal> source;
    std::vector<Scal> rho;
    std::vector<Scal> termvel;
    std::vector<Scal> removed;
  };

  Imp(Owner* owner, M& m_, const EB& eb_, const ParticlesView& init, Scal time,
      Conf conf_)
      : owner_(owner)
      , m(m_)
      , eb(eb_)
      , conf(conf_)
      , time_(time)
      , state_(
            {init.x, init.is_inner, init.v, init.r, init.source, init.rho,
             init.termvel, init.removed}) {
    CheckSize(init);
  }
  static ParticlesView GetView(State& s) {
    return {s.x, s.is_inner, s.v, s.r, s.source, s.rho, s.termvel, s.removed};
  }
  static void CheckSize(const State& s) {
    const size_t n = s.x.size();
    fassert_equal(s.is_inner.size(), n);
    fassert_equal(s.v.size(), n);
    fassert_equal(s.r.size(), n);
    fassert_equal(s.source.size(), n);
    fassert_equal(s.rho.size(), n);
    fassert_equal(s.termvel.size(), n);
    fassert_equal(s.removed.size(), n);
  }
  static void CheckSize(const ParticlesView& s) {
    const size_t n = s.x.size();
    fassert_equal(s.is_inner.size(), n);
    fassert_equal(s.v.size(), n);
    fassert_equal(s.r.size(), n);
    fassert_equal(s.source.size(), n);
    fassert_equal(s.rho.size(), n);
    fassert_equal(s.termvel.size(), n);
    fassert_equal(s.removed.size(), n);
  }
  template <class F>
  static void ForEachAttribute(const ParticlesView& view, F func) {
    func(view.x);
    func(view.is_inner);
    func(view.v);
    func(view.r);
    func(view.source);
    func(view.rho);
    func(view.termvel);
    func(view.removed);
  }
  static void SwapParticles(const ParticlesView& view, size_t i, size_t j) {
    ForEachAttribute(view, [&](auto& v) { //
      std::swap(v[i], v[j]);
    });
  }
  static void ResizeParticles(const ParticlesView& view, size_t size) {
    ForEachAttribute(view, [&](auto& v) { //
      v.resize(size);
    });
  }
  // Clears particles marked as removed.
  static void ClearRemoved(const ParticlesView& view) {
    const size_t n = view.x.size();
    if (!n) {
      return;
    }
    size_t i = 0;
    for (size_t j = n - 1; j >= i;) { // j: index of last non-removed particle
      if (view.removed[i]) {
        while (j > i && view.removed[j]) {
          --j;
        }
        if (i == j) {
          break;
        }
        SwapParticles(view, i, j);
      }
      ++i;
    }
    { // check
      size_t j = 0;
      while (j < n && !view.removed[j]) {
        ++j;
      }
      fassert_equal(j, i);
      while (j < n && view.removed[j]) {
        ++j;
      }
      fassert_equal(j, n);
    }
    ResizeParticles(view, i);
  }
  void Step(
      Scal dt, const FieldEmbed<Scal>& fev,
      const MapEmbed<BCond<Vect>>& mebc_velocity,
      std::function<void(const ParticlesView&)> velocity_hook) {
    auto sem = m.GetSem("step");
    struct {
      FieldFace<Scal> ff_veln;
    } * ctx(sem);
    auto& t = *ctx;
    auto& s = state_;
    if (sem()) {
      t.ff_veln = fev.template Get<FieldFaceb<Scal>>();
      for (auto f : m.Faces()) {
        t.ff_veln[f] /= eb.GetArea(f);
      }
    }
    if (sem.Nested()) {
      Approx2<EB>::ExtrapolateToHaloFaces(t.ff_veln, mebc_velocity, m);
    }
    if (sem("local")) {
      if (m.IsRoot() && time_ == 0) {
        using MIdx = typename M::MIdx;
        const MIdx size(64);
        GBlock<IdxCell, dim> block(size);
        GIndex<IdxCell, dim> index(size);
        FieldCell<Scal> fc(index);
        const IdxCell c0 = m.GetIndexCells().GetIdx(MIdx(0));
        auto callback = [&](const std::function<Vect(Vect x)>& func) {
          const Vect h = m.GetCellSize();
          for (MIdx w : block) {
            Vect x((Vect(w) + Vect(0.5)) / Vect(size));
            x = m.GetCenter(c0) - h * 0.5 + x * (2 * h);
            fc[index.GetIdx(w)] = func(x)[0];
          }
        };
        Approx2<EB>::EvalTrilinearFromFaceField(t.ff_veln, callback, m);
        dump::Raw<M>::WritePlainArrayWithXmf("test.raw", "u", fc.data(), size);
      }

      // Project liquid velocity to particles.
      std::vector<Vect> v_liquid(s.x.size());
      auto callback = [&](const std::function<Vect(Vect x)>& func) {
        for (size_t i = 0; i < s.x.size(); ++i) {
          v_liquid[i] = func(s.x[i]);
        }
      };
      Approx2<EB>::EvalTrilinearFromFaceField(t.ff_veln, callback, m);

      // Compute velocity on particles and advance positions.
      for (size_t i = 0; i < s.x.size(); ++i) {
        const auto c = m.GetCellFromPoint(s.x[i]);
        Scal tau = 0;
        switch (conf.mode) {
          case ParticlesMode::stokes:
            tau = (s.rho[i] - conf.mixture_density) * sqr(2 * s.r[i]) /
                  (18 * conf.mixture_viscosity);
            break;
          case ParticlesMode::termvel:
            tau = s.termvel[i] / conf.gravity.norm();
            break;
          case ParticlesMode::tracer:
            tau = 0;
            break;
        }

        // Update velocity from viscous drag, implicit in time.
        //   dv/dt = (u - v) / tau + g
        // particle velocity `v`, liquid velocity `u`,
        // relaxation time `tau`, gravity `g`
        s.v[i] = (v_liquid[i] + s.v[i] * (tau / dt) + conf.gravity * tau) /
                 (1 + tau / dt);
        velocity_hook(GetView(s));
        s.x[i] += s.v[i] * dt;

        if (s.source[i] != 0) {
          // Apply volume source.
          const Scal pi = M_PI;
          const Scal k = 4. / 3 * pi;
          Scal vol = k * std::pow(s.r[i], 3);
          vol = std::max<Scal>(0, vol + s.source[i] * dt);
          s.r[i] = std::pow(vol / k, 1. / 3);
        }

        const auto gl = m.GetGlobalLength();
        for (size_t d = 0; d < m.GetEdim(); ++d) {
          if (m.flags.is_periodic[d]) {
            if (s.x[i][d] < 0) {
              s.x[i][d] += gl[d];
            }
            if (s.x[i][d] > gl[d]) {
              s.x[i][d] -= gl[d];
            }
          }
        }
        // Remove particles that have left the domain.
        if (!m.GetGlobalBoundingBox().IsInside(s.x[i]) || eb.IsCut(c) ||
            eb.IsExcluded(c)) {
          s.removed[i] = 1;
        }
      }
      ClearRemoved(GetView(s));
      typename M::CommPartRequest req;
      req.x = &s.x;
      req.is_inner = &s.is_inner;
      req.attr_scal = {&s.r, &s.source, &s.rho, &s.termvel, &s.removed};
      req.attr_vect = {&s.v};
      m.CommPart(req);
    }
    if (sem("stat")) {
      time_ += dt;
    }
  }
  static void Append(State& s, const ParticlesView& other) {
    auto append = [](auto& v, const auto& a) {
      v.insert(v.end(), a.begin(), a.end());
    };
    CheckSize(other);
    append(s.x, other.x);
    append(s.is_inner, other.is_inner);
    append(s.v, other.v);
    append(s.r, other.r);
    append(s.source, other.source);
    append(s.rho, other.rho);
    append(s.termvel, other.termvel);
    append(s.removed, other.removed);
    CheckSize(s);
  }
  void DumpCsv(const std::string& path) const {
    auto sem = m.GetSem("dumpcsv");
    struct {
      State s;
      std::vector<int> block;
    } * ctx(sem);
    auto& t = *ctx;
    if (sem("gather")) {
      CheckSize(state_);
      auto& s = state_;
      t.s = s;
      for (size_t i = 0; i < s.x.size(); ++i) {
        t.block.push_back(m.GetId());
      }
      m.Reduce(&t.s.x, Reduction::concat);
      m.Reduce(&t.s.v, Reduction::concat);
      m.Reduce(&t.s.r, Reduction::concat);
      m.Reduce(&t.s.source, Reduction::concat);
      m.Reduce(&t.s.rho, Reduction::concat);
      m.Reduce(&t.s.termvel, Reduction::concat);
      m.Reduce(&t.s.removed, Reduction::concat);
      m.Reduce(&t.block, Reduction::concat);
    }
    if (sem("write") && m.IsRoot()) {
      std::ofstream o(path);
      o.precision(16);
      // header
      o << "x,y,z,vx,vy,vz,r,source,rho,termvel,removed,block";
      o << std::endl;
      // content
      auto& s = t.s;
      for (size_t i = 0; i < s.x.size(); ++i) {
        o << s.x[i][0] << ',' << s.x[i][1] << ',' << s.x[i][2];
        o << ',' << s.v[i][0] << ',' << s.v[i][1] << ',' << s.v[i][2];
        o << ',' << s.r[i];
        o << ',' << s.source[i];
        o << ',' << s.rho[i];
        o << ',' << s.termvel[i];
        o << ',' << s.removed[i];
        o << ',' << t.block[i];
        o << "\n";
      }
    }
    if (sem()) {
    }
  }
  static void ReadCsv(std::istream& in, const ParticlesView& view, char delim) {
    const auto data = dump::ReadCsv<Scal>(in, delim);
    auto set_component = [](std::vector<Vect>& dst, size_t d,
                            const std::vector<Scal>& src) {
      fassert(d < Vect::dim);
      dst.resize(src.size());
      for (size_t i = 0; i < src.size(); ++i) {
        dst[i][d] = src[i];
      }
    };
    for (const auto& p : data) {
      const auto name = p.first;
      for (size_t d = 0; d < Vect::dim; ++d) {
        const std::string dletter(1, GDir<dim>(d).letter());
        // Position.
        if (name == dletter) {
          set_component(view.x, d, p.second);
        }
        // Velocity.
        if (name == "v" + dletter) {
          set_component(view.v, d, p.second);
        }
      }
      if (name == "r") {
        view.r = p.second;
      }
    }
    // Fill the remaining fields with default values.
    const size_t n = view.x.size();
    view.is_inner.resize(n, true);
    view.v.resize(n, Vect(0));
    view.r.resize(n, 0);
    view.source.resize(n, 0);
    view.rho.resize(n, 0);
    view.termvel.resize(n, 0);
    view.removed.resize(n, false);
  }
  static void ReadCsv(
      const std::string& path, const ParticlesView& view, char delim) {
    std::ifstream fin(path);
    fassert(fin.good(), "Can't open file '" + path + "' for reading");
    ReadCsv(fin, view, delim);
  }

  Owner* owner_;
  M& m;
  const EB& eb;
  Conf conf;
  Scal time_;
  State state_;
  size_t nrecv_;
};

template <class EB_>
Particles<EB_>::Particles(
    M& m, const EB& eb, const ParticlesView& init, Scal time, Conf conf)
    : imp(new Imp(this, m, eb, init, time, conf)) {}

template <class EB_>
Particles<EB_>::~Particles() = default;

template <class EB_>
auto Particles<EB_>::GetConf() const -> const Conf& {
  return imp->conf;
}

template <class EB_>
void Particles<EB_>::SetConf(Conf conf) {
  imp->conf = conf;
}

template <class EB_>
void Particles<EB_>::Step(
    Scal dt, const FieldEmbed<Scal>& fe_flux,
    const MapEmbed<BCond<Vect>>& mebc_velocity,
    std::function<void(const ParticlesView&)> velocity_hook) {
  imp->Step(dt, fe_flux, mebc_velocity, velocity_hook);
}

template <class EB_>
auto Particles<EB_>::GetView() const -> ParticlesView {
  return imp->GetView(imp->state_);
}

template <class EB_>
void Particles<EB_>::Append(const ParticlesView& app) {
  imp->Append(imp->state_, app);
}

template <class EB_>
void Particles<EB_>::DumpCsv(const std::string& path) const {
  imp->DumpCsv(path);
}

template <class EB_>
void Particles<EB_>::ReadCsv(
    std::istream& fin, const ParticlesView& view, char delim) {
  Imp::ReadCsv(fin, view, delim);
}

template <class EB_>
void Particles<EB_>::ReadCsv(
    const std::string& path, const ParticlesView& view, char delim) {
  Imp::ReadCsv(path, view, delim);
}

template <class EB_>
auto Particles<EB_>::GetNumRecv() const -> size_t {
  return imp->nrecv_;
}

template <class EB_>
auto Particles<EB_>::GetTime() const -> Scal {
  return imp->time_;
}
