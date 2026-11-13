#include "gl_loader.h"
#include <cstdio>

namespace snp::gles3 {

namespace gl {
    PFNGLGETERRORPROC glGetError = nullptr;
    PFNGLGETSTRINGPROC glGetString = nullptr;
    PFNGLVIEWPORTPROC glViewport = nullptr;
    PFNGLSCISSORPROC glScissor = nullptr;
    PFNGLENABLEPROC glEnable = nullptr;
    PFNGLDISABLEPROC glDisable = nullptr;
    PFNGLCLEARCOLORPROC glClearColor = nullptr;
    PFNGLCLEARDEPTHFPROC glClearDepthf = nullptr;
    PFNGLCLEARPROC glClear = nullptr;
    PFNGLDEPTHFUNCPROC glDepthFunc = nullptr;
    PFNGLDEPTHMASKPROC glDepthMask = nullptr;
    PFNGLPOLYGONOFFSETPROC glPolygonOffset = nullptr;
    PFNGLBLENDFUNCPROC glBlendFunc = nullptr;
    PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate = nullptr;
    PFNGLBLENDEQUATIONPROC glBlendEquation = nullptr;
    PFNGLCOLORMASKPROC glColorMask = nullptr;
    PFNGLCULLFACEPROC glCullFace = nullptr;
    PFNGLFRONTFACEPROC glFrontFace = nullptr;
    PFNGLGENTEXTURESPROC glGenTextures = nullptr;
    PFNGLDELETETEXTURESPROC glDeleteTextures = nullptr;
    PFNGLBINDTEXTUREPROC glBindTexture = nullptr;
    PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
    PFNGLTEXIMAGE2DPROC glTexImage2D = nullptr;
    PFNGLTEXSUBIMAGE2DPROC glTexSubImage2D = nullptr;
    PFNGLTEXPARAMETERIPROC glTexParameteri = nullptr;
    PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
    PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
    PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
    PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
    PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
    PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
    PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
    PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
    PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
    PFNGLBUFFERDATAPROC glBufferData = nullptr;
    PFNGLBUFFERSUBDATAPROC glBufferSubData = nullptr;
    PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
    PFNGLCREATESHADERPROC glCreateShader = nullptr;
    PFNGLDELETESHADERPROC glDeleteShader = nullptr;
    PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
    PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
    PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
    PFNGLATTACHSHADERPROC glAttachShader = nullptr;
    PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
    PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
    PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
    PFNGLUNIFORM1IPROC glUniform1i = nullptr;
    PFNGLUNIFORM1FPROC glUniform1f = nullptr;
    PFNGLUNIFORM2FPROC glUniform2f = nullptr;
    PFNGLUNIFORM4FPROC glUniform4f = nullptr;
    PFNGLUNIFORM4FVPROC glUniform4fv = nullptr;
    PFNGLDRAWARRAYSPROC glDrawArrays = nullptr;
    PFNGLDRAWELEMENTSPROC glDrawElements = nullptr;
    PFNGLREADPIXELSPROC glReadPixels = nullptr;
    PFNGLPIXELSTOREIPROC glPixelStorei = nullptr;
}

namespace {
    template <typename T>
    bool load_gl_func(T& func_ptr, const char* name) {
        func_ptr = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
        if (!func_ptr) {
            std::fprintf(stderr, "[GLES3] Missing GL function: %s\n", name);
            return false;
        }
        return true;
    }
}

bool init_gl_loader() {
    bool ok = true;
    ok &= load_gl_func(gl::glGetError, "glGetError");
    ok &= load_gl_func(gl::glGetString, "glGetString");
    ok &= load_gl_func(gl::glViewport, "glViewport");
    ok &= load_gl_func(gl::glScissor, "glScissor");
    ok &= load_gl_func(gl::glEnable, "glEnable");
    ok &= load_gl_func(gl::glDisable, "glDisable");
    ok &= load_gl_func(gl::glClearColor, "glClearColor");
    ok &= load_gl_func(gl::glClearDepthf, "glClearDepthf");
    ok &= load_gl_func(gl::glClear, "glClear");
    ok &= load_gl_func(gl::glDepthFunc, "glDepthFunc");
    ok &= load_gl_func(gl::glDepthMask, "glDepthMask");
    ok &= load_gl_func(gl::glPolygonOffset, "glPolygonOffset");
    ok &= load_gl_func(gl::glBlendFunc, "glBlendFunc");
    ok &= load_gl_func(gl::glBlendFuncSeparate, "glBlendFuncSeparate");
    ok &= load_gl_func(gl::glBlendEquation, "glBlendEquation");
    ok &= load_gl_func(gl::glColorMask, "glColorMask");
    ok &= load_gl_func(gl::glCullFace, "glCullFace");
    ok &= load_gl_func(gl::glFrontFace, "glFrontFace");
    ok &= load_gl_func(gl::glGenTextures, "glGenTextures");
    ok &= load_gl_func(gl::glDeleteTextures, "glDeleteTextures");
    ok &= load_gl_func(gl::glBindTexture, "glBindTexture");
    ok &= load_gl_func(gl::glActiveTexture, "glActiveTexture");
    ok &= load_gl_func(gl::glTexImage2D, "glTexImage2D");
    ok &= load_gl_func(gl::glTexSubImage2D, "glTexSubImage2D");
    ok &= load_gl_func(gl::glTexParameteri, "glTexParameteri");
    ok &= load_gl_func(gl::glGenFramebuffers, "glGenFramebuffers");
    ok &= load_gl_func(gl::glDeleteFramebuffers, "glDeleteFramebuffers");
    ok &= load_gl_func(gl::glBindFramebuffer, "glBindFramebuffer");
    ok &= load_gl_func(gl::glFramebufferTexture2D, "glFramebufferTexture2D");
    ok &= load_gl_func(gl::glCheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= load_gl_func(gl::glBlitFramebuffer, "glBlitFramebuffer");
    ok &= load_gl_func(gl::glGenRenderbuffers, "glGenRenderbuffers");
    ok &= load_gl_func(gl::glDeleteRenderbuffers, "glDeleteRenderbuffers");
    ok &= load_gl_func(gl::glBindRenderbuffer, "glBindRenderbuffer");
    ok &= load_gl_func(gl::glRenderbufferStorage, "glRenderbufferStorage");
    ok &= load_gl_func(gl::glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    ok &= load_gl_func(gl::glGenBuffers, "glGenBuffers");
    ok &= load_gl_func(gl::glDeleteBuffers, "glDeleteBuffers");
    ok &= load_gl_func(gl::glBindBuffer, "glBindBuffer");
    ok &= load_gl_func(gl::glBufferData, "glBufferData");
    ok &= load_gl_func(gl::glBufferSubData, "glBufferSubData");
    ok &= load_gl_func(gl::glGenVertexArrays, "glGenVertexArrays");
    ok &= load_gl_func(gl::glDeleteVertexArrays, "glDeleteVertexArrays");
    ok &= load_gl_func(gl::glBindVertexArray, "glBindVertexArray");
    ok &= load_gl_func(gl::glEnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= load_gl_func(gl::glVertexAttribPointer, "glVertexAttribPointer");
    ok &= load_gl_func(gl::glCreateShader, "glCreateShader");
    ok &= load_gl_func(gl::glDeleteShader, "glDeleteShader");
    ok &= load_gl_func(gl::glShaderSource, "glShaderSource");
    ok &= load_gl_func(gl::glCompileShader, "glCompileShader");
    ok &= load_gl_func(gl::glGetShaderiv, "glGetShaderiv");
    ok &= load_gl_func(gl::glGetShaderInfoLog, "glGetShaderInfoLog");
    ok &= load_gl_func(gl::glCreateProgram, "glCreateProgram");
    ok &= load_gl_func(gl::glDeleteProgram, "glDeleteProgram");
    ok &= load_gl_func(gl::glAttachShader, "glAttachShader");
    ok &= load_gl_func(gl::glLinkProgram, "glLinkProgram");
    ok &= load_gl_func(gl::glGetProgramiv, "glGetProgramiv");
    ok &= load_gl_func(gl::glGetProgramInfoLog, "glGetProgramInfoLog");
    ok &= load_gl_func(gl::glUseProgram, "glUseProgram");
    ok &= load_gl_func(gl::glGetUniformLocation, "glGetUniformLocation");
    ok &= load_gl_func(gl::glUniform1i, "glUniform1i");
    ok &= load_gl_func(gl::glUniform1f, "glUniform1f");
    ok &= load_gl_func(gl::glUniform2f, "glUniform2f");
    ok &= load_gl_func(gl::glUniform4f, "glUniform4f");
    ok &= load_gl_func(gl::glUniform4fv, "glUniform4fv");
    ok &= load_gl_func(gl::glDrawArrays, "glDrawArrays");
    ok &= load_gl_func(gl::glDrawElements, "glDrawElements");
    ok &= load_gl_func(gl::glReadPixels, "glReadPixels");
    ok &= load_gl_func(gl::glPixelStorei, "glPixelStorei");
    return ok;
}

} // namespace snp::gles3
