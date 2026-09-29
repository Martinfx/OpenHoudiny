// pg.Simulation: a network simulated -- or read from a cache -- frame by
// frame; what each frame holds as arrays without a copy; the geometry at the
// frame; frames to a cache and the shot to USD.
#include "Bindings.h"
#include "PyNetwork.h"

#include "pg/sim/Cache.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/UsdExport.h"
#include "pg/sim/WaterMesh.h"
#include "pg/sim/World.h"

#include <pybind11/stl.h>

#include <cstddef>
#include <filesystem>

namespace pg::python {

namespace {

using FramePtr = std::shared_ptr<const sim::Frame>;

struct PyFrame {
    FramePtr f;
};

py::tuple tupleOf(const Vec3& v) { return py::make_tuple(v.x, v.y, v.z); }

py::dict domainOf(const sim::Domain& d) {
    py::dict out;
    out["resolution"] = py::make_tuple(d.cells[0], d.cells[1], d.cells[2]);
    out["voxel"] = d.voxel;
    out["origin"] = tupleOf(d.origin());
    out["size"] = tupleOf(d.size());
    return out;
}

/// A field of a grid, [i, j, k], from `channels` interleaved halves a cell.
Array field(const FramePtr& f, const uint16_t* data, const sim::Domain& d, int channels, int channel) {
    const py::ssize_t cell = 2 * channels;
    const bool some = data != nullptr && d.cellCount() > 0;
    return halves(f, some ? data + channel : nullptr,
                  {some ? d.cells[0] : 0, some ? d.cells[1] : 0, some ? d.cells[2] : 0},
                  {cell, cell * d.cells[0], cell * d.cells[0] * d.cells[1]});
}

/// Where each body's middle -- of the box round its shape at rest -- is at
/// the frame, and how fast that point goes: `out` x, y, z and vx, vy, vz a body.
std::shared_ptr<std::vector<float>> bodyCentres(const sim::RigidFrame& r) {
    auto out = std::make_shared<std::vector<float>>();
    if (r.empty() || !r.pieces) return out;
    const std::shared_ptr<const sim::RigidLayout> layout = r.layout ? r.layout : sim::rigidLayout(*r.pieces, r.attribute);
    const auto P = r.pieces->positions();
    out->resize(6 * r.poses.size());
    for (size_t b = 0; b < r.poses.size(); ++b) {
        Vec3 lo(1e30f), hi(-1e30f);
        if (b < layout->prims.size()) {
            for (const uint32_t prim : layout->prims[b]) {
                for (const uint32_t p : r.pieces->primitivePoints(prim)) {
                    lo = Vec3(std::min(lo.x, P[p].x), std::min(lo.y, P[p].y), std::min(lo.z, P[p].z));
                    hi = Vec3(std::max(hi.x, P[p].x), std::max(hi.y, P[p].y), std::max(hi.z, P[p].z));
                }
            }
        }
        const Vec3 middle = lo.x <= hi.x ? (lo + hi) * 0.5f : Vec3();
        const sim::RigidPose& pose = r.poses[b];
        const Vec3 at = pose.apply(middle), v = pose.velocityAt(middle);
        float* o = out->data() + 6 * b;
        o[0] = at.x, o[1] = at.y, o[2] = at.z, o[3] = v.x, o[4] = v.y, o[5] = v.z;
    }
    return out;
}

struct PySimulation {
    sim::Network net;
    std::string folder, cache;
    sim::GeometryGraph graph;
    sim::Compiled compiled;
    sim::World world;
    std::unique_ptr<sim::WorldSolver> solver;
    FramePtr current;
    std::shared_ptr<const sim::RigidLayout> adopted;  // the pieces' bodies, for frames read back
    std::shared_ptr<const sim::RigidRebar> adoptedBars;  // ... and the bars in them
    std::shared_ptr<const sim::RigidGlue> adoptedGlue;   // ... and the joints of their glue
    int frame = 0, cached = 0;
    py::list problems;

    PySimulation(const PyNetwork& from, const std::string& readFrom, float preview)
        : net(from.net), folder(from.folder), cache(readFrom) {
        graph.setFrames([this](int f) { return current && current->number == f ? current : nullptr; });
        compiled = net.compile(folder, &graph);
        graph.sync(net, folder);
        std::string errors;
        for (const sim::Problem& p : compiled.problems) {
            const sim::Node* n = net.node(p.node);
            const std::string where = n ? n->name + ": " : "";
            if (p.level == sim::Problem::Level::Error) errors += (errors.empty() ? "" : "; ") + where + p.message;
            problems.append(py::make_tuple(p.level == sim::Problem::Level::Error ? "error" : "warning", p.node, p.message));
        }
        if (!compiled.ok) throw Error(errors.empty() ? "nothing to simulate: link a solver into the Output" : errors);
        if (!(preview > 0.0f && preview <= 1.0f)) throw Error("preview is a fraction above 0, at most 1");
        compiled.world = sim::preview(compiled.world, preview);
        world = compiled.world.sanitized();
        if (!cache.empty()) {
            sim::CacheInfo info;
            std::string error;
            if (!sim::readCacheInfo(cache, info, error)) throw Error(error);
            cached = info.frames;
        } else {
            solver = std::make_unique<sim::WorldSolver>(compiled.world);
        }
    }

    int frames() const { return cache.empty() ? compiled.frames : cached; }

    FramePtr step() {
        const int next = frame + 1;
        if (!cache.empty()) {
            if (next > cached) throw Error(cache + " has " + std::to_string(cached) + " frames");
            auto read = std::make_shared<sim::Frame>();
            std::string error;
            bool ok = false;
            {
                py::gil_scoped_release release;
                ok = sim::readFrame(cache, next, *read, error);
            }
            if (!ok) throw Error(error);
            read->number = next;  // the file's name says which it is
            sim::adoptPieces(*read, world.rigid, &adopted, &adoptedBars, &adoptedGlue);
            sim::adoptCloth(*read, world.cloth);
            current = std::move(read);
        } else {
            py::gil_scoped_release release;
            solver->step();
            current = std::make_shared<const sim::Frame>(solver->capture());
        }
        frame = next;
        return current;
    }

    GeometryPtr cook(int id) {
        if (!net.node(id)) throw Error("no node " + std::to_string(id) + " in the network");
        if (!graph.contains(id)) throw Error(net.node(id)->name + " is not a geometry node");
        GeometryPtr g;
        {
            py::gil_scoped_release release;
            g = graph.cook(id, std::max(frame, 1), world.timeStep);
        }
        const std::string error = graph.error(id);
        if (!error.empty()) throw Error(net.node(id)->name + ": " + error);
        return g;
    }
};

struct PyUsdExport {
    std::unique_ptr<sim::UsdExport> usd;
    PySimulation* sim = nullptr;
    int node = 0;
};

}  // namespace

void bindSimulation(py::module_& m) {
    py::class_<PyFrame>(m, "Frame", "What a frame of a simulation holds, as the editor and the cache keep it.")
        .def_property_readonly("number", [](const PyFrame& p) { return p.f->number; })
        .def_property_readonly("time", [](const PyFrame& p) { return p.f->time; })
        .def_property_readonly("step_ms", [](const PyFrame& p) { return p.f->stepMs; })
        .def_property_readonly("bytes", [](const PyFrame& p) { return p.f->bytes(); })
        // --- gas
        .def_property_readonly("has_gas", [](const PyFrame& p) { return !p.f->fields.empty(); })
        .def("gas_domain", [](const PyFrame& p) { return domainOf(p.f->domain); })
        .def("gas", [](const PyFrame& p, const std::string& name) {
            const int channel = name == "density" ? 0 : name == "temperature" ? 1 : name == "flame" ? 2 : -1;
            if (channel < 0) throw Error("no field '" + name + "' of the gas: density, temperature or flame");
            // A sparse frame's tiles into every cell: a frame of its own, that
            // the array keeps.
            FramePtr f = p.f;
            if (!f->gasTiles.empty()) {
                auto dense = std::make_shared<sim::Frame>();
                dense->domain = f->domain;
                std::vector<uint16_t> scratch;
                dense->fields = f->denseFields(scratch);
                f = dense;
            }
            const bool some = f->fields.size() >= 3 * f->domain.cellCount();
            return field(f, some ? f->fields.data() : nullptr, f->domain, 3, channel);
        })
        // --- water
        .def_property_readonly("has_water", [](const PyFrame& p) { return !p.f->water.empty(); })
        .def_property_readonly("water_particle_count", [](const PyFrame& p) { return p.f->water.particles; })
        .def_property_readonly("water_litres", [](const PyFrame& p) { return p.f->water.litres; })
        .def("water_domain", [](const PyFrame& p) { return domainOf(p.f->water.flowDomain()); })
        .def("water_positions", [](const PyFrame& p) {
            const auto& v = p.f->water.positions;
            return rows(p.f, reinterpret_cast<const float*>(v.data()), v.size(), 3);
        })
        .def("water_velocities", [](const PyFrame& p) {
            const auto& v = p.f->water.velocities;
            const py::ssize_t n = static_cast<py::ssize_t>(v.size() / 3);
            return halves(p.f, v.data(), {n, 3}, {6, 2});
        })
        .def("water_foam", [](const PyFrame& p) {
            const auto& v = p.f->water.whiteness;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("water_ids", [](const PyFrame& p) {
            const auto& v = p.f->water.ids;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("water_flow", [](const PyFrame& p) {
            const sim::WaterFrame& w = p.f->water;
            const sim::Domain d = w.flowDomain();
            const bool some = !w.flow.empty() && w.flow.size() == 3 * d.cellCount();
            return halves(p.f, some ? w.flow.data() : nullptr,
                          {some ? d.cells[0] : 0, some ? d.cells[1] : 0, some ? d.cells[2] : 0, 3},
                          {6, 6 * d.cells[0], 6 * d.cells[0] * d.cells[1], 2});
        })
        .def("water_surface", [](const PyFrame& p, bool ripples) {
            GeometryPtr g;
            {
                py::gil_scoped_release release;
                g = sim::waterMesh(p.f->water, ripples ? &p.f->rain : nullptr);
            }
            return PyGeometry(g);
        }, py::arg("ripples") = true)
        // --- rain
        .def_property_readonly("has_rain", [](const PyFrame& p) { return !p.f->rain.empty(); })
        .def("rain_drops", [](const PyFrame& p) {
            const auto& v = p.f->rain.drops;
            return rows(p.f, v.data(), v.size() / 6, 6);
        })
        .def("rain_drop_ids", [](const PyFrame& p) {
            const auto& v = p.f->rain.dropIds;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("rain_droplets", [](const PyFrame& p) {
            const auto& v = p.f->rain.droplets;
            return rows(p.f, v.data(), v.size() / 6, 6);
        })
        .def("rain_droplet_ids", [](const PyFrame& p) {
            const auto& v = p.f->rain.dropletIds;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("ripples", [](const PyFrame& p) -> py::object {
            const sim::RainFrame& r = p.f->rain;
            if (r.ripples.empty()) return py::none();
            py::dict out;
            out["origin"] = tupleOf(r.rippleOrigin);
            out["cell"] = r.rippleCell;
            out["heights"] = py::cast(halves(p.f, r.ripples.data(), {r.rippleCells[0], r.rippleCells[1]},
                                             {2, 2 * static_cast<py::ssize_t>(r.rippleCells[0])}));
            return out;
        })
        // --- rigid bodies
        .def_property_readonly("has_rigid", [](const PyFrame& p) { return !p.f->rigid.empty(); })
        .def_property_readonly("body_count", [](const PyFrame& p) { return p.f->rigid.poses.size(); })
        .def_property_readonly("joints", [](const PyFrame& p) { return p.f->rigid.joints; })
        .def_property_readonly("broken", [](const PyFrame& p) { return p.f->rigid.broken; })
        .def("body_translations", [](const PyFrame& p) {
            const auto& v = p.f->rigid.poses;
            return rows(p.f, v.empty() ? nullptr : &v[0].position.x, v.size(), 3, sizeof(sim::RigidPose));
        })
        .def("body_centres", [](const PyFrame& p) {
            const auto c = bodyCentres(p.f->rigid);
            return rows(c, c->data(), c->size() / 6, 3, 6 * sizeof(float));
        })
        .def("body_centre_velocities", [](const PyFrame& p) {
            const auto c = bodyCentres(p.f->rigid);
            return rows(c, c->empty() ? nullptr : c->data() + 3, c->size() / 6, 3, 6 * sizeof(float));
        })
        .def("body_rotations", [](const PyFrame& p) {
            const auto& v = p.f->rigid.poses;
            return rows(p.f, v.empty() ? nullptr : &v[0].rotation.x, v.size(), 4, sizeof(sim::RigidPose));
        })
        .def("body_spins", [](const PyFrame& p) {
            const auto& v = p.f->rigid.poses;
            return rows(p.f, v.empty() ? nullptr : &v[0].spin.x, v.size(), 3, sizeof(sim::RigidPose));
        })
        .def("vanished", [](const PyFrame& p) { return p.f->rigid.vanished; })
        .def("unglued", [](const PyFrame& p) { return p.f->rigid.unglued; })
        .def("grit", [](const PyFrame& p) {
            const auto& v = p.f->rigid.debris;
            return rows(p.f, v.data(), v.size() / 4, 4);
        })
        .def("grit_velocities", [](const PyFrame& p) {
            const auto& v = p.f->rigid.debrisVelocity;
            return rows(p.f, v.data(), v.size() / 3, 3);
        })
        .def("grit_ids", [](const PyFrame& p) {
            const auto& v = p.f->rigid.debrisIds;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("grit_orient", [](const PyFrame& p) {
            const auto& v = p.f->rigid.debrisOrient;
            return rows(p.f, v.data(), v.size() / 4, 4);
        })
        .def("grit_glass", [](const PyFrame& p) {
            const auto& v = p.f->rigid.debrisGlass;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("pieces", [](const PyFrame& p) {
            if (p.f->rigid.empty() || !p.f->rigid.pieces) return PyGeometry();
            return PyGeometry(sim::posedPieces(p.f->rigid));
        })
        .def("network", [](const PyFrame& p) { return PyGeometry(sim::rigidNetwork(p.f->rigid)); })
        .def("joint_state", [](const PyFrame& p) {
            const auto& v = p.f->rigid.jointState;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("joint_time", [](const PyFrame& p) {
            const auto& v = p.f->rigid.jointTime;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("rebar", [](const PyFrame& p) { return PyGeometry(sim::rebarBars(p.f->rigid)); })
        .def("rebar_state", [](const PyFrame& p) {
            const auto& v = p.f->rigid.rebarState;
            return rows(p.f, v.data(), v.size(), 1);
        })
        .def("rebar_stations", [](const PyFrame& p) {
            // body, in, out and the bar of each station
            py::list out;
            if (!p.f->rigid.rebar) return out;
            const sim::RigidRebar& r = *p.f->rigid.rebar;
            for (size_t b = 0; b < r.bars.size(); ++b) {
                for (uint32_t s = r.bars[b].first; s < r.bars[b].first + r.bars[b].count; ++s) {
                    const sim::RigidRebar::Station& st = r.stations[s];
                    out.append(py::make_tuple(st.body, st.in, st.out, b));
                }
            }
            return out;
        })
        // --- files
        .def("save", [](const PyFrame& p, const std::string& folder) {
            std::string error;
            if (!sim::writeFrame(*p.f, folder, error)) throw Error(error);
            return sim::frameFile(folder, p.f->number);
        })
        .def_static("read", [](const std::string& folder, int number) {
            auto f = std::make_shared<sim::Frame>();
            std::string error;
            if (!sim::readFrame(folder, number, *f, error)) throw Error(error);
            f->number = number;
            return PyFrame{std::move(f)};
        });

    py::class_<PySimulation>(m, "Simulation", "A network simulated -- or read from a cache -- a frame at a time.")
        .def(py::init([](const PyNetwork& net, const std::string& cache, float preview) {
                 return new PySimulation(net, cache, preview);
             }),
             py::arg("network"), py::arg("cache") = "", py::arg("preview") = 1.0f)
        .def("step", [](PySimulation& s) { return PyFrame{s.step()}; })
        .def_property_readonly("frame", [](const PySimulation& s) { return s.frame; })
        .def_property_readonly("frames", &PySimulation::frames)
        .def_property_readonly("fps", [](const PySimulation& s) { return 1.0f / s.world.timeStep; })
        .def_property_readonly("time_step", [](const PySimulation& s) { return s.world.timeStep; })
        .def_property_readonly("problems", [](const PySimulation& s) { return s.problems; })
        .def_property_readonly("current", [](const PySimulation& s) -> py::object {
            return s.current ? py::cast(PyFrame{s.current}) : py::none();
        })
        .def_property_readonly("displayed", [](const PySimulation& s) { return s.compiled.display; })
        .def("geometry", [](PySimulation& s, int id) { return PyGeometry(s.cook(id)); })
        .def("camera", [](const PySimulation& s, int frame) -> py::object {
            if (!s.compiled.hasCamera) return py::none();
            const sim::Camera& c = s.compiled.cameraAt(frame > 0 ? frame : std::max(s.frame, 1));
            py::dict out;
            out["position"] = tupleOf(c.position);
            out["rotation"] = tupleOf(c.rotation);
            out["focal"] = c.focal;
            out["width"] = c.width;
            out["height"] = c.height;
            return out;
        }, py::arg("frame") = 0)
        .def("save_state", [](const PySimulation& s) {
            if (!s.solver) throw Error("frames read from a cache have no state to save");
            std::string state;
            {
                py::gil_scoped_release release;
                state = s.solver->saveState();
            }
            return py::bytes(state);
        }, "All it takes to go on from this frame as if it had never stopped: a checkpoint (bytes).")
        .def("load_state", [](PySimulation& s, const py::bytes& bytes) {
            if (!s.solver) throw Error("frames read from a cache take no state");
            if (s.frame != 0) throw Error("a state goes into a simulation that has not stepped yet");
            const std::string state = bytes;
            std::string error;
            bool ok = false;
            {
                py::gil_scoped_release release;
                ok = s.solver->loadState(state, error);
                if (ok) s.current = std::make_shared<const sim::Frame>(s.solver->capture());
            }
            if (!ok) {
                // Of no use now: a fresh one takes its place.
                s.solver = std::make_unique<sim::WorldSolver>(s.compiled.world);
                s.current.reset();
                throw Error(error);
            }
            s.frame = s.solver->frame();
        }, py::arg("state"), "Goes on from a state save_state() gave, of the same network: the next step is the frame after it.")
        .def("write_cache_info", [](const PySimulation& s, const std::string& folder) {
            sim::CacheInfo info;
            info.frames = s.frame;
            info.fps = 1.0f / s.world.timeStep;
            info.network = sim::networkHash(s.net.save());
            std::string error;
            if (!sim::writeCacheInfo(folder, info, error)) throw Error(error);
        });

    py::class_<PyUsdExport>(m, "UsdExport", "A simulated shot out to USD, a frame at a time (UsdExport.h).")
        .def(py::init([](const std::string& path, PySimulation& sim, int node) {
            auto u = std::make_unique<PyUsdExport>();
            u->sim = &sim;
            u->node = node;
            const sim::Node* n = node ? sim.net.node(node) : nullptr;
            if (node && !n) throw Error("no node " + std::to_string(node) + " in the network");
            u->usd = std::make_unique<sim::UsdExport>(path, n ? n->name : std::string("geometry"), 1.0f / sim.world.timeStep);
            return u;
        }), py::arg("path"), py::arg("simulation"), py::arg("node") = 0, py::keep_alive<1, 3>())
        .def("add", [](PyUsdExport& u) {
            PySimulation& s = *u.sim;
            if (!s.current) throw Error("no frame yet: step the simulation first");
            const GeometryPtr geo = u.node ? s.cook(u.node) : nullptr;
            const int f = s.current->number;
            std::string error;
            bool ok = false;
            {
                py::gil_scoped_release release;
                ok = u.usd->add(*s.current, geo, s.compiled.hasCamera ? &s.compiled.cameraAt(f) : nullptr,
                                s.compiled.lookAt(f), error);
            }
            if (!ok) throw Error(error);
        })
        .def("finish", [](PyUsdExport& u) {
            std::string error;
            if (!u.usd->finish(error)) throw Error(error);
            return u.usd->path();
        })
        .def_property_readonly("frames", [](const PyUsdExport& u) { return u.usd->frames(); })
        .def_property_readonly("bodies", [](const PyUsdExport& u) { return u.usd->bodies(); })
        .def_property_readonly("gas_files", [](const PyUsdExport& u) { return u.usd->gasFiles(); })
        .def_property_readonly("frame_files", [](const PyUsdExport& u) { return u.usd->frameFiles(); });

    m.def("read_cache_info", [](const std::string& folder) {
        sim::CacheInfo info;
        std::string error;
        if (!sim::readCacheInfo(folder, info, error)) throw Error(error);
        py::dict out;
        out["frames"] = info.frames;
        out["fps"] = info.fps;
        out["network"] = info.network;
        return out;
    });
    m.def("network_hash", [](const PyNetwork& n) { return sim::networkHash(n.net.save()); });
}

}  // namespace pg::python
