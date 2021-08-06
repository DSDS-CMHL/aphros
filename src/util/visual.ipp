// Created by Petr Karnakov on 19.03.2021
// Copyright 2021 ETH Zurich

#pragma once

#include <fstream>
#include <functional>
#include <string>

#include "geom/mesh.h"
#include "parse/codeblocks.h"
#include "parse/parser.h"
#include "parse/vars.h"
#include "util/logger.h"
#include "visual.h"

namespace util {

std::vector<Entry> ParseEntries(std::istream& in) {
  std::vector<Entry> entries;
  auto blocks = ParseCodeBlocks(in);
  for (auto& b : blocks) {
    Entry entry;
    entry.field = b.name;
    std::stringstream ss(b.content);
    Parser(entry.var).ParseStream(ss);
    entries.push_back(entry);
  }
  return entries;
}

std::vector<Entry> ParseEntries(std::string path) {
  std::ifstream f(path);
  fassert(f.good(), "Can't open file '" + path + "' for reading");
  return ParseEntries(f);
}

Colormap GetColormap(const Vars& var) {
  Colormap cmap;
  cmap.values = var.Vect["values"];
  cmap.opacities = var.Vect("opacities", {});
  const auto& colors = var.Vect("colors", {});
  cmap.colors.resize(cmap.values.size());
  for (size_t i = 0; i < std::min(cmap.values.size(), colors.size() / 3); ++i) {
    cmap.colors[i][0] = colors[3 * i + 0];
    cmap.colors[i][1] = colors[3 * i + 1];
    cmap.colors[i][2] = colors[3 * i + 2];
  }
  // fill the tail with default values
  for (size_t i = cmap.opacities.size(); i < cmap.values.size(); ++i) {
    cmap.opacities.push_back(1);
  }
  for (size_t i = cmap.colors.size(); i < cmap.values.size(); ++i) {
    cmap.colors.push_back(Float3(0, 0, 0));
  }
  return cmap;
}

void WritePpm(std::ostream& out, const CanvasView& view, bool binary) {
  out << (binary ? "P6" : "P3") << '\n';
  out << view.size[0] << ' ' << view.size[1] << '\n';
  out << 255 << '\n';
  if (binary) {
    for (int y = view.size[1]; y > 0;) {
      --y;
      for (int x = 0; x < view.size[0]; ++x) {
        out.put(view(x, y) & 0xFF);
        out.put((view(x, y) >> 8) & 0xFF);
        out.put((view(x, y) >> 16) & 0xFF);
      }
    }
  } else { // ASCII
    for (int y = view.size[1]; y > 0;) {
      --y;
      for (int x = 0; x < view.size[0]; ++x) {
        out << (view(x, y) & 0xFF) << ' ';
        out << ((view(x, y) >> 8) & 0xFF) << ' ';
        out << ((view(x, y) >> 16) & 0xFF) << ' ';
      }
      out << '\n';
    }
  }
}

void WritePpm(std::string path, const CanvasView& view, bool binary) {
  std::ofstream f(path, std::ios::binary);
  fassert(f.good(), "Can't open file '" + path + "' for writing");
  WritePpm(f, view, binary);
}

Canvas ReadPpm(std::istream& in) {
  std::string line;

  std::getline(in, line);
  fassert(line == "P3" || line == "P6", "Got '" + line + "'");
  const bool binary = (line == "P6");

  MIdx2 size;
  in >> size[0] >> size[1];

  int max_color;
  in >> max_color;
  fassert(max_color == 255);

  Canvas canvas(size);
  CanvasView view(canvas);

  if (binary) {
    in.get(); // skip line ending
    for (int y = canvas.size[1]; y > 0;) {
      --y;
      for (int x = 0; x < canvas.size[0]; ++x) {
        Byte3 q;
        q[0] = in.get();
        q[1] = in.get();
        q[2] = in.get();
        view(x, y) = 0xFF000000 | (q[0] << 0) | (q[1] << 8) | (q[2] << 16);
      }
    }
  } else { // ASCII
    for (int y = canvas.size[1]; y > 0;) {
      --y;
      for (int x = 0; x < canvas.size[0]; ++x) {
        generic::MIdx<3> t;
        in >> t;
        Byte3 q(t);
        view(x, y) = 0xFF000000 | (q[0] << 0) | (q[1] << 8) | (q[2] << 16);
      }
    }
  }
  return canvas;
}

Canvas ReadPpm(std::string path) {
  std::ifstream f(path, std::ios::binary);
  fassert(f.good(), "Can't open file '" + path + "' for reading");
  return ReadPpm(f);
}

template <class M>
struct Visual<M>::Imp {
  static Scal Clamp(Scal f, Scal min, Scal max) {
    return f < min ? min : f > max ? max : f;
  }

  template <class T>
  static T Clamp(T v) {
    return v.max(T(0)).min(T(1));
  }

  static Scal Clamp(Scal f) {
    return Clamp(f, 0, 1);
  }

  static void RenderToField(
      FieldCell<Float3>& fc_color, const FieldCell<Scal>& fcu,
      const Colormap& cmap, const M& m) {
    cmap.Check();
    for (auto c : m.SuCells()) {
      fc_color[c] = cmap(fc_color[c], fcu[c]);
    }
  }

  static void RenderEntriesToField(
      FieldCell<Float3>& fc_color, const std::vector<Entry>& entries,
      const std::function<FieldCell<Scal>(std::string)>& get_field,
      const M& m) {
    for (const auto& entry : entries) {
      auto cmap = GetColormap(entry.var);
      RenderToField(fc_color, get_field(entry.field), cmap, m);
    }
  }

  static void RenderToCanvasNearest(
      CanvasView& view, const FieldCell<Float3>& fc_color, const M& m) {
    const MIdx msize = m.GetGlobalSize();
    const MIdx2 msize2(msize);
    for (auto c : m.CellsM()) {
      const MIdx w(c);
      if (M::dim >= 3 && w[2] != msize[2] / 2) { // take central slice in 3D
        continue;
      }
      const MIdx2 w2(w);
      const MIdx2 start = w2 * view.size / msize2;
      const MIdx2 end = (w2 + MIdx2(1)) * view.size / msize2;
      const Byte3 q(Clamp(fc_color[c]) * 255);
      const Pixel v = 0xff000000 | (q[0] << 0) | (q[1] << 8) | (q[2] << 16);
      for (int y = start[1]; y < end[1]; y++) {
        for (int x = start[0]; x < end[0]; x++) {
          view(x, y) = v;
        }
      }
    }
  }

  // Evaluates bilinear interpolant on points (0,0), (1,0), (0,1) and (1,1).
  // x,y: target point
  // u,ux,uy,uyx:  values of function for (x,y) = (0,0), (1,0), (0,1), (1,1)
  // FIXME: this is a copy from approx_eb.ipp
  template <class T, class Scal>
  static T Bilinear(Scal x, Scal y, T u, T ux, T uy, T uyx) {
    //                      //
    //   y                  //
    //   |                  //
    //   |*uy    *uyx       //
    //   |                  //
    //   |                  //
    //   |*u     *ux        //
    //   |-------------x    //
    //                      //
    const auto v = u * (1 - x) + ux * x;
    const auto vy = uy * (1 - x) + uyx * x;
    return v * (1 - y) + vy * y;
  }

  static void RenderToCanvasBilinear(
      CanvasView& view, const FieldCell<Float3>& fc_color, const M& m) {
    const MIdx msize = m.GetGlobalSize();
    const MIdx2 msize2(msize);
    for (auto c : m.SuCellsM()) {
      const MIdx w(c);
      if (M::dim >= 3 && w[2] != msize[2] / 2) { // take central slice in 3D
        continue;
      }
      const MIdx2 w2(w);
      const MIdx2 bs = view.size / msize2;
      const MIdx2 start = (w2 * view.size / msize2 + bs / 2).max(MIdx2(0));
      const MIdx2 end =
          ((w2 + MIdx2(1)) * view.size / msize2 + bs / 2).min(view.size);
      const auto dx = m.direction(0);
      const auto dy = m.direction(1);
      const auto q = fc_color[c];
      const auto qx = fc_color[c + dx];
      const auto qy = fc_color[c + dy];
      const auto qyx = fc_color[c + dx + dy];
      for (int y = start[1]; y < end[1]; y++) {
        const Scal fy = Scal(y - start[1]) / (end[1] - start[1]);
        for (int x = start[0]; x < end[0]; x++) {
          const Scal fx = Scal(x - start[0]) / (end[0] - start[0]);
          auto qb = Bilinear(std::abs(fx), std::abs(fy), q, qx, qy, qyx);
          const Byte3 mq(qb * 255);
          Pixel v = 0xff000000 | (mq[0] << 0) | (mq[1] << 8) | (mq[2] << 16);
          view(x, y) = v;
        }
      }
    }
  }
};

template <class M>
void Visual<M>::RenderToField(
    FieldCell<Float3>& fc_color, const FieldCell<Scal>& fcu,
    const Colormap& cmap, const M& m) {
  Imp::RenderToField(fc_color, fcu, cmap, m);
}

template <class M>
void Visual<M>::RenderEntriesToField(
    FieldCell<Float3>& fc_color, const std::vector<Entry>& entries,
    const std::function<FieldCell<Scal>(std::string)>& get_field, const M& m) {
  Imp::RenderEntriesToField(fc_color, entries, get_field, m);
}

template <class M>
void Visual<M>::RenderToCanvasNearest(
    CanvasView& view, const FieldCell<Float3>& fc_color, const M& m) {
  Imp::RenderToCanvasNearest(view, fc_color, m);
}

template <class M>
void Visual<M>::RenderToCanvasBilinear(
    CanvasView& view, const FieldCell<Float3>& fc_color, const M& m) {
  Imp::RenderToCanvasBilinear(view, fc_color, m);
}

} // namespace util
