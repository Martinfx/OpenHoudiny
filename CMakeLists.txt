cmake_minimum_required(VERSION 3.20)
project(prototype LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Prototype: procedural geometry, simulations and shaders, built from nodes.
#
# The core needs C++20, the standard library and GLM -- the vectors,
# matrices and quaternions everything is computed with -- which is headers
# alone: the system's when it has one, else fetched. The default build adds the editor to the `prototype` program, which fetches Dear
# ImGui and (unless the system has it) GLFW; with -DPG_BUILD_GUI=OFF
# `cmake -B build && cmake --build build && ctest --test-dir build` works on a
# bare machine. Production targets (TBB, OpenVDB, USD, LLVM) are listed in
# ARCHITECTURE.md; none of them are needed to evaluate the core invariants.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The same results on any machine (ARCHITECTURE.md: determinism) and on the
# GPU (docs/gpu.md): no multiply and add fused into one behind the code's
# back, whatever -march the build is given.
if(NOT MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PG_SANITIZE "Build with address+undefined sanitizers" OFF)
option(PG_SANITIZE_THREAD "Build with the thread sanitizer" OFF)
option(PG_WITH_JOLT "Rigid bodies with Jolt Physics (MIT), fetched; OFF: the RBD Solver says it is not built in" ON)
option(PG_BUILD_PYTHON "Build the Python module pg (fetches pybind11); needs Python's development files" ON)

# The Python module (import pg, docs/python.md) is the libraries in a shared
# library: they are built position-independent for it. Not with the
# sanitizers: a sanitized library does not load into a plain Python.
if(PG_BUILD_PYTHON AND (PG_SANITIZE OR PG_SANITIZE_THREAD))
  set(PG_BUILD_PYTHON OFF)
endif()
if(PG_BUILD_PYTHON)
  find_package(Python3 COMPONENTS Interpreter Development.Module)
  if(Python3_FOUND)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  else()
    message(STATUS "The Python module is left out: no Python development files (python3-dev)")
    set(PG_BUILD_PYTHON OFF)
  endif()
endif()

# --- GLM --------------------------------------------------------------------
#
# The vectors, matrices and quaternions of the whole program are GLM's
# (OpenGL Mathematics, MIT; headers alone): the system's -- FreeBSD's
# math/glm, Debian's and Ubuntu's libglm-dev -- when it is 1.0 or newer,
# else fetched. Every file sees them the same way, through pgmath: vectors
# start at zero and matrices at the identity (GLM_FORCE_CTOR_INIT), and x, y,
# z and w alone name their parts -- plain structs of floats, laid out as the
# attributes store them (GLM_FORCE_XYZW_ONLY).
find_package(glm 1.0 CONFIG QUIET)
if(NOT TARGET glm::glm)
  include(FetchContent)
  set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glm)
  if(NOT TARGET glm-header-only)
    message(FATAL_ERROR "GLM 1.0 or newer is needed and could not be downloaded. "
      "Install it (FreeBSD: pkg install glm; Debian 13, Ubuntu 25.04: apt install libglm-dev) "
      "or point FETCHCONTENT_SOURCE_DIR_GLM at a checkout of github.com/g-truc/glm 1.0.1.")
  endif()
  # Its headers as the system's: our warnings are not about them.
  get_target_property(PG_GLM_INCLUDES glm-header-only INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(glm-header-only PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PG_GLM_INCLUDES}")
endif()
add_library(pgmath INTERFACE)
target_link_libraries(pgmath INTERFACE glm::glm)
target_compile_definitions(pgmath INTERFACE GLM_FORCE_CTOR_INIT GLM_FORCE_XYZW_ONLY GLM_ENABLE_EXPERIMENTAL)

add_library(pgcore
  src/pg/core/Types.cpp
  src/pg/core/ColorSpace.cpp
  src/pg/core/Attribute.cpp
  src/pg/core/Geometry.cpp
  src/pg/core/Parallel.cpp
  src/pg/core/Node.cpp
  src/pg/core/Graph.cpp
  src/pg/core/CookEngine.cpp
  src/pg/core/Spatial.cpp
  src/pg/core/Pick.cpp
  src/pg/core/Sculpt.cpp
  src/pg/core/Selection.cpp
  src/pg/core/Soft.cpp
  src/pg/core/Tree.cpp
  src/pg/core/Chips.cpp
  src/pg/core/Instances.cpp
  src/pg/core/Lod.cpp
  src/pg/core/Wind.cpp
  src/pg/core/Dissolve.cpp
  src/pg/core/Ecosystem.cpp
  src/pg/core/Mirror.cpp
  src/pg/core/Dyntopo.cpp
  src/pg/core/Grass.cpp
  src/pg/lang/Parse.cpp
  src/pg/lang/Check.cpp
  src/pg/lang/Eval.cpp
  src/pg/lang/Run.cpp
  src/pg/lang/Builtins.cpp
  src/pg/lang/BuiltinsGeo.cpp
  src/pg/nodes/Generators.cpp
  src/pg/nodes/Modifiers.cpp
  src/pg/nodes/Wrangle.cpp
  src/pg/nodes/Primitives.cpp
  src/pg/nodes/Surface.cpp
  src/pg/nodes/Topology.cpp
  src/pg/nodes/Fracture.cpp
  src/pg/nodes/Concrete.cpp
  src/pg/nodes/Rough.cpp
  src/pg/nodes/Wood.cpp
  src/pg/nodes/Cluster.cpp
  src/pg/nodes/Rebar.cpp
  src/pg/nodes/Glass.cpp
  src/pg/nodes/Bricks.cpp
  src/pg/nodes/Rebuild.cpp
  src/pg/nodes/Volumes.cpp
  src/pg/nodes/Usd.cpp
  src/pg/nodes/Abc.cpp
  src/pg/nodes/Vdb.cpp
  src/pg/nodes/Edit.cpp
  src/pg/nodes/Trees.cpp
  src/pg/nodes/Grass.cpp
  src/pg/nodes/Wind.cpp
  src/pg/nodes/Ecosystem.cpp
  src/pg/io/Obj.cpp
  src/pg/io/Vdb.cpp
  src/pg/io/Deflate.cpp
  src/pg/io/MaterialX.cpp
  src/pg/io/VdbRead.cpp
  src/pg/io/Jpeg.cpp
  src/pg/io/Video.cpp
  src/pg/io/Ply.cpp
  src/pg/io/Export.cpp
  src/pg/io/Usda.cpp
  src/pg/io/Exr.cpp
  src/pg/io/ExrRead.cpp
  src/pg/io/Inflate.cpp
  src/pg/io/Lz4.cpp
  src/pg/io/Blosc.cpp
  src/pg/io/Png.cpp
  src/pg/io/JpegDecode.cpp
  src/pg/io/Picture.cpp
  src/pg/io/Yaml.cpp
  src/pg/usd/Layer.cpp
  src/pg/usd/Text.cpp
  src/pg/usd/Crate.cpp
  src/pg/usd/Stage.cpp
  src/pg/usd/Geom.cpp
  src/pg/usd/Shade.cpp
  src/pg/io/Ogawa.cpp
  src/pg/abc/Archive.cpp
  src/pg/abc/Geom.cpp
)
target_include_directories(pgcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(pgcore PUBLIC Threads::Threads pgmath)

if(MSVC)
  target_compile_options(pgcore PRIVATE /W4)
else()
  target_compile_options(pgcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PG_SANITIZE)
  target_compile_options(pgcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(pgcore PUBLIC -fsanitize=address,undefined)
endif()
if(PG_SANITIZE_THREAD)
  target_compile_options(pgcore PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(pgcore PUBLIC -fsanitize=thread)
endif()

# --- GPU compute ---------------------------------------------------------------
#
# Kernels on the graphics card through Vulkan (src/pg/gpu, docs/gpu.md).
# Vulkan is loaded at run time (libvulkan.so.1), not linked: a machine
# without it runs everything on the CPU. Building it takes the Vulkan
# headers (vulkan-headers, libvulkan-dev) and glslangValidator (glslang),
# which compiles the kernels to SPIR-V that the program then carries;
# without them it is left out.

option(PG_WITH_VULKAN "GPU compute through Vulkan, where the headers and glslangValidator are found" ON)
if(PG_WITH_VULKAN)
  # The Vulkan SDK (Windows, macOS) says where it is in VULKAN_SDK.
  find_path(PG_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS ENV VULKAN_SDK PATH_SUFFIXES include Include)
  find_program(GLSLANG_VALIDATOR glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin Bin)
  if(NOT PG_VULKAN_INCLUDE_DIR OR NOT GLSLANG_VALIDATOR)
    message(STATUS "GPU compute: off -- no Vulkan headers (vulkan/vulkan.h) or no glslangValidator")
    set(PG_WITH_VULKAN OFF)
  endif()
endif()
if(PG_WITH_VULKAN)
  message(STATUS "GPU compute: Vulkan (headers ${PG_VULKAN_INCLUDE_DIR}, ${GLSLANG_VALIDATOR})")
  file(GLOB PG_GPU_KERNELS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pg/gpu/shaders/*.comp)
  file(GLOB PG_GPU_INCLUDES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pg/gpu/shaders/*.glsl)
  set(PG_GPU_SPIRV "")
  set(PG_GPU_KERNEL_INCLUDES "")
  set(PG_GPU_KERNEL_TABLE "")
  # Each kernel twice: as it is, and keeping subnormal numbers, for the
  # devices that can be asked to (shaders/keep.glsl).
  foreach(kernel_file ${PG_GPU_KERNELS})
    get_filename_component(kernel ${kernel_file} NAME_WE)
    set(spirv ${CMAKE_CURRENT_BINARY_DIR}/generated/gpu/${kernel}.spv.h)
    set(keep ${CMAKE_CURRENT_BINARY_DIR}/generated/gpu/${kernel}.keep.spv.h)
    add_custom_command(OUTPUT ${spirv} ${keep}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated/gpu
      COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn kSpirv_${kernel} -o ${spirv} ${kernel_file}
      COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 -DPG_KEEP_SUBNORMALS --vn kSpirvKeep_${kernel}
              -o ${keep} ${kernel_file}
      DEPENDS ${kernel_file} ${PG_GPU_INCLUDES}
      COMMENT "SPIR-V of ${kernel}.comp"
      VERBATIM)
    list(APPEND PG_GPU_SPIRV ${spirv} ${keep})
    string(APPEND PG_GPU_KERNEL_INCLUDES "#include \"gpu/${kernel}.spv.h\"\n#include \"gpu/${kernel}.keep.spv.h\"\n")
    string(APPEND PG_GPU_KERNEL_TABLE "    {\"${kernel}\", kSpirv_${kernel}, sizeof kSpirv_${kernel} / sizeof kSpirv_${kernel}[0], "
                                      "kSpirvKeep_${kernel}, sizeof kSpirvKeep_${kernel} / sizeof kSpirvKeep_${kernel}[0]},\n")
  endforeach()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/GpuShaders.cpp.tmp
"// Generated by CMake from src/pg/gpu/shaders/*.comp. Do not edit.
#include \"pg/gpu/Shaders.h\"

#include <cstdint>

${PG_GPU_KERNEL_INCLUDES}
namespace pg::gpu {
const Spirv kSpirv[] = {
${PG_GPU_KERNEL_TABLE}};
const size_t kSpirvCount = sizeof kSpirv / sizeof kSpirv[0];
}  // namespace pg::gpu
")
  configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/GpuShaders.cpp.tmp
                 ${CMAKE_CURRENT_BINARY_DIR}/generated/GpuShaders.cpp COPYONLY)
  add_library(pggpu
    src/pg/gpu/Gpu.cpp
    src/pg/gpu/SelfTest.cpp
    src/pg/gpu/Vulkan.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/GpuShaders.cpp
    ${PG_GPU_SPIRV}
  )
  target_include_directories(pggpu PRIVATE ${PG_VULKAN_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(pggpu PUBLIC pgcore ${CMAKE_DL_LIBS})
  target_compile_definitions(pggpu PUBLIC PG_HAVE_VULKAN)
  if(MSVC)
    target_compile_options(pggpu PRIVATE /W4)
  else()
    target_compile_options(pggpu PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# --- shader graph ------------------------------------------------------------
#
# A network type of its own: nodes that produce shader code instead of
# geometry. Dependency-free like the core. The built-in node library is a text
# file (src/pg/shader/builtin.pgnodes); it is compiled in as bytes, so the tools
# need no data files and the binary can never disagree with the file.

set(PG_BUILTIN_NODES ${CMAKE_CURRENT_SOURCE_DIR}/src/pg/shader/builtin.pgnodes)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PG_BUILTIN_NODES})
file(READ ${PG_BUILTIN_NODES} PG_BUILTIN_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," PG_BUILTIN_BYTES "${PG_BUILTIN_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinLibrary.cpp.tmp
"// Generated by CMake from src/pg/shader/builtin.pgnodes. Do not edit.
#include \"pg/shader/NodeLibrary.h\"

namespace pg::shader {
namespace {
const char kBuiltin[] = {${PG_BUILTIN_BYTES} 0x00};
}  // namespace
const char* builtinLibrarySource() { return kBuiltin; }
}  // namespace pg::shader
")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinLibrary.cpp.tmp
               ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinLibrary.cpp COPYONLY)

add_library(pgshadergraph
  src/pg/shader/Types.cpp
  src/pg/shader/NodeLibrary.cpp
  src/pg/shader/ShaderGraph.cpp
  src/pg/shader/Target.cpp
  src/pg/shader/Generator.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinLibrary.cpp
)
target_include_directories(pgshadergraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(pgshadergraph PRIVATE /W4)
else()
  target_compile_options(pgshadergraph PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(PG_SANITIZE)
  target_compile_options(pgshadergraph PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(pgshadergraph PUBLIC -fsanitize=address,undefined)
endif()
if(PG_SANITIZE_THREAD)
  target_compile_options(pgshadergraph PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(pgshadergraph PUBLIC -fsanitize=thread)
endif()

# --- simulation --------------------------------------------------------------
#
# Smoke and fire as a gas on a 3D grid (src/pg/sim). Dependency-free; its
# loops run on the core's deterministic pg::parallelFor.

# The example networks (examples/sim/*.pgsim) are compiled in the same way as
# the built-in shader nodes: `prototype pyro fire` needs no file next to it.
file(GLOB PG_SIM_EXAMPLES ${CMAKE_CURRENT_SOURCE_DIR}/examples/sim/*.pgsim)
list(SORT PG_SIM_EXAMPLES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/examples/sim ${PG_SIM_EXAMPLES})
set(PG_SIM_EXAMPLE_ARRAYS "")
set(PG_SIM_EXAMPLE_TABLE "")
set(PG_SIM_EXAMPLE_INDEX 0)
foreach(example ${PG_SIM_EXAMPLES})
  get_filename_component(example_name ${example} NAME_WE)
  file(READ ${example} example_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," example_bytes "${example_hex}")
  string(APPEND PG_SIM_EXAMPLE_ARRAYS "const char kExample${PG_SIM_EXAMPLE_INDEX}[] = {${example_bytes} 0x00};\n")
  string(APPEND PG_SIM_EXAMPLE_TABLE "        {\"${example_name}\", kExample${PG_SIM_EXAMPLE_INDEX}},\n")
  math(EXPR PG_SIM_EXAMPLE_INDEX "${PG_SIM_EXAMPLE_INDEX} + 1")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/SimExamples.cpp.tmp
"// Generated by CMake from examples/sim/*.pgsim. Do not edit.
#include <vector>

namespace pg::sim {
struct EmbeddedExample {
    const char* name;
    const char* text;
};
namespace {
${PG_SIM_EXAMPLE_ARRAYS}}  // namespace
const std::vector<EmbeddedExample>& embeddedExamples() {
    static const std::vector<EmbeddedExample> examples = {
${PG_SIM_EXAMPLE_TABLE}    };
    return examples;
}
}  // namespace pg::sim
")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/SimExamples.cpp.tmp
               ${CMAKE_CURRENT_BINARY_DIR}/generated/SimExamples.cpp COPYONLY)

# The digital assets the program carries (examples/assets/*.pgasset), the same way.
file(GLOB PG_SIM_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/*.pgasset)
list(SORT PG_SIM_ASSETS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/examples/assets ${PG_SIM_ASSETS})
set(PG_SIM_ASSET_ARRAYS "")
set(PG_SIM_ASSET_TABLE "")
set(PG_SIM_ASSET_INDEX 0)
foreach(asset ${PG_SIM_ASSETS})
  get_filename_component(asset_name ${asset} NAME_WE)
  file(READ ${asset} asset_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," asset_bytes "${asset_hex}")
  string(APPEND PG_SIM_ASSET_ARRAYS "const char kAsset${PG_SIM_ASSET_INDEX}[] = {${asset_bytes} 0x00};\n")
  string(APPEND PG_SIM_ASSET_TABLE "        {\"${asset_name}\", kAsset${PG_SIM_ASSET_INDEX}},\n")
  math(EXPR PG_SIM_ASSET_INDEX "${PG_SIM_ASSET_INDEX} + 1")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/SimAssets.cpp.tmp
"// Generated by CMake from examples/assets/*.pgasset. Do not edit.
#include <vector>

namespace pg::sim {
struct EmbeddedExample {
    const char* name;
    const char* text;
};
namespace {
${PG_SIM_ASSET_ARRAYS}}  // namespace
const std::vector<EmbeddedExample>& embeddedAssets() {
    static const std::vector<EmbeddedExample> assets = {
${PG_SIM_ASSET_TABLE}    };
    return assets;
}
}  // namespace pg::sim
")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/SimAssets.cpp.tmp
               ${CMAKE_CURRENT_BINARY_DIR}/generated/SimAssets.cpp COPYONLY)

add_library(pgsim
  src/pg/sim/Asset.cpp
  src/pg/sim/ForEach.cpp
  src/pg/sim/Compiler.cpp
  src/pg/sim/Cooker.cpp
  src/pg/sim/Rigid.cpp
  src/pg/sim/Camera.cpp
  src/pg/sim/Frame.cpp
  src/pg/sim/FrameStore.cpp
  src/pg/sim/WaterMesh.cpp
  src/pg/sim/FreeSurface.cpp
  src/pg/sim/Cache.cpp
  src/pg/sim/Cloth.cpp
  src/pg/sim/Grains.cpp
  src/pg/sim/Display.cpp
  src/pg/sim/Prepared.cpp
  src/pg/sim/GeometryGraph.cpp
  src/pg/sim/Grid.cpp
  src/pg/sim/SparseGrid.cpp
  src/pg/sim/State.cpp
  src/pg/sim/Liquid.cpp
  src/pg/sim/Mesh.cpp
  src/pg/sim/Network.cpp
  src/pg/sim/Poisson.cpp
  src/pg/sim/Pyro.cpp
  src/pg/sim/PyroGpu.cpp
  src/pg/sim/Rain.cpp
  src/pg/sim/Scene.cpp
  src/pg/sim/Shape.cpp
  src/pg/sim/Upres.cpp
  src/pg/sim/World.cpp
  src/pg/sim/UsdExport.cpp
  src/pg/sim/AbcExport.cpp
  src/pg/sim/VdbGas.cpp
  src/pg/render/Bvh.cpp
  src/pg/render/Denoise.cpp
  src/pg/render/Embree.cpp
  src/pg/render/Gas.cpp
  src/pg/render/Scene.cpp
  src/pg/render/Particles.cpp
  src/pg/render/PathTracer.cpp
  src/pg/render/Aces.cpp
  src/pg/render/Ocio.cpp
  src/pg/render/Plate.cpp
  src/pg/render/Save.cpp
  src/pg/render/Textures.cpp
  src/pg/render/MaterialGraph.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/SimExamples.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/SimAssets.cpp
)
target_link_libraries(pgsim PUBLIC pgcore)
if(PG_WITH_VULKAN)
  # The gas solver's share of the GPU (PyroGpu.cpp).
  target_link_libraries(pgsim PRIVATE pggpu)
endif()
# The photographs of the materials that come with the program (render/Textures.h).
target_compile_definitions(pgsim PRIVATE PG_TEXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/textures")
if(MSVC)
  target_compile_options(pgsim PRIVATE /W4)
else()
  target_compile_options(pgsim PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Rigid bodies (src/pg/sim/Rigid.cpp): Jolt Physics, fetched and built as a
# static library -- deterministic across platforms, without the instruction
# sets a machine may lack (AVX and later), with nothing but the library.
if(PG_WITH_JOLT)
  include(FetchContent)
  # No GPU compute either (JPH_USE_VK and the others): with a Vulkan or
  # DirectX SDK about, Jolt would build its compute shaders with dxc, which
  # the machine need not have; the rigid bodies run on the CPU.
  foreach(off TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
              ENABLE_ALL_WARNINGS INTERPROCEDURAL_OPTIMIZATION USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT
              USE_TZCNT USE_F16C USE_FMADD DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
              ENABLE_OBJECT_STREAM JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL ENABLE_INSTALL)
    set(${off} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
  # With C++ RTTI, as the rest of the program: the sanitizers check vtables.
  set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
  FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build)
  FetchContent_MakeAvailable(JoltPhysics)
  # Its headers as the system's: our warnings are not about them.
  get_target_property(PG_JOLT_INCLUDES Jolt INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PG_JOLT_INCLUDES}")
  target_link_libraries(pgsim PRIVATE Jolt)
  # Jolt's jobs run on threads of its own and sync through its atomics:
  # under the thread sanitizer it is built with it too, or the sanitizer
  # does not see them and reports races that are not.
  if(PG_SANITIZE_THREAD)
    target_compile_options(Jolt PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  endif()
  target_compile_definitions(pgsim PRIVATE PG_HAVE_JOLT)
endif()

# The path tracer's rays (src/pg/render/Embree.cpp): Intel Embree 4 (Apache
# 2.0) -- hierarchies built and walked with SIMD -- faster than our own
# (src/pg/render/Bvh.h), which stays for a build without it. The system's:
# FreeBSD's graphics/embree, libembree-dev on Debian 13 and Ubuntu 24.04.
# Else fetched and built with what the path tracer needs alone -- triangles
# and instances, SSE2 to AVX2, its own threads -- once, for some minutes.
option(PG_EMBREE "Path tracer rays through Intel Embree 4 (Apache 2.0), the system's or fetched; OFF: our own BVH" ON)
if(PG_EMBREE AND PG_SANITIZE_THREAD)
  # Embree and the TBB under it are not built with the thread sanitizer,
  # which cannot follow how their threads hand memory to each other and
  # reports races that are not: under it, our own BVH.
  message(STATUS "Thread sanitizer: the path tracer uses its own BVH, not Embree")
  set(PG_EMBREE OFF)
endif()
if(PG_EMBREE)
  find_package(embree 4 CONFIG QUIET)
  if(TARGET embree)
    message(STATUS "Embree ${embree_VERSION}: the system's")
  else()
    include(FetchContent)
    foreach(off EMBREE_TUTORIALS EMBREE_ISPC_SUPPORT EMBREE_ZIP_MODE EMBREE_FILTER_FUNCTION EMBREE_RAY_MASK
                EMBREE_RAY_PACKETS EMBREE_GEOMETRY_QUAD EMBREE_GEOMETRY_CURVE EMBREE_GEOMETRY_SUBDIVISION
                EMBREE_GEOMETRY_USER EMBREE_GEOMETRY_GRID EMBREE_GEOMETRY_POINT EMBREE_GEOMETRY_INSTANCE_ARRAY)
      set(${off} OFF CACHE BOOL "" FORCE)
    endforeach()
    foreach(on EMBREE_STATIC_LIB EMBREE_GEOMETRY_TRIANGLE EMBREE_GEOMETRY_INSTANCE)
      set(${on} ON CACHE BOOL "" FORCE)
    endforeach()
    set(EMBREE_TASKING_SYSTEM INTERNAL CACHE STRING "" FORCE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
      # Kernels from SSE2 to AVX2 -- the AVX2 ones build on the AVX ones --
      # without AVX-512, which few machines have and which takes long to build.
      set(EMBREE_MAX_ISA AVX2 CACHE STRING "" FORCE)
    endif()
    FetchContent_Declare(embree
      GIT_REPOSITORY https://github.com/RenderKit/embree.git
      GIT_TAG v4.4.1
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(embree)
    if(TARGET embree)
      message(STATUS "Embree 4.4.1: fetched")
      # Its headers as the system's: our warnings are not about them.
      get_target_property(PG_EMBREE_INCLUDES embree INTERFACE_INCLUDE_DIRECTORIES)
      set_target_properties(embree PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PG_EMBREE_INCLUDES}")
    else()
      message(WARNING "Embree could not be had: the path tracer uses its own BVH. Install it (FreeBSD: pkg install "
                      "embree; Debian, Ubuntu: apt install libembree-dev) or point FETCHCONTENT_SOURCE_DIR_EMBREE "
                      "at a checkout of github.com/RenderKit/embree 4.4.1.")
      set(PG_EMBREE OFF)
    endif()
  endif()
endif()
if(PG_EMBREE)
  target_link_libraries(pgsim PRIVATE embree)
  target_compile_definitions(pgsim PRIVATE PG_HAVE_EMBREE)
endif()

# The gas as the path tracer goes through it (src/pg/render/Gas.cpp): a
# NanoVDB grid -- OpenVDB's sparse volume flattened into one block of memory
# (Apache 2.0) -- headers alone, nothing to build. The system's when they
# compile here as Gas.cpp takes them (OpenVDB 12 and later lay them out so);
# else fetched with OpenVDB 13.0.0, none of whose CMake runs. Not 13.1: its
# NanoVDB takes std::atomic_ref under C++20, which libc++ 18 -- FreeBSD 14's
# -- does not have.
option(PG_NANOVDB "Path tracer renders the gas through NanoVDB (headers only), the system's or fetched; OFF: without the gas" ON)
if(PG_NANOVDB)
  find_path(PG_NANOVDB_SYSTEM_DIR nanovdb/tools/CreateNanoGrid.h)
  set(PG_NANOVDB_INCLUDE_DIR "")
  if(PG_NANOVDB_SYSTEM_DIR)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${PG_NANOVDB_SYSTEM_DIR}")
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles([[
      #include <nanovdb/NanoVDB.h>
      #include <nanovdb/tools/CreateNanoGrid.h>
      #include <nanovdb/tools/GridBuilder.h>
      int main() {
        nanovdb::tools::build::Grid<nanovdb::Vec3f> build(nanovdb::Vec3f(0.0f));
        auto handle = nanovdb::tools::createNanoGrid<decltype(build), nanovdb::Vec3f>(
            build, nanovdb::tools::StatsMode::BBox, nanovdb::CheckMode::Disable);
        const nanovdb::Vec3fGrid* grid = handle.grid<nanovdb::Vec3f>();
        nanovdb::Vec3fGrid::AccessorType acc = grid->getAccessor();
        const auto* leaf = acc.probeLeaf(nanovdb::Coord(0, 0, 0));
        return static_cast<int>(handle.bufferSize() == 0) + (leaf ? static_cast<int>(leaf->getValue(0u)[0]) : 0);
      }]] PG_NANOVDB_SYSTEM_WORKS)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_QUIET)
    if(PG_NANOVDB_SYSTEM_WORKS)
      set(PG_NANOVDB_INCLUDE_DIR "${PG_NANOVDB_SYSTEM_DIR}")
      message(STATUS "NanoVDB: the system's (${PG_NANOVDB_SYSTEM_DIR})")
    else()
      message(STATUS "NanoVDB: the system's (${PG_NANOVDB_SYSTEM_DIR}) does not compile here; fetching 13.0.0")
    endif()
  endif()
  if(NOT PG_NANOVDB_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(openvdb
      GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openvdb.git
      GIT_TAG v13.0.0
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR nanovdb/headers-only)  # no CMakeLists.txt there: its build is not ours
    FetchContent_MakeAvailable(openvdb)
    if(EXISTS "${openvdb_SOURCE_DIR}/nanovdb/nanovdb/tools/CreateNanoGrid.h")
      set(PG_NANOVDB_INCLUDE_DIR "${openvdb_SOURCE_DIR}/nanovdb")
      message(STATUS "NanoVDB: fetched with OpenVDB 13.0.0")
    else()
      message(WARNING "NanoVDB could not be had: the path tracer renders without the gas. Install OpenVDB 12 or "
                      "newer with NanoVDB's headers, or point FETCHCONTENT_SOURCE_DIR_OPENVDB at a checkout of "
                      "github.com/AcademySoftwareFoundation/openvdb 13.0.0.")
      set(PG_NANOVDB OFF)
    endif()
  endif()
endif()
if(PG_NANOVDB)
  target_include_directories(pgsim SYSTEM PRIVATE "${PG_NANOVDB_INCLUDE_DIR}")
  target_compile_definitions(pgsim PRIVATE PG_HAVE_NANOVDB)
endif()

# Denoising (src/pg/render/Denoise.cpp): Intel Open Image Denoise 2 (Apache
# 2.0), a neural network trained on path-traced pictures. The system's:
# FreeBSD's graphics/oidn, the packages of other systems. Else built from
# its sources once -- statically, the CPU device alone, in a minute or two
# -- which takes ISPC and TBB (Debian, Ubuntu: apt install ispc
# libtbb-dev): 2.3.3, the newest the ISPC of Ubuntu 24.04 (1.22) builds --
# 2.4 wants ISPC 1.29, 2.5 1.30. Without it, our own filter.
option(PG_OIDN "Denoise with Intel Open Image Denoise 2: the system's, else built from its sources (needs ISPC and TBB); OFF: our own filter" ON)
if(PG_OIDN AND (PG_SANITIZE OR PG_SANITIZE_THREAD))
  # Its TBB is not built with the sanitizers -- and binds its threads through
  # a library it opens in a way the address sanitizer cannot follow.
  message(STATUS "Sanitizers: our own denoising filter, not Open Image Denoise")
  set(PG_OIDN OFF)
endif()
if(PG_OIDN)
  find_package(OpenImageDenoise 2 CONFIG QUIET)
  if(TARGET OpenImageDenoise)
    message(STATUS "Open Image Denoise ${OpenImageDenoise_VERSION}: the system's")
  else()
    find_program(PG_ISPC ispc)
    find_package(TBB CONFIG QUIET)
    # ISPC 1.21 or newer: an older one would stop Open Image Denoise's
    # configuration, and ours with it.
    set(PG_ISPC_VERSION "")
    if(PG_ISPC)
      execute_process(COMMAND "${PG_ISPC}" --version OUTPUT_VARIABLE PG_ISPC_TEXT ERROR_QUIET)
      string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" PG_ISPC_VERSION "${PG_ISPC_TEXT}")
    endif()
    # Its sources build on Linux, Windows and macOS; on FreeBSD its port
    # (pkg install oidn) is the way.
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|Darwin)$" AND PG_ISPC_VERSION AND
       PG_ISPC_VERSION VERSION_GREATER_EQUAL 1.21 AND TARGET TBB::tbb)
      include(FetchContent)
      foreach(off OIDN_APPS OIDN_FILTER_RTLIGHTMAP OIDN_DEVICE_SYCL OIDN_DEVICE_CUDA OIDN_DEVICE_HIP OIDN_DEVICE_METAL
                  OIDN_INSTALL_DEPENDENCIES)
        set(${off} OFF CACHE BOOL "" FORCE)
      endforeach()
      foreach(on OIDN_STATIC_LIB OIDN_DEVICE_CPU OIDN_FILTER_RT)
        set(${on} ON CACHE BOOL "" FORCE)
      endforeach()
      set(ISPC_EXECUTABLE "${PG_ISPC}" CACHE FILEPATH "" FORCE)
      if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)  # the files as unpacked now: rebuilt if the archive changes
      endif()
      FetchContent_Declare(oidn
        URL https://github.com/RenderKit/oidn/releases/download/v2.3.3/oidn-2.3.3.src.tar.gz
        URL_HASH SHA256=ccf221535b4007607fb53d3ff5afa74de25413bb8ef5d03d215f46c7cc2f96cf)
      FetchContent_MakeAvailable(oidn)
      if(TARGET OpenImageDenoise)
        message(STATUS "Open Image Denoise 2.3.3: built from its sources")
        # Its headers as the system's: our warnings are not about them.
        get_target_property(PG_OIDN_INCLUDES OpenImageDenoise INTERFACE_INCLUDE_DIRECTORIES)
        set_target_properties(OpenImageDenoise PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PG_OIDN_INCLUDES}")
      endif()
    endif()
    if(NOT TARGET OpenImageDenoise)
      message(WARNING "Open Image Denoise could not be had: our own filter takes the noise out. Install it "
                      "(FreeBSD: pkg install oidn), or ISPC 1.21 or newer and TBB to build it (Debian, Ubuntu: "
                      "apt install ispc libtbb-dev).")
      set(PG_OIDN OFF)
    endif()
  endif()
endif()
if(PG_OIDN)
  target_link_libraries(pgsim PRIVATE OpenImageDenoise)
  target_compile_definitions(pgsim PRIVATE PG_HAVE_OIDN)
endif()

# Rendering with Cycles (src/pg/render/Cycles.cpp): Blender's renderer
# (Apache 2.0), as a library -- what the Render tab and `--renderer cycles`
# render with. No system has it packaged on its own: it is built from its
# sources -- a git checkout of 5.2.0 -- by its own CMake, in a directory of
# its own (cycles-v5.2.0-build), once, in a few minutes: its libraries
# alone, the processor's device, with the Embree, the Open Image Denoise
# and the NanoVDB headers we have -- Cycles 5 reads volumes as NanoVDB
# grids alone. It needs OpenImageIO and TBB (FreeBSD: pkg install
# openimageio pugixml onetbb; Debian, Ubuntu: apt install
# libopenimageio-dev libpugixml-dev libtbb-dev). Our code compiles against
# it with the definitions and the flags its own code has, and links what
# its own program links, both read from its build. Without it, the path
# tracer renders.
option(PG_CYCLES "Render with Cycles, Blender's renderer, built from its sources (needs OpenImageIO and TBB)" ON)
if(PG_CYCLES AND PG_SANITIZE_THREAD)
  # As Embree: its TBB is not built with the thread sanitizer.
  message(STATUS "Thread sanitizer: no Cycles, the path tracer renders")
  set(PG_CYCLES OFF)
endif()
if(PG_CYCLES AND MSVC)
  message(STATUS "Cycles: not built with Visual Studio here; the path tracer renders")
  set(PG_CYCLES OFF)
endif()
if(PG_CYCLES)
  # Found as Cycles finds it: its CMake files of some systems name programs
  # a package of their own installs.
  find_path(PG_OIIO_INCLUDE_DIR OpenImageIO/imageio.h)
  find_library(PG_OIIO_LIBRARY NAMES OpenImageIO)
  find_library(PG_OIIO_UTIL_LIBRARY NAMES OpenImageIO_Util)
  if(NOT PG_OIIO_INCLUDE_DIR OR NOT PG_OIIO_LIBRARY OR NOT PG_OIIO_UTIL_LIBRARY)
    message(WARNING "Cycles needs OpenImageIO: the path tracer renders. Install it (FreeBSD: pkg install "
                    "openimageio pugixml onetbb; Debian, Ubuntu: apt install libopenimageio-dev libpugixml-dev "
                    "libtbb-dev).")
    set(PG_CYCLES OFF)
  else()
    # A system's OpenImageIO built against another standard library (libc++
    # here, libstdc++ there) does not link: its strings are not ours.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${PG_OIIO_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${PG_OIIO_LIBRARY} ${PG_OIIO_UTIL_LIBRARY})
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("
      #include <OpenImageIO/imageio.h>
      #include <string>
      int main() { auto in = OIIO::ImageInput::open(std::string(\"none.png\")); return in ? 1 : 0; }"
      PG_CYCLES_OIIO_LINKS)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_QUIET)
    if(NOT PG_CYCLES_OIIO_LINKS)
      message(WARNING "Cycles: the system's OpenImageIO does not link with this compiler's standard library: "
                      "the path tracer renders.")
      set(PG_CYCLES OFF)
    endif()
  endif()
endif()
if(PG_CYCLES)
  include(FetchContent)
  set(PG_CYCLES_TAG v5.2.0)
  # Each version in directories of its own: the sources of another, which
  # the build changed, are not checked out anew, nor built over.
  FetchContent_Declare(cycles
    GIT_REPOSITORY https://github.com/blender/cycles.git
    GIT_TAG ${PG_CYCLES_TAG}
    GIT_SHALLOW TRUE
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/cycles-${PG_CYCLES_TAG}-src
    SOURCE_SUBDIR pg-cycles-builds-on-its-own)  # its CMake runs below, not as part of ours
  FetchContent_MakeAvailable(cycles)
  # A few lines of its sources changed, once each: `mark` in the change, so
  # that a file that has it is left as it is; the change `after` the text it
  # follows, `instead` of it with `replace`.
  function(pg_change_cycles file mark anchor change why)
    set(path ${cycles_SOURCE_DIR}/${file})
    file(READ ${path} text)
    if(text MATCHES "${mark}")
      return()
    endif()
    string(FIND "${text}" "${anchor}" at)
    if(at EQUAL -1)
      message(WARNING "Cycles: ${file} is not as expected -- ${why}")
      return()
    endif()
    string(REPLACE "${anchor}" "${change}" text "${text}")
    file(WRITE ${path} "${text}")
  endfunction()
  # Our gas goes to Cycles as a mesh round its voxels -- Cycles makes its own
  # volumes' meshes with OpenVDB, which it is built without -- and Cycles
  # blurs only its own volumes along their velocity. Five lines of its
  # object manager let such a mesh's volume move as its velocity attribute
  # says too (render/Cycles.cpp, gas()).
  set(anchor "    else if (mesh->attributes.find(ATTR_STD_CORNER_NORMAL)) {\n      flag |= SD_OBJECT_HAS_CORNER_NORMALS;\n    }\n")
  pg_change_cycles(src/scene/object.cpp "pg: a mesh round voxels" "${anchor}"
    "${anchor}    /* pg: a mesh round voxels, its volume moving as their velocity says. */\n    if (mesh->attributes.find(ATTR_STD_VOLUME_VELOCITY)) {\n      flag |= SD_OBJECT_HAS_VOLUME_MOTION;\n      kobject.velocity_scale = 1.0f;\n    }\n"
    "the gas renders without motion blur")
  # Cycles 5 bounds how much a volume stops in each node of an octree by its
  # value at the middle of the shutter alone: where moving gas is blurred
  # to, it would not look. Read over the whole shutter (render/Cycles.cpp,
  # gas()).
  set(anchor "    sd.P = ray.P + rand_p * voxel_size;\n")
  pg_change_cycles(src/kernel/bake/bake.h "pg: over the whole shutter" "${anchor}"
    "${anchor}    /* pg: over the whole shutter, where moving gas is blurred to too. */\n    sd.time = (num_samples > 1) ? float(sample) / float(num_samples - 1) : 0.5f;\n"
    "moving gas is blurred less far than it goes")
  # Cycles 5 has OpenColorIO always; we manage colour ourselves (Color.h) and
  # hand it linear light, so it is built without, as before: no package more
  # to install.
  pg_change_cycles(src/cmake/external_libs.cmake "pg: without OpenColorIO"
    "if(NOT USD_OVERRIDE_OPENCOLORIO)\n"
    "if(FALSE) # pg: without OpenColorIO\n" "it needs OpenColorIO")
  pg_change_cycles(src/cmake/dependency_targets.cmake "pg: without OpenColorIO"
    "add_library(bf::dependencies::opencolorio ALIAS OpenColorIO::OpenColorIO)\n"
    "add_library(bf_deps_opencolorio INTERFACE) # pg: without OpenColorIO\nadd_library(bf::dependencies::opencolorio ALIAS bf_deps_opencolorio)\n"
    "it needs OpenColorIO")
  pg_change_cycles(src/CMakeLists.txt "pg: without OpenColorIO"
    "add_definitions(-DWITH_OCIO)\n" "# pg: without OpenColorIO\n" "it needs OpenColorIO")
  # Cycles 5 reads volumes as NanoVDB grids alone; its own code that makes
  # them does so from OpenVDB's, which it is built without -- ours makes
  # them (render/Cycles.cpp, gas()), with the NanoVDB headers above.
  foreach(file src/util/nanovdb.h src/util/nanovdb.cpp src/scene/image_vdb.cpp)
    pg_change_cycles(${file} "pg: NanoVDB without OpenVDB" "#ifdef WITH_NANOVDB\n"
      "#if defined(WITH_NANOVDB) && defined(WITH_OPENVDB) /* pg: NanoVDB without OpenVDB */\n"
      "the gas may not render")
  endforeach()
  set(PG_CYCLES_DIR ${CMAKE_BINARY_DIR}/cycles-${PG_CYCLES_TAG}-build)
  # Cycles finds OpenImageIO by its CMake package, whose files on some
  # systems name programs another package of theirs installs (Debian's
  # OpenImageIOTargets.cmake: /usr/bin/iconvert): one of ours instead, of
  # the libraries found above.
  set(PG_CYCLES_OIIO_DIR ${CMAKE_BINARY_DIR}/cycles-openimageio)
  file(WRITE ${PG_CYCLES_OIIO_DIR}/OpenImageIOConfig.cmake
       "if(NOT TARGET OpenImageIO::OpenImageIO)\n"
       "  add_library(OpenImageIO::OpenImageIO_Util UNKNOWN IMPORTED)\n"
       "  set_target_properties(OpenImageIO::OpenImageIO_Util PROPERTIES IMPORTED_LOCATION \"${PG_OIIO_UTIL_LIBRARY}\"\n"
       "                        INTERFACE_INCLUDE_DIRECTORIES \"${PG_OIIO_INCLUDE_DIR}\")\n"
       "  add_library(OpenImageIO::OpenImageIO UNKNOWN IMPORTED)\n"
       "  set_target_properties(OpenImageIO::OpenImageIO PROPERTIES IMPORTED_LOCATION \"${PG_OIIO_LIBRARY}\"\n"
       "                        INTERFACE_INCLUDE_DIRECTORIES \"${PG_OIIO_INCLUDE_DIR}\"\n"
       "                        INTERFACE_LINK_LIBRARIES OpenImageIO::OpenImageIO_Util)\n"
       "endif()\n"
       "set(OPENIMAGEIO_INCLUDE_DIR \"${PG_OIIO_INCLUDE_DIR}\")\n")
  set(PG_CYCLES_ARGS
    -DOpenImageIO_DIR=${PG_CYCLES_OIIO_DIR}
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
    -DWITH_LIBS_PRECOMPILED=OFF
    -DWITH_CYCLES_ALEMBIC=OFF -DWITH_CYCLES_OPENCOLORIO=OFF -DWITH_CYCLES_OPENSUBDIV=OFF -DWITH_CYCLES_OPENVDB=OFF
    -DWITH_CYCLES_OSL=OFF -DWITH_CYCLES_USD=OFF -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=OFF
    -DWITH_CYCLES_LOGGING=OFF -DWITH_CYCLES_STANDALONE_GUI=OFF
    -DWITH_CYCLES_DEVICE_CUDA=OFF -DWITH_CYCLES_DEVICE_OPTIX=OFF -DWITH_CYCLES_DEVICE_HIP=OFF
    -DWITH_CYCLES_DEVICE_METAL=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF)
  if(CMAKE_C_COMPILER)
    # Ours, where a project of ours has C on; else Cycles finds its own.
    list(APPEND PG_CYCLES_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
  endif()
  if(PG_EMBREE AND NOT DEFINED embree_SOURCE_DIR)
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_EMBREE=ON)
  else()
    # Embree fetched and built static with only what our rays use: Cycles
    # goes through its own hierarchy.
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_EMBREE=OFF)
  endif()
  if(PG_NANOVDB)
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_NANOVDB=ON -DNANOVDB_INCLUDE_DIR=${PG_NANOVDB_INCLUDE_DIR})
  else()
    message(WARNING "Cycles: without NanoVDB it renders no gas")
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_NANOVDB=OFF)
  endif()
  if(PG_OIDN)
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_OPENIMAGEDENOISE=ON)
    if(DEFINED oidn_SOURCE_DIR)
      # Ours, built from its sources: its headers to compile against; the
      # library is linked into our programs.
      list(APPEND PG_CYCLES_ARGS -DOPENIMAGEDENOISE_INCLUDE_DIR=${oidn_SOURCE_DIR}/include
           -DOPENIMAGEDENOISE_LIBRARY=${oidn_BINARY_DIR}/libOpenImageDenoise.a
           -DOPENIMAGEDENOISE_OPENIMAGEDENOISE_LIBRARY=${oidn_BINARY_DIR}/libOpenImageDenoise.a)
    endif()
  else()
    list(APPEND PG_CYCLES_ARGS -DWITH_CYCLES_OPENIMAGEDENOISE=OFF)
  endif()
  # Configured when what it is configured with changes: makefiles, so that
  # what it compiles and links with can be read.
  string(SHA1 PG_CYCLES_KEY "${cycles_SOURCE_DIR};${PG_CYCLES_TAG};${PG_CYCLES_ARGS}")
  set(PG_CYCLES_STAMP ${PG_CYCLES_DIR}/pg-configured)
  set(PG_CYCLES_CONFIGURED "")
  if(EXISTS ${PG_CYCLES_STAMP})
    file(READ ${PG_CYCLES_STAMP} PG_CYCLES_CONFIGURED)
  endif()
  if(NOT PG_CYCLES_CONFIGURED STREQUAL PG_CYCLES_KEY)
    message(STATUS "Cycles: configuring its build")
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${cycles_SOURCE_DIR} -B ${PG_CYCLES_DIR} -G "Unix Makefiles"
                            ${PG_CYCLES_ARGS}
                    RESULT_VARIABLE PG_CYCLES_RESULT OUTPUT_VARIABLE PG_CYCLES_OUT ERROR_VARIABLE PG_CYCLES_ERR)
    if(PG_CYCLES_RESULT EQUAL 0)
      file(WRITE ${PG_CYCLES_STAMP} "${PG_CYCLES_KEY}")
    else()
      string(REGEX MATCH "[^\n]*Error[^\n]*\n[^\n]*\n[^\n]*" PG_CYCLES_WHY "${PG_CYCLES_ERR}")
      message(WARNING "Cycles could not be configured: the path tracer renders.\n${PG_CYCLES_WHY}")
      set(PG_CYCLES OFF)
    endif()
  endif()
endif()
if(PG_CYCLES)
  # What its own code compiles with -- definitions, include directories,
  # instruction sets -- and what its own program links, in its order.
  file(STRINGS ${PG_CYCLES_DIR}/src/session/CMakeFiles/cycles_session.dir/flags.make PG_CYCLES_FLAGS)
  set(PG_CYCLES_DEFINES "")
  set(PG_CYCLES_INCLUDES "")
  set(PG_CYCLES_OPTIONS "")
  foreach(line IN LISTS PG_CYCLES_FLAGS)
    if(line MATCHES "^CXX_DEFINES = (.*)")
      string(REGEX MATCHALL "-D[A-Za-z_][A-Za-z_0-9]*(=[^ \"]*)?" defs "${CMAKE_MATCH_1}")
      foreach(d IN LISTS defs)
        if(NOT d MATCHES "^-D(CCL_NAMESPACE_|NDEBUG)")
          string(SUBSTRING "${d}" 2 -1 d)
          list(APPEND PG_CYCLES_DEFINES "${d}")
        endif()
      endforeach()
    elseif(line MATCHES "^CXX_INCLUDES = (.*)")
      string(REGEX MATCHALL "(-I|-isystem )[^ ]+" incs "${CMAKE_MATCH_1}")
      foreach(i IN LISTS incs)
        string(REGEX REPLACE "^(-I|-isystem )" "" i "${i}")
        list(APPEND PG_CYCLES_INCLUDES "${i}")
      endforeach()
    elseif(line MATCHES "^CXX_FLAGS = (.*)")
      # Its instruction sets: the -m flags, whole words.
      string(REGEX MATCHALL " -m[^ ]+" opts " ${CMAKE_MATCH_1}")
      foreach(o IN LISTS opts)
        string(STRIP "${o}" o)
        list(APPEND PG_CYCLES_OPTIONS "${o}")
      endforeach()
    endif()
  endforeach()
  file(READ ${PG_CYCLES_DIR}/src/app/CMakeFiles/cycles.dir/link.txt PG_CYCLES_LINK)
  string(REGEX REPLACE ".* -o [^ ]+ " "" PG_CYCLES_LINK "${PG_CYCLES_LINK}")
  string(STRIP "${PG_CYCLES_LINK}" PG_CYCLES_LINK)
  separate_arguments(PG_CYCLES_LINK UNIX_COMMAND "${PG_CYCLES_LINK}")
  set(PG_CYCLES_LIBS "")
  set(PG_CYCLES_BYPRODUCTS "")
  set(PG_CYCLES_TARGETS "")
  foreach(item IN LISTS PG_CYCLES_LINK)
    if(item MATCHES "^\\.\\./\\.\\./lib/lib(.*)\\.a$")
      list(APPEND PG_CYCLES_LIBS ${PG_CYCLES_DIR}/lib/lib${CMAKE_MATCH_1}.a)
      list(APPEND PG_CYCLES_BYPRODUCTS ${PG_CYCLES_DIR}/lib/lib${CMAKE_MATCH_1}.a)
      list(APPEND PG_CYCLES_TARGETS ${CMAKE_MATCH_1})
    elseif(item MATCHES "^(/|-l)" AND NOT item MATCHES "OpenImageDenoise")
      list(APPEND PG_CYCLES_LIBS ${item})
    endif()
  endforeach()
  list(REMOVE_DUPLICATES PG_CYCLES_BYPRODUCTS)
  list(REMOVE_DUPLICATES PG_CYCLES_TARGETS)
  cmake_host_system_information(RESULT PG_CORES QUERY NUMBER_OF_LOGICAL_CORES)
  include(ExternalProject)
  ExternalProject_Add(cycles_build
    SOURCE_DIR ${cycles_SOURCE_DIR}
    BINARY_DIR ${PG_CYCLES_DIR}
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${PG_CYCLES_DIR} --target ${PG_CYCLES_TARGETS} --parallel ${PG_CORES}
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS ${PG_CYCLES_BYPRODUCTS})
  add_library(pgcycles STATIC src/pg/render/Cycles.cpp)
  target_link_libraries(pgcycles PUBLIC pgsim)
  target_include_directories(pgcycles SYSTEM PRIVATE ${PG_CYCLES_INCLUDES})
  if(PG_NANOVDB)
    target_include_directories(pgcycles SYSTEM PRIVATE "${PG_NANOVDB_INCLUDE_DIR}")
    target_compile_definitions(pgcycles PRIVATE PG_HAVE_NANOVDB)
  endif()
  target_compile_definitions(pgcycles PRIVATE PG_HAVE_CYCLES "CCL_NAMESPACE_BEGIN=namespace ccl {"
                                              "CCL_NAMESPACE_END=}" ${PG_CYCLES_DEFINES})
  target_compile_options(pgcycles PRIVATE ${PG_CYCLES_OPTIONS})
  add_dependencies(pgcycles cycles_build)
  if(PG_OIDN)
    add_dependencies(cycles_build OpenImageDenoise)
  endif()
  # Its libraries need one another round: as a group.
  set(PG_CYCLES_STATIC "")
  set(PG_CYCLES_SHARED "")
  foreach(item IN LISTS PG_CYCLES_LIBS)
    if(item MATCHES "^${PG_CYCLES_DIR}/")
      list(APPEND PG_CYCLES_STATIC ${item})
    else()
      list(APPEND PG_CYCLES_SHARED ${item})
    endif()
  endforeach()
  list(REMOVE_DUPLICATES PG_CYCLES_STATIC)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    string(REPLACE ";" "," PG_CYCLES_GROUP "${PG_CYCLES_STATIC}")
    target_link_libraries(pgcycles PRIVATE "$<LINK_GROUP:RESCAN,${PG_CYCLES_GROUP}>")
  else()
    target_link_libraries(pgcycles PRIVATE ${PG_CYCLES_STATIC} ${PG_CYCLES_STATIC})
  endif()
  target_link_libraries(pgcycles PRIVATE ${PG_CYCLES_SHARED})
  if(PG_OIDN)
    target_link_libraries(pgcycles PRIVATE OpenImageDenoise)
  endif()
  message(STATUS "Cycles 5.2.0: built from its sources (${PG_CYCLES_DIR})")
else()
  add_library(pgcycles STATIC src/pg/render/Cycles.cpp)
  target_link_libraries(pgcycles PUBLIC pgsim)
endif()
if(MSVC)
  target_compile_options(pgcycles PRIVATE /W4)
else()
  target_compile_options(pgcycles PRIVATE -Wall -Wextra)
endif()

# The renderers: OpenGL through function pointers the caller loads, so they
# link against nothing. The shader preview (the editor, `prototype render`) and
# the volume renderer of the simulation (the editor, `prototype pyro`).
add_library(pgpreview
  src/pg/gl/Camera.cpp
  src/pg/gl/Gl.cpp
  src/pg/gl/Png.cpp
  src/pg/gl/Preview.cpp
  src/pg/gl/Volume.cpp
)
target_link_libraries(pgpreview PUBLIC pgshadergraph pgsim)

# --- prototype --------------------------------------------------------------
#
# One program for the node editor -- what runs when it is given no command --
# and for the same work from the command line: list, gen, check, render.
#
# The editor needs Dear ImGui and GLFW. They are fetched at configure
# time (a system GLFW 3.3+ is used when there is one); to build offline, point
# FETCHCONTENT_SOURCE_DIR_IMGUI / _GLFW at local checkouts.
# PG_BUILD_GUI=OFF leaves the editor out: the commands alone, no dependencies
# -- for servers and CI.

option(PG_BUILD_GUI "Build the editor into prototype (fetches Dear ImGui and GLFW)" ON)

set(PG_TOOL_SOURCES tools/prototype/main.cpp tools/prototype/Commands.cpp tools/prototype/Offscreen.cpp)
if(PG_BUILD_GUI)
  list(APPEND PG_TOOL_SOURCES
    tools/prototype/App.cpp
    tools/prototype/Bake.cpp
    tools/prototype/Editor.cpp
    tools/prototype/FrameJob.cpp
    tools/prototype/FrameRender.cpp
    tools/prototype/Gizmo.cpp
    tools/prototype/NodeCanvas.cpp
    tools/prototype/Recovery.cpp
    tools/prototype/RenderJob.cpp
    tools/prototype/ShaderWorkspace.cpp
    tools/prototype/SimGeometry.cpp
    tools/prototype/SimAssets.cpp
    tools/prototype/SimElements.cpp
    tools/prototype/SimRunner.cpp
    tools/prototype/SimViewport.cpp
    tools/prototype/SimRender.cpp
    tools/prototype/RenderView.cpp
    tools/prototype/SimThumbnails.cpp
    tools/prototype/SimWorkspace.cpp
    tools/prototype/Theme.cpp
    tools/prototype/Thumbnails.cpp
    tools/prototype/Wedge.cpp
    tools/prototype/Widgets.cpp
    tools/prototype/Workspace.cpp)
endif()
add_executable(prototype ${PG_TOOL_SOURCES})
target_include_directories(prototype PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/prototype)
# Where the examples built into the program read their files (meshes) from.
target_compile_definitions(prototype PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim")
target_link_libraries(prototype PRIVATE pgshadergraph pgsim pgcycles Threads::Threads)
if(PG_WITH_VULKAN)
  target_link_libraries(prototype PRIVATE pggpu)
endif()
# Warnings for our own sources only; the fetched libraries compile as they are.
if(MSVC)
  set_source_files_properties(${PG_TOOL_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4")
else()
  set_source_files_properties(${PG_TOOL_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")
endif()

# `prototype render` and `sim` draw without a window through EGL when it exists
# -- and through a hidden GLFW window in the builds with the editor (Offscreen.h).
find_path(PG_EGL_INCLUDE_DIR EGL/egl.h)
find_library(PG_EGL_LIBRARY EGL)
if(PG_EGL_INCLUDE_DIR AND PG_EGL_LIBRARY)
  target_sources(prototype PRIVATE src/pg/gl/HeadlessContext.cpp)
  target_include_directories(prototype PRIVATE ${PG_EGL_INCLUDE_DIR})
  target_link_libraries(prototype PRIVATE pgpreview ${PG_EGL_LIBRARY})
  target_compile_definitions(prototype PRIVATE PG_HAVE_EGL)
endif()

if(PG_BUILD_GUI)
  include(FetchContent)
  # Not added as a CMake subproject -- imgui has no CMakeLists.txt -- so
  # SOURCE_SUBDIR points nowhere and its sources are compiled into prototype
  # below.
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(imgui)

  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # On Linux GLFW 3.4 builds for X11 and Wayland; Wayland needs
    # wayland-scanner. Without it: X11 only.
    find_program(PG_WAYLAND_SCANNER wayland-scanner)
    if(NOT PG_WAYLAND_SCANNER)
      set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(glfw
      GIT_REPOSITORY https://github.com/glfw/glfw.git
      GIT_TAG 3.4
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
  endif()

  target_sources(prototype PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  )
  # First on the include path, and not SYSTEM (-isystem loses to any -I): a
  # Dear ImGui installed on the system must not shadow the one fetched here.
  # FreeBSD's ports put theirs in /usr/local/include, next to the EGL headers
  # added above; a different 1.92.x compiles, then crashes at run time.
  target_include_directories(prototype BEFORE PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  # The fetched version, which App.cpp checks the imgui.h it sees against.
  file(STRINGS ${imgui_SOURCE_DIR}/imgui.h PG_IMGUI_VERSION_LINE REGEX "^#define IMGUI_VERSION_NUM")
  string(REGEX MATCH "[0-9]+" PG_IMGUI_VERSION_NUM "${PG_IMGUI_VERSION_LINE}")
  target_compile_definitions(prototype PRIVATE PG_HAVE_GUI PG_IMGUI_VERSION_NUM=${PG_IMGUI_VERSION_NUM}
    PG_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders")
  # The editor's typeface, Inter (tools/prototype/fonts, SIL OFL 1.1), compiled
  # in like the examples: the same letters on every system, nothing to find
  # at run time.
  set(PG_FONT_ARRAYS "")
  foreach(font Inter-Regular Inter-SemiBold)
    set(font_file ${CMAKE_CURRENT_SOURCE_DIR}/tools/prototype/fonts/${font}.ttf)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${font_file})
    file(READ ${font_file} font_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_bytes "${font_hex}")
    string(REPLACE "-" "" font_id "${font}")
    string(APPEND PG_FONT_ARRAYS "extern const unsigned char k${font_id}[] = {${font_bytes}};\n")
    string(APPEND PG_FONT_ARRAYS "extern const int k${font_id}Size = static_cast<int>(sizeof k${font_id});\n")
  endforeach()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/EditorFonts.cpp.tmp
"// Generated by CMake from tools/prototype/fonts/*.ttf. Do not edit.
namespace pg::editor::theme {
${PG_FONT_ARRAYS}}  // namespace pg::editor::theme
")
  configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/EditorFonts.cpp.tmp
                 ${CMAKE_CURRENT_BINARY_DIR}/generated/EditorFonts.cpp COPYONLY)
  target_sources(prototype PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/EditorFonts.cpp)
  # The OpenGL backend finds its functions at run time, like pg::gl::Api.
  target_link_libraries(prototype PRIVATE pgpreview glfw ${CMAKE_DL_LIBS})

  # The editor's interface without a window (tests/test_editor_ui.cpp):
  # Dear ImGui's core and the editor's theme, widgets and node canvas, no
  # display and no OpenGL -- it runs wherever the other tests do.
  add_executable(pgeditortests
    tests/test_main.cpp
    tests/test_editor_ui.cpp
    tools/prototype/FrameJob.cpp
    tools/prototype/NodeCanvas.cpp
    tools/prototype/Recovery.cpp
    tools/prototype/Theme.cpp
    tools/prototype/Widgets.cpp
    tools/prototype/Workspace.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/EditorFonts.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(pgeditortests BEFORE PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tools/prototype
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  # FrameJob works on a thread of its own: pthread is no part of libc on
  # FreeBSD, nor on glibc before 2.34.
  target_link_libraries(pgeditortests PRIVATE Threads::Threads)
  add_test(NAME editor_ui COMMAND pgeditortests)
  # The editor itself, driven by scripts -- under xvfb, or on the display
  # there is -- as a person would: quitting with changes not saved,
  # saving as, a crash recovered, Simulate Again, the cache saved and a scene
  # exported while the window plays on, points picked while what picks them
  # is made (tests/editor/run_editor.py).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(PG_XVFB_RUN xvfb-run)
  if(Python3_Interpreter_FOUND AND (PG_XVFB_RUN OR DEFINED ENV{DISPLAY}))
    if(NOT PG_XVFB_RUN)
      set(PG_XVFB_RUN "")
    endif()
    foreach(case quit_asks_and_saves quit_lets_go quit_cancelled example_saved_as_then_replaced crash_recovered
                 simulate_again saved_and_exported_in_background picked_while_its_picker_is_made)
      add_test(NAME editor_${case}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/editor/run_editor.py
                       --prototype $<TARGET_FILE:prototype> --xvfb "${PG_XVFB_RUN}" --case ${case})
      set_tests_properties(editor_${case} PROPERTIES TIMEOUT 600)
    endforeach()
  endif()
endif()

# --- Python -------------------------------------------------------------------
#
# The module pg: networks, geometry as numpy arrays, simulations, caches and
# USD from a script (docs/python.md). build/python is what goes on the
# PYTHONPATH: pg/__init__.py and the extension _pg beside it. pybind11 is
# fetched; to build offline, point FETCHCONTENT_SOURCE_DIR_PYBIND11 at a
# checkout.
if(PG_BUILD_PYTHON)
  include(FetchContent)
  # For the Python found above, the one the tests run: pybind11's own search
  # could find another.
  set(PYBIND11_FINDPYTHON ON)
  set(Python_EXECUTABLE ${Python3_EXECUTABLE})
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(pybind11)
  if(NOT COMMAND pybind11_add_module)
    # Offline (FETCHCONTENT_FULLY_DISCONNECTED) and no checkout given.
    message(WARNING "The Python module is left out: pybind11 could not be fetched; "
                    "set FETCHCONTENT_SOURCE_DIR_PYBIND11 to a checkout of it")
    set(PG_BUILD_PYTHON OFF)
  endif()
endif()
if(PG_BUILD_PYTHON)
  set(PG_PYTHON_DIR ${CMAKE_CURRENT_BINARY_DIR}/python/pg)
  pybind11_add_module(_pg MODULE
    src/python/Module.cpp
    src/python/PyGeometry.cpp
    src/python/PyNetwork.cpp
    src/python/PySimulation.cpp
    src/python/PyUsd.cpp)
  target_link_libraries(_pg PRIVATE pgsim)
  target_include_directories(_pg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/python)
  target_compile_definitions(_pg PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim")
  set_target_properties(_pg PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PG_PYTHON_DIR})
  if(NOT MSVC)
    target_compile_options(_pg PRIVATE -Wall -Wextra)
  endif()
  # Only the libraries the module uses. With libc++ on Linux the driver adds
  # libgcc_s after LLVM's unwinder (libc++abi's); both in the module's scope,
  # libgcc_s would bind its own calls to the other one, and the first
  # exception of a libstdc++ module imported after pg -- pxr's -- crashes.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(_pg PRIVATE -Wl,--as-needed)
  endif()
  # The package's Python side beside the module, and where the programs are:
  # prototype draws the pictures (Network.render).
  configure_file(src/python/pg/__init__.py ${PG_PYTHON_DIR}/__init__.py COPYONLY)
  file(GENERATE OUTPUT ${PG_PYTHON_DIR}/_build.py CONTENT
"# Written by CMake: where this build's programs and examples are.
PROTOTYPE = r'$<TARGET_FILE:prototype>'
EXAMPLES = r'${CMAKE_CURRENT_SOURCE_DIR}/examples'
")
endif()

# --- tests -----------------------------------------------------------------
enable_testing()
add_executable(pgtests
  tests/test_main.cpp
  tests/test_attribute_cow.cpp
  tests/test_geometry.cpp
  tests/test_cook.cpp
  tests/test_cache.cpp
  tests/test_determinism.cpp
  tests/test_expression.cpp
  tests/test_lang.cpp
  tests/test_assets.cpp
  tests/test_topology.cpp
  tests/test_rigid.cpp
  tests/test_concrete.cpp
  tests/test_rebar.cpp
  tests/test_glass.cpp
  tests/test_bricks.cpp
  tests/test_materials.cpp
  tests/test_uv.cpp
  tests/test_foliage.cpp
  tests/test_plants.cpp
  tests/test_materialx.cpp
  tests/test_particles.cpp
  tests/test_constraints.cpp
  tests/test_debris.cpp
  tests/test_guide.cpp
  tests/test_shader_graph.cpp
  tests/test_pyro.cpp
  tests/test_upres.cpp
  tests/test_frame_store.cpp
  tests/test_sim_network.cpp
  tests/test_sim_geometry.cpp
  tests/test_animation.cpp
  tests/test_shapes.cpp
  tests/test_mesh.cpp
  tests/test_liquid.cpp
  tests/test_rain.cpp
  tests/test_sops.cpp
  tests/test_export.cpp
  tests/test_gpu.cpp
  tests/test_state.cpp
  tests/test_coupling.cpp
  tests/test_cloth.cpp
  tests/test_grains.cpp
  tests/test_quench.cpp
  tests/test_shatter.cpp
  tests/test_grit_grains.cpp
  tests/test_alembic.cpp
  tests/test_vdb_read.cpp
  tests/test_aces.cpp
  tests/test_ocio.cpp
  tests/test_usd_materials.cpp
  tests/test_edit.cpp
  tests/test_dyntopo.cpp
  tests/test_vertices.cpp
  tests/test_trees.cpp
  tests/test_vegetation.cpp
  tests/test_render.cpp
  tests/test_gas.cpp
  tests/test_usd.cpp
  tests/test_usd_read.cpp
  tests/test_exr.cpp
  tests/test_picture.cpp
  tests/test_volume_mesh.cpp
  tests/test_video.cpp
)
target_link_libraries(pgtests PRIVATE pgcore pgshadergraph pgsim pgcycles)
if(PG_WITH_VULKAN)
  target_link_libraries(pgtests PRIVATE pggpu)
endif()
target_compile_definitions(pgtests PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim"
                                           PG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME pgtests COMMAND pgtests)
# The commands work in every build, with or without the editor.
add_test(NAME prototype_list COMMAND prototype list)
# The Python module, from a script as a user would (tests/python).
if(PG_BUILD_PYTHON)
  add_test(NAME python COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/python -v)
  set_tests_properties(python PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python")
endif()

# Compiles every example graph, and every output of every library node in both
# stages, for every target with glslangValidator; SPIR-V goes through spirv-val.
find_program(GLSLANG_VALIDATOR glslangValidator)
find_program(SPIRV_VAL spirv-val)
file(GLOB PG_SHADER_EXAMPLES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/*.pgsg)
if(GLSLANG_VALIDATOR)
  set(PG_SPIRV_VAL_ARGS)
  if(SPIRV_VAL)
    set(PG_SPIRV_VAL_ARGS --spirv-val ${SPIRV_VAL})
  endif()
  add_test(NAME shaders_compile
           COMMAND prototype check --nodes --glslang ${GLSLANG_VALIDATOR} ${PG_SPIRV_VAL_ARGS}
                   ${PG_SHADER_EXAMPLES})
  # The example user library, loaded at run time like any user's: each of its
  # nodes, and the graph that uses them.
  set(PG_USER_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/extra/stylized.pgnodes)
  add_test(NAME shaders_compile_user_library
           COMMAND prototype check --library ${PG_USER_LIBRARY} --nodes-from ${PG_USER_LIBRARY}
                   --glslang ${GLSLANG_VALIDATOR} ${PG_SPIRV_VAL_ARGS}
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/extra/toon.pgsg)
endif()

# --- benchmark + demo ------------------------------------------------------
add_executable(pgbench bench/bench_main.cpp)
target_link_libraries(pgbench PRIVATE pgcore)
# The rigid bodies against step 4 of the ROADMAP: the demolition tower, and
# ten times its pieces, on one thread and on all.
add_executable(pgbench_rigid bench/bench_rigid.cpp)
target_link_libraries(pgbench_rigid PRIVATE pgsim)
target_compile_definitions(pgbench_rigid PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim")
# The gas against step 5: the demolition's dust at the resolutions asked for,
# where a step's time goes and how much of the domain the dust takes.
add_executable(pgbench_pyro bench/bench_pyro.cpp)
target_link_libraries(pgbench_pyro PRIVATE pgsim)
target_compile_definitions(pgbench_pyro PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim")
add_executable(pgbench_liquid bench/bench_liquid.cpp)
target_link_libraries(pgbench_liquid PRIVATE pgsim)
target_compile_definitions(pgbench_liquid PRIVATE PG_SIM_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/sim")

add_executable(pgdemo cli/demo_main.cpp)
target_link_libraries(pgdemo PRIVATE pgcore)
