#pragma once
//
// The editor's window: GLFW, an OpenGL 3.3 context, Dear ImGui and imnodes
// around an Editor. What `pgshader` runs when it is given no command.
//
namespace pg::editor {

/// argv as pgshader got it: [GRAPH.pgsg] [--library FILE]... [--target NAME]
/// [--mesh NAME] [--size WxH] [--screenshot OUT.png [--frames N]].
int runEditor(int argc, char** argv);

}  // namespace pg::editor
