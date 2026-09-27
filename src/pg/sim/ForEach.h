#pragma once
//
// For-Each loops: the nodes between a For-Each Begin and its For-Each End
// run once a piece of what comes into the Begin -- a piece by an attribute
// (the class a Connectivity gives), a primitive, a point -- or a number of
// times, or each time on what the last time made; the End puts what each
// time made together. As Houdini's For-Each blocks.
//
//   [Connectivity] -> [For-Each Begin] -> [Transform] -> [For-Each End] -> ...
//                          |  one piece at a time  |
//
// The Begin, cooked on its own, gives the first piece: what the nodes after
// it show while they are edited. The End cooks the loop: the nodes between
// -- the body -- copied into a network of their own (the Begin's place taken
// by an Asset Input), cooked in a geometry graph of its own, a piece at a
// time. Each piece carries, as detail attributes, `iteration` (from 0),
// `numiterations` and `value` (the piece's attribute value, or the
// element's number): a wrangle in the body reads them with detail().
//
#include "pg/core/Node.h"
#include "pg/sim/Network.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::sim {

/// How the Begin cuts what comes in.
enum class ForEachMethod { Pieces, Primitives, Points, Count, Feedback };

/// The pieces a For-Each goes over: at most `limit` of them (0: all). An
/// empty list, with why in `error`, when it cannot cut it.
std::vector<GeometryPtr> forEachPieces(const GeometryPtr& whole, ForEachMethod method, const std::string& attribute,
                                       int count, size_t limit, std::string& error);

/// Registers "foreachbegin" and "foreachend" with the core. Idempotent.
void registerForEachNodes();

/// The body a For-Each End cooks: `body`'s node `output` is what one piece
/// makes, its Asset Input nodes take the piece (index 0). Other nodes ignore it.
void setForEachBody(pg::Node& node, std::shared_ptr<const Network> body, int output);

/// The nodes between `begin` and `end` of `net`, as a network of their own:
/// the Begin replaced by an Asset Input (index 0). `output` is the node
/// whose geometry goes into the End. False, with why, when there is no body.
bool forEachBody(const Network& net, int begin, int end, Network& body, int& output, std::string& error);

/// The For-Each Begin that For-Each End `end` closes: the one its Begin
/// parameter names, else the nearest upstream. 0, with why, if none.
int forEachBegin(const Network& net, int end, std::string& error);

}  // namespace pg::sim
