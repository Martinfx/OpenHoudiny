#pragma once
//
// The wrangle language: a C-like language run over every point, primitive,
// vertex -- or once over the whole geometry -- the way VEX is in Houdini.
//
//   float d = length(@P);
//   if (d > ch("radius")) {
//       @Cd = {1, 0, 0};
//   } else {
//       int near[] = nearpoints(0, @P, 0.3);
//       @count = len(near);
//   }
//
// What it has:
//   * types int, float, vector2, vector, vector4, matrix3, matrix, string
//     and arrays of int, float, vector2, vector, vector4 and string;
//   * local variables, if/else, for, foreach, while, do-while, break,
//     continue, return, and functions of its own;
//   * attributes by name: @P, @Cd, typed where they are made -- i@count,
//     f@mass, v@dir, s@name, p@orient, u@uv -- @group_name for groups, and
//     @opinput1_P for the same element of another input;
//   * reading any element of any input (point(), prim(), nearpoints(),
//     neighbours(), primpoints() ...) and making and deleting geometry
//     (addpoint(), addprim(), removepoint(), setpointattrib() ...);
//   * ch("name") for a parameter of the node -- or ch("../node/param") for
//     another's -- and $F, $T for the frame and the time.
//
// Division is always a real one: 7 / 2 is 3.5, and it is cut to 3 only when
// it goes into an int. Dividing by zero gives zero rather than infinity.
//
// A program is parsed once. Each run types it against the geometry it runs
// on (an attribute that is there has its own type) and then runs it: over
// deterministic chunks in parallel when it only touches the element it runs
// on, in order otherwise (when it makes or deletes geometry, writes another
// element's attributes, or writes strings). The result never depends on the
// number of threads.
//
// It is a tree-walking interpreter: the seam where a compiler would plug in is
// Program::run.
//
#include "pg/core/Geometry.h"
#include "pg/core/Node.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pg::lang {

enum class Type : uint8_t {
    Void,
    Int,
    Float,
    Vec2,
    Vec3,
    Vec4,
    Mat3,
    Mat4,
    String,
    IntArray,
    FloatArray,
    Vec2Array,
    Vec3Array,
    Vec4Array,
    StringArray,
};

/// "int", "vector", "float[]" -- as the language spells it.
const char* typeName(Type t);

/// What a program reads that is not geometry: the variables $F, $T ... and
/// parameters -- ch("name") of the node itself, ch("../box1/size") of
/// another. Called from several threads at once: it must be read-only.
class Host {
public:
    virtual ~Host() = default;
    /// $name: false if there is no such variable.
    virtual bool variable(std::string_view name, double& out) const;
    /// ch(path): component `component` (0 for a number) of a parameter. False,
    /// with why, if there is no such parameter.
    virtual bool channel(std::string_view path, int component, double& out, std::string& error) const;
    /// chs(path): a parameter's text.
    virtual bool channelText(std::string_view path, std::string& out, std::string& error) const;
};

/// The frame, the time and the frame rate as $F, $FF, $T, $FPS.
class TimeHost : public Host {
public:
    explicit TimeHost(const CookContext& ctx) : ctx_(ctx) {}
    bool variable(std::string_view name, double& out) const override;

protected:
    CookContext ctx_;
};

/// A ch() the program calls with a name written in it: what a wrangle node
/// makes a parameter of.
struct Channel {
    std::string name;
    Type type = Type::Float;  ///< Float (ch, chf), Int (chi), Vec3 (chv), String (chs)
};

struct RunOptions {
    /// What the program runs over: every point, primitive or vertex, or once
    /// over the whole geometry (Detail).
    AttrClass runOver = AttrClass::Point;
    /// Only the elements of this group of the class run over; empty: all.
    std::string group;
    /// The geometries point(), npoints() ... read: input 0 as it came in --
    /// the program's own writes do not show there -- and up to three more.
    /// A null input 0 reads a copy of the geometry taken before the run.
    std::array<GeometryPtr, 4> inputs;
    CookContext ctx;
    /// $F, ch() ... Without one, $F, $T, $FPS come from ctx and ch() finds
    /// nothing.
    const Host* host = nullptr;
    /// Set from another thread, it stops the run: run() returns false.
    const std::atomic<bool>* interrupt = nullptr;
};

/// What a run said besides its result: printf() and warning() lines.
struct RunReport {
    std::string log;
    std::vector<std::string> warnings;
};

class Program {
public:
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    /// Parses `source`. Null, with the line and column of what is wrong in
    /// `error`, if it does not parse -- or calls a function that does not
    /// exist, or with the wrong number of arguments.
    static std::unique_ptr<Program> parse(const std::string& source, std::string& error);

    /// Runs over `geo`: writes attributes, makes and deletes geometry. False,
    /// with why, for a program that does not type against this geometry, one
    /// that calls error(), or a run that was interrupted -- `geo` may then
    /// be half written.
    bool run(Geometry& geo, const RunOptions& options, std::string& error, RunReport* report = nullptr) const;
    /// Over every point, at `ctx`.
    bool run(Geometry& geo, const CookContext& ctx, std::string& error) const;

    /// True when a run depends on the time: @Time, @Frame, $F, $T ...
    bool readsTime() const;
    /// The ch() calls with the name written in them, in the order they first
    /// appear; one each.
    const std::vector<Channel>& channels() const;
    /// The @names the program uses, in the order they first appear.
    const std::vector<std::string>& slotNames() const;

    struct Impl;

private:
    Program();
    std::unique_ptr<Impl> impl_;
};

/// One expression without geometry -- a parameter's value: $F * 0.1,
/// sin($T) * 2, ch("../box1/sizex") / 2, {1, $F, 0}.
class Expression {
public:
    ~Expression();
    Expression(const Expression&) = delete;
    Expression& operator=(const Expression&) = delete;

    static std::unique_ptr<Expression> parse(const std::string& text, std::string& error);

    /// Its type: Int, Float, Vec2..Vec4 or String.
    Type type() const;
    /// A number (the first component of a vector).
    bool evalFloat(const Host& host, double& out, std::string& error) const;
    /// A vector (a number in all three components).
    bool evalVector(const Host& host, Vec3& out, std::string& error) const;
    bool evalText(const Host& host, std::string& out, std::string& error) const;

    /// True when its value depends on the time ($F, $T ...).
    bool readsTime() const;
    /// The paths of the ch() calls in it, as written.
    const std::vector<Channel>& channels() const;

    struct Impl;

private:
    Expression();
    std::unique_ptr<Impl> impl_;
};

/// True when `text` is a plain number, not an expression: "1.5", "-2", "1e3".
bool isNumber(std::string_view text);

}  // namespace pg::lang
