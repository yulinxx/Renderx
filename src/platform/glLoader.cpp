#include "glLoader.h"

#include <cstring>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__linux__)
    #include <GL/glx.h>
#elif defined(__APPLE__)
    #include <dlfcn.h>
#endif

// 进程级全局表：仅供旧 rhiGl 使用（见 glLoader.h 中的缺陷说明），
// Phase 6 删除旧后端时一并移除。
static GLFuncs g_funcs;

extern "C" GLFuncs* gl()
{
    return &g_funcs;
}

static void* default_get_proc_address(const char* name)
{
#ifdef _WIN32
    void* ptr = (void*)wglGetProcAddress(name);
    if (!ptr)
    {
        static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (opengl32)
        {
            ptr = (void*)GetProcAddress(opengl32, name);
        }
    }
    return ptr;
#elif defined(__linux__)
    return (void*)glXGetProcAddress((const GLubyte*)name);
#elif defined(__APPLE__)
    // macOS 没有 glXGetProcAddress/wglGetProcAddress。
    // OpenGL.framework 导出了全部 GL 符号，直接通过 dlsym 解析。
    return dlsym(RTLD_DEFAULT, name);
#else
    (void)name;
    return nullptr;
#endif
}

extern "C" bool gl_loader_load(GLFuncs* out, void* getProcAddress)
{
    if (!out)
    {
        return false;
    }

    typedef void* (*GetProcAddrFunc)(const char*);
    GetProcAddrFunc getProc =
        getProcAddress ? reinterpret_cast<GetProcAddrFunc>(getProcAddress) : default_get_proc_address;

    GLFuncs& f = *out;
    memset(out, 0, sizeof(GLFuncs));

    f.GenBuffers = (PFNGLGENBUFFERSPROC)getProc("glGenBuffers");
    f.DeleteBuffers = (PFNGLDELETEBUFFERSPROC)getProc("glDeleteBuffers");
    f.BindBuffer = (PFNGLBINDBUFFERPROC)getProc("glBindBuffer");
    f.BufferData = (PFNGLBUFFERDATAPROC)getProc("glBufferData");
    f.BufferSubData = (PFNGLBUFFERSUBDATAPROC)getProc("glBufferSubData");
    f.BufferStorage = (PFNGLBUFFERSTORAGEPROC)getProc("glBufferStorage");
    f.MapBufferRange = (PFNGLMAPBUFFERRANGEPROC)getProc("glMapBufferRange");
    f.UnmapBuffer = (PFNGLUNMAPBUFFERPROC)getProc("glUnmapBuffer");
    f.FlushMappedBufferRange = (PFNGLFLUSHMAPPEDBUFFERRANGEPROC)getProc("glFlushMappedBufferRange");
    f.InvalidateBufferData = (PFNGLINVALIDATEBUFFERDATAPROC)getProc("glInvalidateBufferData");
    f.CopyBufferSubData = (PFNGLCOPYBUFFERSUBDATAPROC)getProc("glCopyBufferSubData");

    f.GenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)getProc("glGenVertexArrays");
    f.DeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)getProc("glDeleteVertexArrays");
    f.BindVertexArray = (PFNGLBINDVERTEXARRAYPROC)getProc("glBindVertexArray");
    f.EnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)getProc("glEnableVertexAttribArray");
    f.VertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)getProc("glVertexAttribPointer");
    f.VertexAttribBinding = (PFNGLVERTEXATTRIBBINDINGPROC)getProc("glVertexAttribBinding");
    f.VertexAttribFormat = (PFNGLVERTEXATTRIBFORMATPROC)getProc("glVertexAttribFormat");
    f.BindVertexBuffer = (PFNGLBINDVERTEXBUFFERPROC)getProc("glBindVertexBuffer");
    f.VertexBindingDivisor = (PFNGLVERTEXBINDINGDIVISORPROC)getProc("glVertexBindingDivisor");

    f.DrawArrays = (PFNGLDRAWARRAYSPROC)getProc("glDrawArrays");
    f.DrawElements = (PFNGLDRAWELEMENTSPROC)getProc("glDrawElements");
    f.DrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)getProc("glDrawArraysInstanced");
    f.DrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)getProc("glDrawElementsInstanced");
    f.DrawArraysIndirect = (PFNGLDRAWARRAYSINDIRECTPROC)getProc("glDrawArraysIndirect");
    f.DrawElementsIndirect = (PFNGLDRAWELEMENTSINDIRECTPROC)getProc("glDrawElementsIndirect");
    f.MultiDrawArraysIndirect = (PFNGLMULTIDRAWARRAYSINDIRECTPROC)getProc("glMultiDrawArraysIndirect");
    f.MultiDrawElementsIndirect = (PFNGLMULTIDRAWELEMENTSINDIRECTPROC)getProc("glMultiDrawElementsIndirect");

    f.DispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)getProc("glDispatchCompute");
    f.MemoryBarrier = (PFNGLMEMORYBARRIERPROC)getProc("glMemoryBarrier");
    f.ShaderBinary = (PFNGLSHADERBINARYPROC)getProc("glShaderBinary");
    f.SpecializeShader = (PFNGLSPECIALIZESHADERPROC)getProc("glSpecializeShader");
    f.NamedBufferStorage = (PFNGLNAMEDBUFFERSTORAGEPROC)getProc("glNamedBufferStorage");

    f.CreateShader = (PFNGLCREATESHADERPROC)getProc("glCreateShader");
    f.DeleteShader = (PFNGLDELETESHADERPROC)getProc("glDeleteShader");
    f.ShaderSource = (PFNGLSHADERSOURCEPROC)getProc("glShaderSource");
    f.CompileShader = (PFNGLCOMPILESHADERPROC)getProc("glCompileShader");
    f.CreateProgram = (PFNGLCREATEPROGRAMPROC)getProc("glCreateProgram");
    f.DeleteProgram = (PFNGLDELETEPROGRAMPROC)getProc("glDeleteProgram");
    f.AttachShader = (PFNGLATTACHSHADERPROC)getProc("glAttachShader");
    f.LinkProgram = (PFNGLLINKPROGRAMPROC)getProc("glLinkProgram");
    f.UseProgram = (PFNGLUSEPROGRAMPROC)getProc("glUseProgram");
    f.GetShaderiv = (PFNGLGETSHADERIVPROC)getProc("glGetShaderiv");
    f.GetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)getProc("glGetShaderInfoLog");
    f.GetProgramiv = (PFNGLGETPROGRAMIVPROC)getProc("glGetProgramiv");
    f.GetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)getProc("glGetProgramInfoLog");
    f.GetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)getProc("glGetUniformLocation");
    f.Uniform1f = (PFNGLUNIFORM1FPROC)getProc("glUniform1f");
    f.Uniform2f = (PFNGLUNIFORM2FPROC)getProc("glUniform2f");
    f.Uniform3f = (PFNGLUNIFORM3FPROC)getProc("glUniform3f");
    f.Uniform4f = (PFNGLUNIFORM4FPROC)getProc("glUniform4f");
    f.Uniform1i = (PFNGLUNIFORM1IPROC)getProc("glUniform1i");
    f.UniformMatrix3fv = (PFNGLUNIFORMMATRIX3FVPROC)getProc("glUniformMatrix3fv");
    f.UniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)getProc("glUniformMatrix4fv");
    f.Uniform1ui = (PFNGLUNIFORM1UIPROC)getProc("glUniform1ui");
    f.Uniform2ui = (PFNGLUNIFORM2UIPROC)getProc("glUniform2ui");
    f.Uniform1fv = (PFNGLUNIFORM1FVPROC)getProc("glUniform1fv");
    f.Uniform2fv = (PFNGLUNIFORM2FVPROC)getProc("glUniform2fv");
    f.Uniform3fv = (PFNGLUNIFORM3FVPROC)getProc("glUniform3fv");
    f.Uniform4fv = (PFNGLUNIFORM4FVPROC)getProc("glUniform4fv");

    f.GenTextures = (PFNGLGENTEXTURESPROC)getProc("glGenTextures");
    f.DeleteTextures = (PFNGLDELETETEXTURESPROC)getProc("glDeleteTextures");
    f.BindTexture = (PFNGLBINDTEXTUREPROC)getProc("glBindTexture");
    f.TexImage2D = (PFNGLTEXIMAGE2DPROC)getProc("glTexImage2D");
    f.TexSubImage2D = (PFNGLTEXSUBIMAGE2DPROC)getProc("glTexSubImage2D");
    f.TexParameteri = (PFNGLTEXPARAMETERIPROC)getProc("glTexParameteri");
    f.ActiveTexture = (PFNGLACTIVETEXTUREPROC)getProc("glActiveTexture");

    f.Enable = (PFNGLENABLEPROC)getProc("glEnable");
    f.Disable = (PFNGLDISABLEPROC)getProc("glDisable");
    f.BlendFunc = (PFNGLBLENDFUNCPROC)getProc("glBlendFunc");
    f.LineWidth = (PFNGLLINEWIDTHPROC)getProc("glLineWidth");
    f.PointSize = (PFNGLPOINTSIZEPROC)getProc("glPointSize");
    f.Viewport = (PFNGLVIEWPORTPROC)getProc("glViewport");
    f.Scissor = (PFNGLSCISSORPROC)getProc("glScissor");
    f.ClearColor = (PFNGLCLEARCOLORPROC)getProc("glClearColor");
    f.Clear = (PFNGLCLEARPROC)getProc("glClear");
    f.ReadPixels = (PFNGLREADPIXELSPROC)getProc("glReadPixels");
    f.ColorMask = (PFNGLCOLORMASKPROC)getProc("glColorMask");
    f.DepthMask = (PFNGLDEPTHMASKPROC)getProc("glDepthMask");
    f.DepthFunc = (PFNGLDEPTHFUNCPROC)getProc("glDepthFunc");
    f.PolygonMode = (PFNGLPOLYGONMODEPROC)getProc("glPolygonMode");
    f.PolygonOffset = (PFNGLPOLYGONOFFSETPROC)getProc("glPolygonOffset");
    f.GetIntegerv = (PFNGLGETINTEGERVPROC)getProc("glGetIntegerv");
    f.GetFloatv = (PFNGLGETFLOATVPROC)getProc("glGetFloatv");
    f.Hint = (PFNGLHINTPROC)getProc("glHint");

    f.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)getProc("glGenFramebuffers");
    f.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)getProc("glDeleteFramebuffers");
    f.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)getProc("glBindFramebuffer");
    f.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)getProc("glFramebufferTexture2D");
    f.GenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)getProc("glGenRenderbuffers");
    f.DeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)getProc("glDeleteRenderbuffers");
    f.BindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)getProc("glBindRenderbuffer");
    f.RenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)getProc("glRenderbufferStorage");
    f.FramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)getProc("glFramebufferRenderbuffer");
    f.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)getProc("glCheckFramebufferStatus");

    f.CreateBuffers = (PFNGLCREATEBUFFERSPROC)getProc("glCreateBuffers");
    f.NamedBufferData = (PFNGLNAMEDBUFFERDATAPROC)getProc("glNamedBufferData");
    f.NamedBufferSubData = (PFNGLNAMEDBUFFERSUBDATAPROC)getProc("glNamedBufferSubData");
    f.MapNamedBufferRange = (PFNGLMAPNAMEDBUFFERRANGEPROC)getProc("glMapNamedBufferRange");
    f.UnmapNamedBuffer = (PFNGLUNMAPNAMEDBUFFERPROC)getProc("glUnmapNamedBuffer");
    f.FlushMappedNamedBufferRange =
        (PFNGLFLUSHMAPPEDNAMEDBUFFERRANGEPROC)getProc("glFlushMappedNamedBufferRange");
    f.CopyNamedBufferSubData = (PFNGLCOPYNAMEDBUFFERSUBDATAPROC)getProc("glCopyNamedBufferSubData");

    f.Flush = (PFNGLFLUSHPROC)getProc("glFlush");
    f.BindBufferRange = (PFNGLBINDBUFFERRANGEPROC)getProc("glBindBufferRange");

    f.CreateTextures = (PFNGLCREATETEXTURESPROC)getProc("glCreateTextures");
    f.TextureParameteri = (PFNGLTEXTUREPARAMETERIPROC)getProc("glTextureParameteri");
    f.TextureStorage2D = (PFNGLTEXTURESTORAGE2DPROC)getProc("glTextureStorage2D");
    f.TextureSubImage2D = (PFNGLTEXTURESUBIMAGE2DPROC)getProc("glTextureSubImage2D");
    f.BindTextureUnit = (PFNGLBINDTEXTUREUNITPROC)getProc("glBindTextureUnit");
    f.CreateVertexArrays = (PFNGLCREATEVERTEXARRAYSPROC)getProc("glCreateVertexArrays");
    f.EnableVertexArrayAttrib = (PFNGLENABLEVERTEXARRAYATTRIBPROC)getProc("glEnableVertexArrayAttrib");
    f.VertexArrayAttribBinding = (PFNGLVERTEXARRAYATTRIBBINDINGPROC)getProc("glVertexArrayAttribBinding");
    f.VertexArrayAttribFormat = (PFNGLVERTEXARRAYATTRIBFORMATPROC)getProc("glVertexArrayAttribFormat");
    f.VertexArrayVertexBuffer = (PFNGLVERTEXARRAYVERTEXBUFFERPROC)getProc("glVertexArrayVertexBuffer");
    f.VertexArrayElementBuffer = (PFNGLVERTEXARRAYELEMENTBUFFERPROC)getProc("glVertexArrayElementBuffer");

    f.FenceSync = (PFNGLFENCESYNCPROC)getProc("glFenceSync");
    f.ClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)getProc("glClientWaitSync");
    f.DeleteSync = (PFNGLDELETESYNCPROC)getProc("glDeleteSync");

    f.GetError = (PFNGLGETERRORPROC)getProc("glGetError");
    f.GetString = (PFNGLGETSTRINGPROC)getProc("glGetString");
    f.GetStringi = (PFNGLGETSTRINGIPROC)getProc("glGetStringi");
    f.GetBooleanv = (PFNGLGETBOOLEANVPROC)getProc("glGetBooleanv");

    // 新 RHI GL 后端补充入口（剔除/朝向、分离式混合、像素对齐、uniform block）。
    // 这些不进关键入口检查：缺失时由后端在 Capabilities 里降级声明，
    // 而不是让整个设备创建失败。
    f.FrontFace = (GLFuncs::PFNGLFRONTFACEPROC)getProc("glFrontFace");
    f.CullFace = (GLFuncs::PFNGLCULLFACEPROC)getProc("glCullFace");
    f.PixelStorei = (GLFuncs::PFNGLPIXELSTOREIPROC)getProc("glPixelStorei");
    f.BlendFuncSeparate = (GLFuncs::PFNGLBLENDFUNCSEPARATEPROC)getProc("glBlendFuncSeparate");
    f.BlendEquationSeparate = (GLFuncs::PFNGLBLENDEQUATIONSEPARATEPROC)getProc("glBlendEquationSeparate");
    f.GetUniformBlockIndex = (GLFuncs::PFNGLGETUNIFORMBLOCKINDEXPROC)getProc("glGetUniformBlockIndex");
    f.UniformBlockBinding = (GLFuncs::PFNGLUNIFORMBLOCKBINDINGPROC)getProc("glUniformBlockBinding");
    f.VertexAttribIPointer = (GLFuncs::PFNGLVERTEXATTRIBIPOINTERPROC)getProc("glVertexAttribIPointer");
    f.VertexAttribDivisor = (GLFuncs::PFNGLVERTEXATTRIBDIVISORPROC)getProc("glVertexAttribDivisor");
    f.DebugMessageCallback = (GLFuncs::PFNGLDEBUGMESSAGECALLBACKPROC)getProc("glDebugMessageCallback");
    f.DebugMessageControl = (GLFuncs::PFNGLDEBUGMESSAGECONTROLPROC)getProc("glDebugMessageControl");

    // 关键入口缺失即视为加载失败：后续所有绘制都会静默无效，
    // 早失败比在渲染期空指针崩溃好。
    if (!f.GenBuffers || !f.BindVertexArray || !f.UseProgram)
    {
        return false;
    }

    return true;
}

extern "C" bool gl_loader_init(void* getProcAddress)
{
    return gl_loader_load(&g_funcs, getProcAddress);
}