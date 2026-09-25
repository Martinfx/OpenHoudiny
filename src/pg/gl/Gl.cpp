#include "pg/gl/Gl.h"

namespace pg::gl {

bool Api::load(GetProc getProc, std::string& missing) {
#define PG_GL_LOAD(name, ret, params)                                         \
    name = reinterpret_cast<ret(PG_GLAPI*) params>(getProc("gl" #name));      \
    if (!name) {                                                              \
        missing = "gl" #name;                                                 \
        return false;                                                         \
    }
    PG_GL_FUNCTIONS(PG_GL_LOAD)
#undef PG_GL_LOAD
    return true;
}

}  // namespace pg::gl
