// prototype -- procedural geometry, simulations and shaders from nodes: one
// program for the node editor and for the same work from the command line.
//
//   prototype [NETWORK.pgsim | GRAPH.pgsg] [options]   opens the editor -- the default
//   prototype list|gen|check|render|sim ...            runs a command, headless
//   prototype help
//
// A build with PG_BUILD_GUI=OFF has no editor and needs no dependencies; it
// then wants a command.
#include "Commands.h"

#include "pg/sim/Asset.h"

#ifdef PG_HAVE_GUI
#include "App.h"
#endif

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "";
    // The digital assets: those the program carries, those of the folders
    // it reads them from ($PROTOTYPE_ASSETS, the user's).
    std::vector<std::string> assetErrors;
    pg::sim::AssetLibrary::instance().loadDefaults(&assetErrors);
    for (const std::string& e : assetErrors) std::fprintf(stderr, "prototype: asset %s\n", e.c_str());
    if (pg::cli::isCommand(first)) return pg::cli::runCommand(argc, argv);
    if (first == "help" || first == "-h" || first == "--help") {
        pg::cli::printUsage(stdout);
        return 0;
    }
#ifdef PG_HAVE_GUI
    return pg::editor::runEditor(argc, argv);
#else
    std::fprintf(stderr, "prototype: this build has no editor (PG_BUILD_GUI=OFF) -- give it a command\n\n");
    pg::cli::printUsage(stderr);
    return 2;
#endif
}
