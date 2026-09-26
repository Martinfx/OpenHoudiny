// pgshader -- the shader graph tool: one program for the node editor and for
// the same work from the command line.
//
//   pgshader [GRAPH.pgsg] [options]       opens the editor -- the default
//   pgshader list|gen|check|render ...    runs a command, headless
//   pgshader help
//
// A build with PG_BUILD_GUI=OFF has no editor and needs no dependencies; it
// then wants a command.
#include "Commands.h"

#ifdef PG_HAVE_GUI
#include "App.h"
#endif

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "";
    if (pg::cli::isCommand(first)) return pg::cli::runCommand(argc, argv);
    if (first == "help" || first == "-h" || first == "--help") {
        pg::cli::printUsage(stdout);
        return 0;
    }
#ifdef PG_HAVE_GUI
    return pg::editor::runEditor(argc, argv);
#else
    std::fprintf(stderr, "pgshader: this build has no editor (PG_BUILD_GUI=OFF) -- give it a command\n\n");
    pg::cli::printUsage(stderr);
    return 2;
#endif
}
