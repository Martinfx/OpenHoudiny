#pragma once
//
// Digital assets: a network of geometry nodes packed into one node of its
// own, with the parameters it chooses to show, used anywhere as any other
// node -- as Houdini's digital assets are.
//
//   building.pgasset -- a network of geometry nodes:
//     [Asset Input 0] -> ... -> [the displayed node]      its input, its output
//     asset building 3 "Building"                         its name and version
//     promote floors count floors "Floors"                a node's parameter, shown as its own
//
//   any network:  [Grid] -> [Building] -> ...             an instance: a node of type "building"
//
// The definitions live in a library (AssetLibrary): the .pgasset files of
// the folders it reads, those the program carries (examples/assets), and
// those written into a network's file. An instance follows its definition:
// saved again, the definition changes every instance, wherever it is.
//
// An instance cooks the definition's network -- a copy of it, with the
// instance's values in the promoted parameters and its inputs in the Asset
// Input nodes -- in a geometry graph of its own (GeometryGraph.h), so that
// what changed cooks again and nothing else.
//
#include "pg/core/Node.h"
#include "pg/sim/Network.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pg::sim {

struct AssetDef {
    std::string name;
    int version = 1;
    std::string file;                    ///< where it was read from; empty: carried by the program or a network
    std::shared_ptr<const Network> net;  ///< the definition
    NodeType type;                       ///< the type of its instances
    std::vector<int> inputs;             ///< its Asset Input nodes, by index
    int output = 0;                      ///< the node whose geometry it gives: the displayed one
    uint64_t revision = 0;               ///< different for every definition the library has held
    /// AssetLibrary::varies(), remembered: the library's revision it was
    /// asked at, shifted by one, and the answer in the lowest bit.
    mutable std::atomic<uint64_t> variesMemo{0};
};

class AssetLibrary {
public:
    static AssetLibrary& instance();

    /// Adds `net` -- a network with Network::asset() set -- as a definition,
    /// replacing one of the same name. False, with why, if it is not one: no
    /// name, no displayed node, a promoted parameter that is not there, a
    /// name a node type of the program has, an instance of itself inside
    /// (or inside an asset it holds).
    bool add(const Network& net, const std::string& file, std::string& error);
    /// Adds it unless the library has this asset in the same version or a
    /// newer one: what a network's file brings along.
    bool addIfNewer(const Network& net, std::string& error);
    std::shared_ptr<const AssetDef> find(std::string_view name) const;
    std::vector<std::shared_ptr<const AssetDef>> all() const;
    /// Whether the asset's geometry changes with the frame on its own: asked
    /// of its network again whenever the library changes, since an asset it
    /// holds may have.
    bool varies(std::string_view name) const;
    /// Reads every .pgasset in `folder`; one line in `errors` for each that
    /// does not load. The number read.
    int loadFolder(const std::string& folder, std::vector<std::string>& errors);
    /// The assets the program carries, and those of the folders(): once.
    void loadDefaults(std::vector<std::string>* errors = nullptr);
    /// Changes whenever a definition does.
    uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

    /// Where assets are read from: $PROTOTYPE_ASSETS (folders split by ':'),
    /// then userFolder().
    static std::vector<std::string> folders();
    /// Where the editor saves them: $XDG_DATA_HOME/prototype/assets, or
    /// ~/.local/share/prototype/assets.
    static std::string userFolder();

private:
    AssetLibrary() = default;
    /// The definition of `net`, or null with why: built outside the lock,
    /// since the types of its nodes may be assets of the library.
    static std::shared_ptr<AssetDef> build(const Network& net, const std::string& file, std::string& error);
    bool insert(std::shared_ptr<AssetDef> def, bool onlyNewer, std::string& error);
    /// Whether `net` holds an instance of `name`, itself or inside the assets it holds.
    bool holdsLocked(const Network& net, std::string_view name, std::vector<std::string>& seen) const;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<const AssetDef>, std::less<>> defs_;
    /// Definitions replaced: kept, since a node type of theirs may still be
    /// pointed at while an editor draws.
    std::vector<std::shared_ptr<const AssetDef>> retired_;
    std::atomic<uint64_t> revision_{0};
    bool defaults_ = false;
};

/// The asset types, for the editor's lists: category "Assets".
std::vector<const NodeType*> assetTypes();

/// Makes nodes `ids` of `net` one asset, `info`: its definition -- those
/// nodes, an Asset Input for each geometry that came into them, the display
/// flag on the one whose geometry went on -- into the library as `file`
/// (and into `def`, if given); in their place in `net`, an instance linked
/// as they were. The instance's id; 0, with why, when they cannot be one:
/// only geometry nodes go in, at most four geometries come in, and the
/// geometry of one node goes on.
int collapseToAsset(Network& net, const std::vector<int>& ids, const AssetInfo& info, const std::string& file,
                    Network* def, std::string& error);

/// Writes an asset's definition to `path`, making its folder; false, with why.
bool writeAsset(const Network& def, const std::string& path, std::string& error);

/// Registers the core's node types of assets: "asset" (an instance) and
/// "asset_input" (what comes into one). Idempotent.
void registerAssetNodes();
/// Hands `geometry` to an Asset Input node of the core; other nodes ignore it.
void setAssetInput(pg::Node& node, GeometryPtr geometry);

}  // namespace pg::sim
