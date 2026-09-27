#pragma once
//
// The editor's window: GLFW, an OpenGL 3.3 context and Dear ImGui around an
// Editor. What `prototype` runs when it is given no command.
//
namespace pg::editor {

/// argv as prototype got it: [NETWORK.pgsim | GRAPH.pgsg] [--example NAME]
/// [--shaders] [--select NODE] [--library FILE]... [--target NAME]
/// [--mesh NAME] [--size WxH] [--screenshot OUT.png [--frames N]] [--script FILE].
int runEditor(int argc, char** argv);

}  // namespace pg::editor
