#include "pg/lang/Ast.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace pg::lang {

std::string at(const Pos& p, const std::string& message) {
    return "line " + std::to_string(p.line) + ", col " + std::to_string(p.col) + ": " + message;
}

namespace {

// --- lexer -----------------------------------------------------------------------------------

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src) {}

    bool run(std::vector<Token>& out, std::string& error) {
        for (;;) {
            if (!skip(error)) return false;
            Token t;
            t.pos = pos();
            if (i_ >= s_.size()) {
                t.kind = Tok::End;
                out.push_back(std::move(t));
                return true;
            }
            if (!one(t, error)) return false;
            out.push_back(std::move(t));
        }
    }

private:
    Pos pos() const { return {line_, static_cast<int>(i_ - lineStart_) + 1}; }
    char peek(size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    void advance() {
        if (s_[i_] == '\n') {
            ++line_;
            lineStart_ = i_ + 1;
        }
        ++i_;
    }

    bool skip(std::string& error) {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                const Pos start = pos();
                advance();
                advance();
                while (i_ < s_.size() && !(s_[i_] == '*' && peek(1) == '/')) advance();
                if (i_ >= s_.size()) {
                    error = at(start, "a comment that does not end: */ is missing");
                    return false;
                }
                advance();
                advance();
            } else {
                break;
            }
        }
        return true;
    }

    /// The type a prefix before @ gives: i@ f@ u@ v@ p@ s@ 3@ 4@, and the
    /// arrays i[]@ ...; Void if `c` is not one.
    static Type prefixType(char c, bool array) {
        Type t = Type::Void;
        switch (c) {
            case 'i': t = Type::Int; break;
            case 'f': t = Type::Float; break;
            case 'u': t = Type::Vec2; break;
            case 'v': t = Type::Vec3; break;
            case 'p': t = Type::Vec4; break;
            case 's': t = Type::String; break;
            case '3': t = Type::Mat3; break;
            case '4': t = Type::Mat4; break;
            default: return Type::Void;
        }
        if (!array) return t;
        const Type a = arrayOf(t);
        return a == Type::Void ? Type::Void : a;
    }

    bool attr(Token& t, Type prefix, std::string& error) {
        // at '@'
        advance();
        if (!identStart(peek())) {
            error = at(t.pos, "a name must follow @");
            return false;
        }
        const size_t start = i_;
        while (identChar(peek())) advance();
        t.kind = Tok::Attr;
        t.text = s_.substr(start, i_ - start);
        t.attrType = prefix;
        return true;
    }

    bool one(Token& t, std::string& error) {
        const char c = peek();
        // A prefixed attribute: v@P, i[]@list, 3@xform.
        if ((c == 'i' || c == 'f' || c == 'u' || c == 'v' || c == 'p' || c == 's' || c == '3' || c == '4') &&
            (peek(1) == '@' || (peek(1) == '[' && peek(2) == ']' && peek(3) == '@'))) {
            const bool array = peek(1) == '[';
            const Type type = prefixType(c, array);
            if (type == Type::Void) {
                error = at(t.pos, std::string("no attributes of arrays of ") + (c == '3' ? "matrix3" : "matrix"));
                return false;
            }
            advance();
            if (array) {
                advance();
                advance();
            }
            return attr(t, type, error);
        }
        if (c == '@') return attr(t, Type::Void, error);
        if (c == '$') {
            advance();
            if (!identStart(peek())) {
                error = at(t.pos, "a name must follow $");
                return false;
            }
            const size_t start = i_;
            while (identChar(peek())) advance();
            t.kind = Tok::Dollar;
            t.text = s_.substr(start, i_ - start);
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            return number(t, error);
        }
        if (identStart(c)) {
            const size_t start = i_;
            while (identChar(peek())) advance();
            t.kind = Tok::Ident;
            t.text = s_.substr(start, i_ - start);
            return true;
        }
        if (c == '"' || c == '\'') return string(t, c, error);

        auto two = [&](char second, Tok ifTwo, Tok ifOne) {
            advance();
            if (peek() == second) {
                advance();
                t.kind = ifTwo;
            } else {
                t.kind = ifOne;
            }
            return true;
        };
        switch (c) {
            case '(': advance(); t.kind = Tok::LParen; return true;
            case ')': advance(); t.kind = Tok::RParen; return true;
            case '{': advance(); t.kind = Tok::LBrace; return true;
            case '}': advance(); t.kind = Tok::RBrace; return true;
            case '[': advance(); t.kind = Tok::LBracket; return true;
            case ']': advance(); t.kind = Tok::RBracket; return true;
            case ',': advance(); t.kind = Tok::Comma; return true;
            case ';': advance(); t.kind = Tok::Semi; return true;
            case '.': advance(); t.kind = Tok::Dot; return true;
            case '?': advance(); t.kind = Tok::Question; return true;
            case ':': advance(); t.kind = Tok::Colon; return true;
            case '~': advance(); t.kind = Tok::Tilde; return true;
            case '^': advance(); t.kind = Tok::Caret; return true;
            case '*': return two('=', Tok::StarEq, Tok::Star);
            case '/': return two('=', Tok::SlashEq, Tok::Slash);
            case '%': return two('=', Tok::PercentEq, Tok::Percent);
            case '!': return two('=', Tok::Ne, Tok::Not);
            case '=': return two('=', Tok::EqEq, Tok::Assign);
            case '+':
                advance();
                if (peek() == '+') { advance(); t.kind = Tok::PlusPlus; }
                else if (peek() == '=') { advance(); t.kind = Tok::PlusEq; }
                else t.kind = Tok::Plus;
                return true;
            case '-':
                advance();
                if (peek() == '-') { advance(); t.kind = Tok::MinusMinus; }
                else if (peek() == '=') { advance(); t.kind = Tok::MinusEq; }
                else t.kind = Tok::Minus;
                return true;
            case '&': return two('&', Tok::AndAnd, Tok::Amp);
            case '|': return two('|', Tok::OrOr, Tok::Pipe);
            case '<':
                advance();
                if (peek() == '<') { advance(); t.kind = Tok::Shl; }
                else if (peek() == '=') { advance(); t.kind = Tok::Le; }
                else t.kind = Tok::Lt;
                return true;
            case '>':
                advance();
                if (peek() == '>') { advance(); t.kind = Tok::Shr; }
                else if (peek() == '=') { advance(); t.kind = Tok::Ge; }
                else t.kind = Tok::Gt;
                return true;
            default:
                break;
        }
        error = at(t.pos, std::string("unexpected character '") + c + "'");
        return false;
    }

    bool number(Token& t, std::string& error) {
        const size_t start = i_;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance();
            advance();
            const size_t digits = i_;
            while (std::isxdigit(static_cast<unsigned char>(peek()))) advance();
            if (i_ == digits) {
                error = at(t.pos, "hex digits must follow 0x");
                return false;
            }
            t.kind = Tok::Int;
            t.ival = static_cast<int64_t>(std::strtoull(s_.substr(digits, i_ - digits).c_str(), nullptr, 16));
            return true;
        }
        bool real = false;
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        if (peek() == '.' && !identStart(peek(1))) {  // 1.x is not a number with a dot
            real = true;
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (std::isdigit(static_cast<unsigned char>(peek(1))) ||
             ((peek(1) == '-' || peek(1) == '+') && std::isdigit(static_cast<unsigned char>(peek(2)))))) {
            real = true;
            advance();
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        const std::string text = s_.substr(start, i_ - start);
        if (identStart(peek())) {
            error = at(t.pos, "a number runs into a name: '" + text + peek() + "'");
            return false;
        }
        if (real) {
            t.kind = Tok::Float;
            t.fval = std::strtod(text.c_str(), nullptr);
        } else {
            t.kind = Tok::Int;
            t.ival = static_cast<int64_t>(std::strtoull(text.c_str(), nullptr, 10));
            if (t.ival > 2147483647LL) {  // too big for an int: a real number
                t.kind = Tok::Float;
                t.fval = std::strtod(text.c_str(), nullptr);
            }
        }
        return true;
    }

    bool string(Token& t, char quote, std::string& error) {
        advance();
        std::string out;
        while (i_ < s_.size() && s_[i_] != quote) {
            char c = s_[i_];
            if (c == '\n') break;
            if (c == '\\' && i_ + 1 < s_.size()) {
                advance();
                c = s_[i_];
                switch (c) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '0': c = '\0'; break;
                    default: break;  // \" \\ \' and the rest as they are
                }
            }
            out.push_back(c);
            advance();
        }
        if (i_ >= s_.size() || s_[i_] != quote) {
            error = at(t.pos, "a string that does not end");
            return false;
        }
        advance();
        t.kind = Tok::String;
        t.text = std::move(out);
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1;
    size_t lineStart_ = 0;
};

// --- parser ----------------------------------------------------------------------------------

bool typeKeyword(const std::string& w, Type& out) {
    if (w == "int") out = Type::Int;
    else if (w == "float") out = Type::Float;
    else if (w == "vector2") out = Type::Vec2;
    else if (w == "vector") out = Type::Vec3;
    else if (w == "vector4") out = Type::Vec4;
    else if (w == "matrix3") out = Type::Mat3;
    else if (w == "matrix") out = Type::Mat4;
    else if (w == "string") out = Type::String;
    else if (w == "void") out = Type::Void;
    else return false;
    return true;
}

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

    bool program(Ast& ast) {
        while (peek().kind != Tok::End) {
            if (functionAhead()) {
                Function f;
                if (!function(f)) return false;
                ast.functions.push_back(std::move(f));
                continue;
            }
            StmtPtr s = statement(true);
            if (!s) return false;
            ast.main.push_back(std::move(s));
        }
        return true;
    }

    std::unique_ptr<Expr> lone() {
        auto e = expression();
        if (!e) return nullptr;
        if (peek().kind == Tok::Semi) next();
        if (peek().kind != Tok::End) {
            fail("the expression ends here; what follows is too much");
            return nullptr;
        }
        return e;
    }

    const std::string& error() const { return error_; }

private:
    const Token& peek(size_t k = 0) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
    const Token& next() { return t_[i_ < t_.size() - 1 ? i_++ : i_]; }
    bool accept(Tok k) {
        if (peek().kind != k) return false;
        next();
        return true;
    }
    bool fail(const std::string& message) { return failAt(peek().pos, message); }
    bool failAt(const Pos& p, const std::string& message) {
        if (error_.empty()) error_ = at(p, message);
        return false;
    }
    bool expect(Tok k, const char* what) {
        if (accept(k)) return true;
        return fail(std::string("expected ") + what);
    }
    bool isType(size_t k = 0) const {
        Type t;
        return peek(k).kind == Tok::Ident && typeKeyword(peek(k).text, t);
    }

    /// [function] type [ '[' ']' ] name '('
    bool functionAhead() const {
        size_t k = 0;
        if (peek().kind == Tok::Ident && peek().text == "function") k = 1;
        if (!isType(k)) return false;
        ++k;
        if (peek(k).kind == Tok::LBracket && peek(k + 1).kind == Tok::RBracket) k += 2;
        return peek(k).kind == Tok::Ident && peek(k + 1).kind == Tok::LParen;
    }

    /// type, and [] after it for an array.
    bool type(Type& out, bool allowVoid) {
        const Token& w = peek();
        if (w.kind != Tok::Ident || !typeKeyword(w.text, out)) return fail("expected a type");
        if (out == Type::Void && !allowVoid) return fail("a variable cannot be void");
        next();
        if (peek().kind == Tok::LBracket && peek(1).kind == Tok::RBracket) {
            next();
            next();
            return array(out, w.pos);
        }
        return true;
    }

    bool array(Type& t, const Pos& p) {
        const Type a = arrayOf(t);
        if (a == Type::Void) return failAt(p, std::string("no arrays of ") + typeName(t));
        t = a;
        return true;
    }

    bool function(Function& f) {
        if (peek().text == "function") next();
        f.pos = peek().pos;
        if (!type(f.ret, true)) return false;
        f.name = next().text;
        if (builtins().count(f.name) || f.name == "set" || f.name == "array") {
            return failAt(f.pos, "'" + f.name + "' is a builtin; a function of its own needs another name");
        }
        next();  // (
        if (!accept(Tok::RParen)) {
            for (;;) {
                Type t;
                const Pos tp = peek().pos;
                if (peek().kind == Tok::Ident && peek().text == "const") next();
                if (!type(t, false)) return false;
                // float a, b; vector c  --  or C's  float a, float b
                for (;;) {
                    if (peek().kind != Tok::Ident) return fail("expected a parameter's name");
                    Param p{t, next().text, tp};
                    if (peek().kind == Tok::LBracket && peek(1).kind == Tok::RBracket) {
                        next();
                        next();
                        if (!array(p.type, tp)) return false;
                    }
                    f.params.push_back(std::move(p));
                    if (peek().kind == Tok::Comma && !isType(1) && !(peek(1).kind == Tok::Ident && peek(1).text == "const")) {
                        next();
                        continue;
                    }
                    break;
                }
                if (accept(Tok::RParen)) break;
                if (!accept(Tok::Semi) && !accept(Tok::Comma)) return fail("expected ',', ';' or ')'");
            }
        }
        if (peek().kind != Tok::LBrace) return fail("expected '{' and the function's body");
        f.body = statement(false);
        return f.body != nullptr;
    }

    /// `last`: a statement at the end of the program may leave out its ';'.
    StmtPtr statement(bool topLevel) {
        auto s = std::make_unique<Stmt>();
        s->pos = peek().pos;
        const Token& w = peek();
        if (w.kind == Tok::Semi) {
            next();
            s->kind = SK::Empty;
            return s;
        }
        if (w.kind == Tok::LBrace) {
            next();
            s->kind = SK::Block;
            while (peek().kind != Tok::RBrace) {
                if (peek().kind == Tok::End) {
                    fail("expected '}'");
                    return nullptr;
                }
                StmtPtr inner = statement(false);
                if (!inner) return nullptr;
                s->body.push_back(std::move(inner));
            }
            next();
            return s;
        }
        if (w.kind == Tok::Ident) {
            if (w.text == "if") return ifStatement(std::move(s));
            if (w.text == "while") {
                next();
                s->kind = SK::While;
                if (!expect(Tok::LParen, "'('")) return nullptr;
                s->expr = expression();
                if (!s->expr || !expect(Tok::RParen, "')'")) return nullptr;
                StmtPtr body = statement(false);
                if (!body) return nullptr;
                s->body.push_back(std::move(body));
                return s;
            }
            if (w.text == "do") {
                next();
                s->kind = SK::DoWhile;
                StmtPtr body = statement(false);
                if (!body) return nullptr;
                s->body.push_back(std::move(body));
                if (!(peek().kind == Tok::Ident && peek().text == "while")) {
                    fail("expected 'while' after do's body");
                    return nullptr;
                }
                next();
                if (!expect(Tok::LParen, "'('")) return nullptr;
                s->expr = expression();
                if (!s->expr || !expect(Tok::RParen, "')'")) return nullptr;
                if (!endOfStatement(topLevel)) return nullptr;
                return s;
            }
            if (w.text == "for") return forStatement(std::move(s));
            if (w.text == "foreach") return foreachStatement(std::move(s));
            if (w.text == "break" || w.text == "continue") {
                s->kind = w.text == "break" ? SK::Break : SK::Continue;
                next();
                if (!endOfStatement(topLevel)) return nullptr;
                return s;
            }
            if (w.text == "return") {
                next();
                s->kind = SK::Return;
                if (peek().kind != Tok::Semi && peek().kind != Tok::RBrace && peek().kind != Tok::End) {
                    s->expr = expression();
                    if (!s->expr) return nullptr;
                }
                if (!endOfStatement(topLevel)) return nullptr;
                return s;
            }
            if (w.text == "else") {
                fail("'else' without an 'if'");
                return nullptr;
            }
            if (w.text == "const" && isType(1)) next();
            if (isType() && peek(1).kind != Tok::LParen) {
                if (!declaration(*s)) return nullptr;
                if (!endOfStatement(topLevel)) return nullptr;
                return s;
            }
        }
        s->kind = SK::Expr;
        s->expr = expression();
        if (!s->expr) return nullptr;
        if (!endOfStatement(topLevel)) return nullptr;
        return s;
    }

    /// ';' -- or nothing before the end of the program or of a block.
    bool endOfStatement(bool) {
        if (accept(Tok::Semi)) return true;
        if (peek().kind == Tok::End || peek().kind == Tok::RBrace) return true;
        return fail("expected ';'");
    }

    bool declaration(Stmt& s) {
        s.kind = SK::Decl;
        const Pos tp = peek().pos;
        if (!type(s.declType, false)) return false;
        for (;;) {
            if (peek().kind != Tok::Ident) return fail("expected a variable's name");
            Type kw;
            if (typeKeyword(peek().text, kw)) return fail("'" + peek().text + "' is a type, not a name");
            Declarator d;
            d.pos = peek().pos;
            d.name = next().text;
            if (peek().kind == Tok::LBracket) {
                next();
                if (!expect(Tok::RBracket, "']' -- an array's size is not given")) return false;
                if (isArray(s.declType)) return failAt(tp, "an array of arrays");
                d.array = true;
            }
            if (accept(Tok::Assign)) {
                d.init = assignment();
                if (!d.init) return false;
            }
            s.decls.push_back(std::move(d));
            if (!accept(Tok::Comma)) break;
        }
        return true;
    }

    StmtPtr ifStatement(StmtPtr s) {
        next();
        s->kind = SK::If;
        if (!expect(Tok::LParen, "'('")) return nullptr;
        s->expr = expression();
        if (!s->expr || !expect(Tok::RParen, "')'")) return nullptr;
        StmtPtr then = statement(false);
        if (!then) return nullptr;
        s->body.push_back(std::move(then));
        if (peek().kind == Tok::Ident && peek().text == "else") {
            next();
            StmtPtr other = statement(false);
            if (!other) return nullptr;
            s->body.push_back(std::move(other));
        }
        return s;
    }

    StmtPtr forStatement(StmtPtr s) {
        next();
        s->kind = SK::For;
        if (!expect(Tok::LParen, "'('")) return nullptr;
        auto init = std::make_unique<Stmt>();
        init->pos = peek().pos;
        if (peek().kind == Tok::Semi) {
            init->kind = SK::Empty;
        } else if (isType()) {
            if (!declaration(*init)) return nullptr;
        } else {
            init->kind = SK::Expr;
            init->expr = expression();
            if (!init->expr) return nullptr;
        }
        if (!expect(Tok::Semi, "';'")) return nullptr;
        if (peek().kind != Tok::Semi) {
            s->expr = expression();
            if (!s->expr) return nullptr;
        }
        if (!expect(Tok::Semi, "';'")) return nullptr;
        if (peek().kind != Tok::RParen) {
            s->step = expression();
            if (!s->step) return nullptr;
        }
        if (!expect(Tok::RParen, "')'")) return nullptr;
        StmtPtr body = statement(false);
        if (!body) return nullptr;
        s->body.push_back(std::move(body));
        s->body.push_back(std::move(init));
        return s;
    }

    /// foreach (type value; array)  or  foreach (int index; type value; array)
    StmtPtr foreachStatement(StmtPtr s) {
        next();
        s->kind = SK::Foreach;
        if (!expect(Tok::LParen, "'('")) return nullptr;
        auto var = [&](Type& t, std::string& name) {
            if (!type(t, false)) return false;
            if (peek().kind != Tok::Ident) return fail("expected a variable's name");
            name = next().text;
            if (peek().kind == Tok::LBracket && peek(1).kind == Tok::RBracket) {
                next();
                next();
                return array(t, peek().pos);
            }
            return true;
        };
        Type t1 = Type::Void;
        std::string n1;
        if (!var(t1, n1) || !expect(Tok::Semi, "';'")) return nullptr;
        if (isType() && peek(1).kind == Tok::Ident && peek(2).kind == Tok::Semi) {
            s->indexType = t1;
            s->indexName = n1;
            if (!var(s->valueType, s->valueName) || !expect(Tok::Semi, "';'")) return nullptr;
        } else {
            s->valueType = t1;
            s->valueName = n1;
        }
        s->expr = expression();
        if (!s->expr || !expect(Tok::RParen, "')'")) return nullptr;
        StmtPtr body = statement(false);
        if (!body) return nullptr;
        s->body.push_back(std::move(body));
        return s;
    }

    // --- expressions ------------------------------------------------------------------------

    std::unique_ptr<Expr> expression() { return assignment(); }

    static bool assignable(const Expr& e) {
        switch (e.kind) {
            case EK::Var:
            case EK::Attr: return true;
            case EK::Member:
            case EK::Index: return assignable(*e.args[0]);
            default: return false;
        }
    }

    std::unique_ptr<Expr> assignment() {
        const Pos p = peek().pos;
        auto lhs = ternary();
        if (!lhs) return nullptr;
        const Tok k = peek().kind;
        if (k == Tok::Assign || k == Tok::PlusEq || k == Tok::MinusEq || k == Tok::StarEq || k == Tok::SlashEq ||
            k == Tok::PercentEq) {
            if (!assignable(*lhs)) {
                failAt(p, "cannot assign to this: only to a variable, an attribute, or a part of one");
                return nullptr;
            }
            next();
            auto rhs = assignment();
            if (!rhs) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = EK::Assign;
            e->pos = p;
            e->op = k;
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            return e;
        }
        return lhs;
    }

    std::unique_ptr<Expr> ternary() {
        auto cond = binary(0);
        if (!cond) return nullptr;
        if (peek().kind != Tok::Question) return cond;
        const Pos p = peek().pos;
        next();
        auto a = expression();
        if (!a || !expect(Tok::Colon, "':'")) return nullptr;
        auto b = ternary();
        if (!b) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = EK::Ternary;
        e->pos = p;
        e->args.push_back(std::move(cond));
        e->args.push_back(std::move(a));
        e->args.push_back(std::move(b));
        return e;
    }

    /// Binary operators by precedence, lowest first.
    static int precedence(Tok k) {
        switch (k) {
            case Tok::OrOr: return 1;
            case Tok::AndAnd: return 2;
            case Tok::Pipe: return 3;
            case Tok::Caret: return 4;
            case Tok::Amp: return 5;
            case Tok::EqEq: case Tok::Ne: return 6;
            case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 7;
            case Tok::Shl: case Tok::Shr: return 8;
            case Tok::Plus: case Tok::Minus: return 9;
            case Tok::Star: case Tok::Slash: case Tok::Percent: return 10;
            default: return 0;
        }
    }

    std::unique_ptr<Expr> binary(int minPrec) {
        auto lhs = unary();
        if (!lhs) return nullptr;
        for (;;) {
            const Tok k = peek().kind;
            const int prec = precedence(k);
            if (prec == 0 || prec <= minPrec) return lhs;
            const Pos p = peek().pos;
            next();
            auto rhs = binary(prec);
            if (!rhs) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = EK::Binary;
            e->pos = p;
            e->op = k;
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            lhs = std::move(e);
        }
    }

    std::unique_ptr<Expr> unary() {
        const Tok k = peek().kind;
        const Pos p = peek().pos;
        if (k == Tok::Plus) {
            next();
            return unary();
        }
        if (k == Tok::Minus || k == Tok::Not || k == Tok::Tilde || k == Tok::PlusPlus || k == Tok::MinusMinus) {
            next();
            auto inner = unary();
            if (!inner) return nullptr;
            auto e = std::make_unique<Expr>();
            e->pos = p;
            if (k == Tok::PlusPlus || k == Tok::MinusMinus) {
                if (!assignable(*inner)) {
                    failAt(p, "++ and -- need a variable or an attribute");
                    return nullptr;
                }
                e->kind = k == Tok::PlusPlus ? EK::PreInc : EK::PreDec;
            } else {
                e->kind = EK::Unary;
                e->op = k;
            }
            e->args.push_back(std::move(inner));
            return e;
        }
        return postfix();
    }

    std::unique_ptr<Expr> postfix() {
        auto e = primary();
        if (!e) return nullptr;
        for (;;) {
            const Pos p = peek().pos;
            if (accept(Tok::Dot)) {
                if (peek().kind != Tok::Ident) {
                    fail("expected a component after '.'");
                    return nullptr;
                }
                auto m = std::make_unique<Expr>();
                m->kind = EK::Member;
                m->pos = p;
                m->text = next().text;
                m->args.push_back(std::move(e));
                e = std::move(m);
            } else if (accept(Tok::LBracket)) {
                auto idx = expression();
                if (!idx || !expect(Tok::RBracket, "']'")) return nullptr;
                auto m = std::make_unique<Expr>();
                m->kind = EK::Index;
                m->pos = p;
                m->args.push_back(std::move(e));
                m->args.push_back(std::move(idx));
                e = std::move(m);
            } else if (peek().kind == Tok::PlusPlus || peek().kind == Tok::MinusMinus) {
                if (!assignable(*e)) {
                    fail("++ and -- need a variable or an attribute");
                    return nullptr;
                }
                auto m = std::make_unique<Expr>();
                m->kind = next().kind == Tok::PlusPlus ? EK::PostInc : EK::PostDec;
                m->pos = p;
                m->args.push_back(std::move(e));
                e = std::move(m);
            } else {
                return e;
            }
        }
    }

    bool arguments(Expr& call, Tok close) {
        if (accept(close)) return true;
        for (;;) {
            auto a = expression();
            if (!a) return false;
            call.args.push_back(std::move(a));
            if (accept(Tok::Comma)) continue;
            if (accept(close)) return true;
            return fail(close == Tok::RParen ? "expected ',' or ')'" : "expected ',' or '}'");
        }
    }

    std::unique_ptr<Expr> primary() {
        const Token& w = peek();
        auto e = std::make_unique<Expr>();
        e->pos = w.pos;
        switch (w.kind) {
            case Tok::Int:
                e->kind = EK::Int;
                e->ival = next().ival;
                return e;
            case Tok::Float:
                e->kind = EK::Float;
                e->fval = next().fval;
                return e;
            case Tok::String:
                e->kind = EK::String;
                e->text = next().text;
                return e;
            case Tok::Dollar:
                e->kind = EK::Dollar;
                e->text = next().text;
                return e;
            case Tok::Attr: {
                e->kind = EK::Attr;
                e->type = w.attrType;
                std::string name = next().text;
                // @opinput1_P: P of the same element of input 1.
                if (name.rfind("opinput", 0) == 0 && name.size() > 9 && std::isdigit(static_cast<unsigned char>(name[7])) &&
                    name[8] == '_') {
                    e->input = name[7] - '0';
                    if (e->input > 3) {
                        failAt(e->pos, "there are inputs 0 to 3");
                        return nullptr;
                    }
                    name = name.substr(9);
                }
                e->text = std::move(name);
                return e;
            }
            case Tok::LParen: {
                next();
                auto inner = expression();
                if (!inner || !expect(Tok::RParen, "')'")) return nullptr;
                return inner;
            }
            case Tok::LBrace:
                next();
                e->kind = EK::Braces;
                if (!arguments(*e, Tok::RBrace)) return nullptr;
                return e;
            case Tok::Ident: {
                const std::string name = next().text;
                Type cast;
                if (peek().kind == Tok::LParen) {
                    next();
                    if (typeKeyword(name, cast)) {
                        if (cast == Type::Void) {
                            failAt(e->pos, "void is not a value");
                            return nullptr;
                        }
                        e->kind = EK::Cast;
                        e->type = cast;
                    } else {
                        e->kind = EK::Call;
                        e->text = name;
                    }
                    if (!arguments(*e, Tok::RParen)) return nullptr;
                    if (e->kind == EK::Cast && e->args.empty()) {
                        failAt(e->pos, name + "() takes what it makes from: an argument or more");
                        return nullptr;
                    }
                    return e;
                }
                if (typeKeyword(name, cast)) {
                    failAt(e->pos, "a type where a value was expected: '" + name + "'");
                    return nullptr;
                }
                static const std::set<std::string> reserved = {"if", "else", "for", "foreach", "while", "do",
                                                               "break", "continue", "return", "function"};
                if (reserved.count(name)) {
                    failAt(e->pos, "'" + name + "' cannot be used here");
                    return nullptr;
                }
                e->kind = EK::Var;
                e->text = name;
                return e;
            }
            case Tok::End:
                fail("the program ends where an expression was expected");
                return nullptr;
            default:
                fail("expected an expression");
                return nullptr;
        }
    }

    std::vector<Token> t_;
    size_t i_ = 0;
    std::string error_;
};

// --- what the parse can already tell ---------------------------------------------------------

class Scan {
public:
    Scan(Ast& ast, std::string& error) : ast_(ast), error_(error) {
        for (const Function& f : ast.functions) functions_[f.name] = &f;
    }

    bool all() {
        std::set<std::string> seen;
        for (const Function& f : ast_.functions) {
            if (!seen.insert(f.name).second) return failAt(f.pos, "a second function called '" + f.name + "'");
        }
        for (const Function& f : ast_.functions) {
            if (!stmt(*f.body)) return false;
        }
        for (const StmtPtr& s : ast_.main) {
            if (!stmt(*s)) return false;
        }
        return true;
    }

    bool expr(const Expr& e) {
        switch (e.kind) {
            case EK::Attr:
                noteAttr(e.text);
                if (e.text == "Time" || e.text == "Frame" || e.text == "TimeInc") ast_.readsTime = true;
                break;
            case EK::Dollar:
                if (e.text == "F" || e.text == "FF" || e.text == "T") ast_.readsTime = true;
                break;
            case EK::Member:
                if (e.args[0]->kind == EK::Attr && e.args[0]->text == "P" && e.args[0]->input == 0 &&
                    (e.args[0]->type == Type::Void || e.args[0]->type == Type::Vec3)) {
                    static const std::set<std::string> xyz = {"x", "y", "z", "r", "g", "b"};
                    if (!xyz.count(e.text)) return failAt(e.pos, "P is a vector: it has no component '" + e.text + "'");
                }
                break;
            case EK::Call:
                if (!call(e)) return false;
                break;
            default:
                break;
        }
        for (const auto& a : e.args) {
            if (!expr(*a)) return false;
        }
        return true;
    }

private:
    bool failAt(const Pos& p, const std::string& message) {
        if (error_.empty()) error_ = at(p, message);
        return false;
    }

    void noteAttr(const std::string& name) {
        if (std::find(ast_.attrNames.begin(), ast_.attrNames.end(), name) == ast_.attrNames.end()) {
            ast_.attrNames.push_back(name);
        }
    }

    bool call(const Expr& e) {
        const std::string& name = e.text;
        const size_t n = e.args.size();
        if (auto f = functions_.find(name); f != functions_.end()) {
            if (f->second->params.size() != n) {
                return failAt(e.pos, name + "() takes " + std::to_string(f->second->params.size()) +
                                         " argument(s), got " + std::to_string(n));
            }
            return true;
        }
        const Builtins& b = builtins();
        const auto it = b.find(name);
        if (it == b.end()) return failAt(e.pos, "unknown function '" + name + "'");
        // ch("name"): a parameter the node may make.
        static const std::map<std::string, Type, std::less<>> chans = {
            {"ch", Type::Float}, {"chf", Type::Float}, {"chi", Type::Int}, {"chv", Type::Vec3}, {"chs", Type::String},
            {"chu", Type::Vec2}, {"chp", Type::Vec4}};
        if (auto c = chans.find(name); c != chans.end() && n >= 1 && e.args[0]->kind == EK::String) {
            const std::string& path = e.args[0]->text;
            const bool known = std::any_of(ast_.channels.begin(), ast_.channels.end(),
                                           [&](const Channel& ch) { return ch.name == path; });
            if (!known) ast_.channels.push_back({path, c->second});
        }
        if (it->second.empty()) return arity(e, name, n);  // typed by the checker; arity below
        std::set<size_t> counts;
        for (const Overload& o : it->second) {
            if (o.params.size() == n || (o.variadic && n + 1 >= o.params.size())) return true;
            counts.insert(o.params.size());
        }
        std::string list;
        for (size_t c : counts) list += (list.empty() ? "" : " or ") + std::to_string(c);
        return failAt(e.pos, name + "() takes " + list + " argument(s), got " + std::to_string(n));
    }

    /// The builtins the checker types itself: how many arguments they take.
    bool arity(const Expr& e, const std::string& name, size_t n) {
        static const std::map<std::string, std::pair<size_t, size_t>, std::less<>> counts = {
            {"ch", {1, 1}}, {"chf", {1, 1}}, {"chi", {1, 1}}, {"chv", {1, 1}}, {"chs", {1, 1}}, {"chu", {1, 1}},
            {"chp", {1, 1}}, {"set", {1, 16}}, {"array", {0, 1000}}, {"point", {3, 3}}, {"prim", {3, 3}},
            {"vertex", {3, 4}}, {"detail", {2, 2}}, {"rand", {1, 2}}, {"random", {1, 2}}, {"noise", {1, 1}},
            {"ident", {0, 0}}, {"printf", {1, 1000}}, {"sprintf", {1, 1000}}, {"warning", {1, 1000}},
            {"error", {1, 1000}}, {"len", {1, 1}}, {"append", {2, 2}}, {"push", {2, 2}}, {"pop", {1, 1}},
            {"insert", {3, 3}}, {"removeindex", {2, 2}}, {"removevalue", {2, 2}}, {"find", {2, 2}},
            {"sort", {1, 1}}, {"reverse", {1, 1}}, {"resize", {2, 2}}, {"isvalidindex", {2, 2}},
            {"setpointattrib", {4, 5}}, {"setprimattrib", {4, 5}}, {"setvertexattrib", {5, 6}},
            {"setdetailattrib", {3, 4}}, {"sum", {1, 1}}, {"avg", {1, 16}}, {"argsort", {1, 1}}, {"slice", {3, 3}},
            {"min", {1, 16}}, {"max", {1, 16}}, {"concat", {1, 1000}}};
        const auto it = counts.find(name);
        if (it == counts.end()) return true;
        if (n >= it->second.first && n <= it->second.second) return true;
        const std::string want = it->second.first == it->second.second
                                     ? std::to_string(it->second.first)
                                     : std::to_string(it->second.first) + " to " + std::to_string(it->second.second);
        return failAt(e.pos, name + "() takes " + want + " argument(s), got " + std::to_string(n));
    }

    bool stmt(const Stmt& s) {
        if (s.expr && !expr(*s.expr)) return false;
        if (s.step && !expr(*s.step)) return false;
        for (const Declarator& d : s.decls) {
            if (d.init && !expr(*d.init)) return false;
        }
        for (const StmtPtr& b : s.body) {
            if (!stmt(*b)) return false;
        }
        return true;
    }

    Ast& ast_;
    std::string& error_;
    std::map<std::string, const Function*> functions_;
};

}  // namespace

std::unique_ptr<Ast> parse(const std::string& source, bool expression, std::string& error) {
    error.clear();
    std::vector<Token> toks;
    Lexer lexer(source);
    if (!lexer.run(toks, error)) return nullptr;
    auto ast = std::make_unique<Ast>();
    Parser parser(std::move(toks));
    if (expression) {
        auto e = parser.lone();
        if (!e) {
            error = parser.error().empty() ? "not an expression" : parser.error();
            return nullptr;
        }
        auto s = std::make_unique<Stmt>();
        s->kind = SK::Return;
        s->pos = e->pos;
        s->expr = std::move(e);
        ast->main.push_back(std::move(s));
    } else if (!parser.program(*ast)) {
        error = parser.error().empty() ? "does not parse" : parser.error();
        return nullptr;
    }
    Scan scan(*ast, error);
    if (!scan.all()) return nullptr;
    return ast;
}

}  // namespace pg::lang
