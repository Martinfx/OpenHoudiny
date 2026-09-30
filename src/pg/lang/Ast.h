#pragma once
//
// The insides of the wrangle language (Lang.h): what the parser makes, what
// the checker turns it into, and what the interpreter runs.
//
//   source --Parse.cpp--> Ast (untyped, shared by every run)
//          --Check.cpp--> Typed (per run: types from the geometry, attributes
//                                bound, functions chosen)
//          --Eval.cpp---> results, element after element
//
#include "pg/lang/Lang.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pg::lang {

// --- values ------------------------------------------------------------------------------

/// A 3x3 matrix, row-major, rows are the images of x, y, z: v * M.
// The language's matrices are GLM's -- but, as every value of the language,
// zero until given one (GLM starts its own at the identity). VEX's rows are
// GLM's columns: the same numbers in memory, the other convention -- VEX's
// v * M is GLM's M * v, and its A * B (A, then B) GLM's B * A.
struct Mat3 : glm::mat3 {
    Mat3() : glm::mat3(0.0f) {}
    Mat3(const glm::mat3& m) : glm::mat3(m) {}  // NOLINT: GLM's arithmetic gives these
    explicit Mat3(float diagonal) : glm::mat3(diagonal) {}
};
struct Mat4 : glm::mat4 {
    Mat4() : glm::mat4(0.0f) {}
    Mat4(const glm::mat4& m) : glm::mat4(m) {}  // NOLINT
    explicit Mat4(float diagonal) : glm::mat4(diagonal) {}
};

using IntArr = std::vector<int32_t>;
using FloatArr = std::vector<float>;
using Vec2Arr = std::vector<Vec2>;
using Vec3Arr = std::vector<Vec3>;
using Vec4Arr = std::vector<Vec4>;
using StrArr = std::vector<std::string>;

template <class T> struct TypeOf;
template <> struct TypeOf<int32_t> { static constexpr Type value = Type::Int; };
template <> struct TypeOf<float> { static constexpr Type value = Type::Float; };
template <> struct TypeOf<Vec2> { static constexpr Type value = Type::Vec2; };
template <> struct TypeOf<Vec3> { static constexpr Type value = Type::Vec3; };
template <> struct TypeOf<Vec4> { static constexpr Type value = Type::Vec4; };
template <> struct TypeOf<Mat3> { static constexpr Type value = Type::Mat3; };
template <> struct TypeOf<Mat4> { static constexpr Type value = Type::Mat4; };
template <> struct TypeOf<std::string> { static constexpr Type value = Type::String; };
template <> struct TypeOf<IntArr> { static constexpr Type value = Type::IntArray; };
template <> struct TypeOf<FloatArr> { static constexpr Type value = Type::FloatArray; };
template <> struct TypeOf<Vec2Arr> { static constexpr Type value = Type::Vec2Array; };
template <> struct TypeOf<Vec3Arr> { static constexpr Type value = Type::Vec3Array; };
template <> struct TypeOf<Vec4Arr> { static constexpr Type value = Type::Vec4Array; };
template <> struct TypeOf<StrArr> { static constexpr Type value = Type::StringArray; };

inline bool isArray(Type t) { return t >= Type::IntArray; }
inline bool isVector(Type t) { return t == Type::Vec2 || t == Type::Vec3 || t == Type::Vec4; }
inline bool isMatrix(Type t) { return t == Type::Mat3 || t == Type::Mat4; }
inline bool isScalar(Type t) { return t == Type::Int || t == Type::Float; }
inline bool isNumeric(Type t) { return isScalar(t) || isVector(t) || isMatrix(t); }
/// Components of a vector type: 2, 3, 4; 1 for a scalar.
inline int width(Type t) { return t == Type::Vec2 ? 2 : t == Type::Vec3 ? 3 : t == Type::Vec4 ? 4 : 1; }
Type elementOf(Type array);   ///< float[] -> float
Type arrayOf(Type element);   ///< float -> float[]; Void if there is none

// --- the parse -----------------------------------------------------------------------------

struct Pos {
    int line = 1, col = 1;
};

enum class Tok : uint8_t {
    End, Int, Float, String, Ident, Attr, Dollar,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semi, Dot, Question, Colon,
    Plus, Minus, Star, Slash, Percent, Not, Tilde, Amp, Pipe, Caret, AndAnd, OrOr,
    Lt, Le, Gt, Ge, EqEq, Ne, Shl, Shr,
    Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, PlusPlus, MinusMinus,
};

struct Token {
    Tok kind = Tok::End;
    Pos pos;
    std::string text;     ///< an identifier, a string's value, an attribute's name
    int64_t ival = 0;
    double fval = 0.0;
    Type attrType = Type::Void;  ///< Attr: the type its prefix gives (i@ f@ ...); Void: none
};

enum class EK : uint8_t {
    Int, Float, String, Braces, Var, Dollar, Attr, Member, Index, Call, Cast, Unary, Binary, Assign,
    PreInc, PreDec, PostInc, PostDec, Ternary,
};

struct Expr {
    EK kind = EK::Int;
    Pos pos;
    int64_t ival = 0;
    double fval = 0.0;
    std::string text;        ///< a name, a string, an attribute
    Tok op = Tok::End;       ///< Unary, Binary, Assign
    Type type = Type::Void;  ///< Attr: its prefix type; Cast: the type cast to
    int input = 0;           ///< Attr: @opinputN_name reads input N
    std::vector<std::unique_ptr<Expr>> args;
};

enum class SK : uint8_t { Expr, Decl, Block, If, While, DoWhile, For, Foreach, Break, Continue, Return, Empty };

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Declarator {
    std::string name;
    Pos pos;
    bool array = false;
    std::unique_ptr<Expr> init;
};

struct Stmt {
    SK kind = SK::Empty;
    Pos pos;
    std::unique_ptr<Expr> expr;  ///< Expr, Return; the condition of If, While, DoWhile, For
    Type declType = Type::Void;  ///< Decl
    std::vector<Declarator> decls;
    std::vector<StmtPtr> body;   ///< Block; If: then [, else]; loops: the body; For: init, step as [1], [2]
    std::unique_ptr<Expr> step;  ///< For
    // Foreach (int i; T value; array)
    Type indexType = Type::Void, valueType = Type::Void;
    std::string indexName, valueName;
    bool valueArray = false;
};

struct Param {
    Type type = Type::Void;
    std::string name;
    Pos pos;
};

struct Function {
    std::string name;
    Type ret = Type::Void;
    std::vector<Param> params;
    std::unique_ptr<Stmt> body;
    Pos pos;
};

struct Ast {
    std::vector<Function> functions;
    std::vector<StmtPtr> main;
    bool readsTime = false;
    std::vector<Channel> channels;
    std::vector<std::string> attrNames;
};

/// Parses a program (statements and functions) or, with `expression`, one
/// expression. Null, with "line L, col C: why", if it does not.
std::unique_ptr<Ast> parse(const std::string& source, bool expression, std::string& error);

std::string at(const Pos& p, const std::string& message);

// --- the typed program -----------------------------------------------------------------------

enum class Op : uint8_t {
    Const, Local, Attr, Comp, Index, Cast, Neg, Not, BitNot,
    Add, Sub, Mul, Div, Mod, VecMat, MatMat, MatScale,
    Lt, Le, Gt, Ge, Eq, Ne, And, Or, BitAnd, BitOr, BitXor, Shl, Shr,
    Ternary, Assign, IncDec, Builtin, Call, Make,
};

struct TNode;
using TPtr = std::unique_ptr<TNode>;

/// Where an assignment goes.
struct LValue {
    enum class K : uint8_t { Local, Attr, Group } kind = K::Local;
    int slot = -1;       ///< Local: the variable; Attr, Group: the binding
    Type base = Type::Void;  ///< the type of the variable or attribute
    int comp = -1;       ///< a component of a vector: .x or [constant]
    TPtr index;          ///< an element of an array, or a component by a computed index
};

struct Env;
using Impl = void (*)(Env&, const TNode&, void*);

struct TNode {
    Op op = Op::Const;
    Type type = Type::Void;  ///< what it gives
    Type sub = Type::Void;   ///< the operands' type: of a comparison, what a cast is from
    int a = -1, b = -1;      ///< a slot, a binding, a component, a function
    Pos pos;
    // Const
    int32_t ci = 0;
    float cf = 0.0f;
    Vec4 cv;                 ///< a vector constant, in its first components
    std::string cs;
    // Builtin
    Impl fn = nullptr;
    /// What a builtin resolved when it was typed: an attribute, a geometry.
    const void* aux = nullptr;
    const void* aux2 = nullptr;
    std::unique_ptr<LValue> lv;  ///< Assign, IncDec, and builtins that write into an argument
    std::vector<TPtr> kids;
    /// Call: where each argument that is a variable gets the parameter's
    /// value back -- arguments go by reference, as in VEX. Null for the rest.
    std::vector<std::unique_ptr<LValue>> refs;
};

enum class TS : uint8_t { Expr, Decl, Block, If, While, DoWhile, For, Foreach, Break, Continue, Return };

struct TStmt;
using TSPtr = std::unique_ptr<TStmt>;

struct TStmt {
    TS kind = TS::Expr;
    TPtr expr;                 ///< Expr; If, While, DoWhile, For: the condition; Return: the value
    TPtr step;                 ///< For
    Type type = Type::Void;    ///< Decl: of the variable; Foreach: of the value; Return: of the function
    int slot = -1;             ///< Decl: the variable; Foreach: the value; Return: where the value goes
    int slot2 = -1;            ///< Foreach: the index, -1 if none
    std::vector<TSPtr> body;   ///< Block, loops: the body; If: then, else; For: init
};

struct TFunction {
    std::string name;
    Type ret = Type::Void;
    std::vector<int> params;       ///< the slots of the parameters
    std::vector<Type> paramTypes;
    int result = -1;               ///< where a return puts its value
    TSPtr body;
    bool checked = false, checking = false;
};

/// A @name of the element being run on: an attribute, or what the run knows.
struct Binding {
    enum class K : uint8_t {
        Attr,      ///< an attribute
        Missing,   ///< read, not there: zero
        Group,     ///< @group_name
        ElemNum, PtNum, PrimNum, VtxNum, NumElem, NumPt, NumPrim, NumVtx,
        Time, Frame, TimeInc,
        Centroid,  ///< @P of a primitive
    } kind = K::Missing;
    Type type = Type::Float;
    std::string name;
    AttrClass cls = AttrClass::Point;   ///< the attribute's class
    /// How the element run on maps to this attribute's: the same one, the
    /// point of a vertex, the primitive of a vertex, the detail.
    enum class Map : uint8_t { Same, VertexPoint, VertexPrim, Detail } map = Map::Same;
    int input = -1;                     ///< @opinputN_: that input; -1: the geometry run on
    const std::byte* read = nullptr;
    std::byte* write = nullptr;
    AttributeArray* array = nullptr;    ///< for strings
    const AttributeArray* readArray = nullptr;
    size_t count = 0;                   ///< elements it has
    uint8_t* mask = nullptr;            ///< Group, written
    const uint8_t* readMask = nullptr;  ///< Group, read
    bool written = false;
    /// A string attribute written: its table's strings, by index -- an
    /// ordered run only, one thread.
    std::shared_ptr<std::unordered_map<std::string, int32_t>> strings;
};

/// A builtin, one of its overloads.
struct Overload {
    Type ret = Type::Void;
    std::vector<Type> params;
    Impl fn = nullptr;
    /// Changes geometry or other elements: runs the program in order.
    bool sideEffect = false;
    /// The last parameter may repeat (addprim(0, "poly", a, b, c ...)).
    bool variadic = false;
    /// Parameters written into: the first `refs` must be variables (append()).
    int refs = 0;
};

using Builtins = std::map<std::string, std::vector<Overload>, std::less<>>;
/// Every builtin, by name. Some are typed by the checker itself (point(),
/// ch(), set() ...); their names are here with no overloads, so that the
/// parser knows them.
const Builtins& builtins();

/// A deferred change to the geometry: what addpoint(), setpointattrib() ...
/// ask for, done after the run in the order asked.
struct Deferred {
    struct NewPoint {
        Vec3 P;
        int32_t from = -1;  ///< a point whose attributes it takes
    };
    struct NewPrim {
        bool closed = true;
        std::vector<int32_t> points;
    };
    struct SetAttr {
        AttrClass cls = AttrClass::Point;
        std::string name;
        int32_t elem = 0;
        int32_t vertexOf = -1;  ///< setvertexattrib(prim, i): the primitive
        Type type = Type::Float;
        Vec4 value;
        std::string text;
        uint8_t mode = 0;  ///< 0 set, 1 add, 2 min, 3 max, 4 mult
    };
    struct SetGroup {
        AttrClass cls = AttrClass::Point;
        std::string name;
        int32_t elem = 0;
        bool member = true;
    };
    std::vector<NewPoint> points;
    std::vector<NewPrim> prims;
    std::vector<SetAttr> attrs;
    std::vector<SetGroup> groups;
    std::vector<int32_t> removePoints;
    std::vector<std::pair<int32_t, bool>> removePrims;  ///< with its points
    size_t basePoints = 0, basePrims = 0, baseVertices = 0;
};

struct Queries;  // Eval.cpp: point trees and neighbours, per input, made before a run

/// What every element of one run shares.
struct Run {
    const Geometry* geo = nullptr;          ///< the geometry run on (its attributes are bound)
    std::array<const Geometry*, 4> inputs{};
    AttrClass cls = AttrClass::Point;
    size_t count = 0;                       ///< elements run over
    std::vector<Binding> bindings;
    std::vector<uint32_t> vertexPrim;       ///< vertex -> its primitive, when bound
    std::vector<Vec3> centroids;            ///< @P of the primitives, when read
    CookContext ctx;
    const Host* host = nullptr;
    const std::atomic<bool>* interrupt = nullptr;
    Queries* queries = nullptr;
    std::vector<TFunction>* functions = nullptr;
    std::array<int, 16> slotCounts{};       ///< per Type
};

/// One thread's state while it runs elements.
struct Env {
    std::vector<int32_t> i;
    std::vector<float> f;
    std::vector<Vec2> v2;
    std::vector<Vec3> v3;
    std::vector<Vec4> v4;
    std::vector<Mat3> m3;
    std::vector<Mat4> m4;
    std::vector<std::string> s;
    std::vector<IntArr> ia;
    std::vector<FloatArr> fa;
    std::vector<Vec2Arr> v2a;
    std::vector<Vec3Arr> v3a;
    std::vector<Vec4Arr> v4a;
    std::vector<StrArr> sa;

    const Run* run = nullptr;
    size_t elem = 0;
    Deferred* deferred = nullptr;  ///< set in an ordered run
    std::string log;
    std::vector<std::string> warnings;
    std::string error;             ///< error() called, or interrupted
    bool stop = false;
    uint32_t ticks = 0;

    explicit Env(const Run& r);
    template <class T> std::vector<T>& store();
    /// True when the run should end: an error, or interrupted.
    bool halted() {
        if (stop) return true;
        if (run->interrupt && (++ticks & 1023u) == 0 && run->interrupt->load(std::memory_order_relaxed)) {
            error = "interrupted";
            stop = true;
        }
        return stop;
    }
};

template <> inline std::vector<int32_t>& Env::store<int32_t>() { return i; }
template <> inline std::vector<float>& Env::store<float>() { return f; }
template <> inline std::vector<Vec2>& Env::store<Vec2>() { return v2; }
template <> inline std::vector<Vec3>& Env::store<Vec3>() { return v3; }
template <> inline std::vector<Vec4>& Env::store<Vec4>() { return v4; }
template <> inline std::vector<Mat3>& Env::store<Mat3>() { return m3; }
template <> inline std::vector<Mat4>& Env::store<Mat4>() { return m4; }
template <> inline std::vector<std::string>& Env::store<std::string>() { return s; }
template <> inline std::vector<IntArr>& Env::store<IntArr>() { return ia; }
template <> inline std::vector<FloatArr>& Env::store<FloatArr>() { return fa; }
template <> inline std::vector<Vec2Arr>& Env::store<Vec2Arr>() { return v2a; }
template <> inline std::vector<Vec3Arr>& Env::store<Vec3Arr>() { return v3a; }
template <> inline std::vector<Vec4Arr>& Env::store<Vec4Arr>() { return v4a; }
template <> inline std::vector<StrArr>& Env::store<StrArr>() { return sa; }

// --- evaluation (Eval.cpp) -------------------------------------------------------------------

template <class T> T ev(const TNode& n, Env& e);
/// A reference to the value, without a copy when it is a variable.
template <class T> const T& evRef(const TNode& n, Env& e, T& scratch);
/// The variable, array or not, an argument written into names.
template <class T> T& refOf(const LValue& lv, Env& e);

enum class Flow : uint8_t { Next, Break, Continue, Return };
Flow exec(const TStmt& s, Env& e);

/// Evaluates to a value of any type, written to `out` (a T* of the node's type).
void evalInto(const TNode& n, Env& e, void* out);

// --- helpers the builtins share --------------------------------------------------------------

float valueNoise(const Vec3& p);
float hashFloat(uint32_t h);
uint32_t hashBits(float x);
Vec4 quatFromAxisAngle(float angle, const Vec3& axis);
Vec4 quatMul(const Vec4& a, const Vec4& b);
Vec3 quatRotate(const Vec4& q, const Vec3& v);
Mat3 quatToMat3(const Vec4& q);
Vec4 mat3ToQuat(const Mat3& m);
Mat3 mul(const Mat3& a, const Mat3& b);
Vec3 mul(const Vec3& v, const Mat3& m);
Mat3 transpose(const Mat3& m);
Mat3 inverse(const Mat3& m);
float determinant(const Mat3& m);
Mat4 transpose(const Mat4& m);
Mat4 inverse(const Mat4& m);
float determinant(const Mat4& m);
/// The 3x3 of a 4x4, and a 4x4 with a 3x3 in its corner.
Mat3 upper(const Mat4& m);
Mat4 widen(const Mat3& m);

/// sprintf's formatting: %d %i %f %g %e %s %x %c %%, flags and widths.
struct FormatArg {
    Type type = Type::Float;
    double number = 0.0;
    Vec4 v;
    std::string text;
};
std::string format(const std::string& fmt, const std::vector<FormatArg>& args);

}  // namespace pg::lang
