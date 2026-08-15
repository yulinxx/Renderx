#include "gl_loader.h"

#include <cstring>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__linux__)
    #include <GL/glx.h>
#elif defined(__APPLE__)
    #include <dlfcn.h>
#endif

static GLFuncs g_funcs;

extern "C" RENDER_API GLFuncs* gl()
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

extern "C" RENDER_API bool gl_loader_init(void* getProcAddress)
{
    typedef void* (*GetProcAddrFunc)(const char*);
    GetProcAddrFunc getProc =
        getProcAddress ? reinterpret_cast<GetProcAddrFunc>(getProcAddress) : default_get_proc_address;

    memset(&g_funcs, 0, sizeof(g_funcs));

    g_funcs.GenBuffers = (PFNGLGENBUFFERSPROC)getProc("glGenBuffers");
    g_funcs.DeleteBuffers = (PFNGLDELETEBUFFERSPROC)getProc("glDeleteBuffers");
    g_funcs.BindBuffer = (PFNGLBINDBUFFERPROC)getProc("glBindBuffer");
    g_funcs.BufferData = (PFNGLBUFFERDATAPROC)getProc("glBufferData");
    g_funcs.BufferSubData = (PFNGLBUFFERSUBDATAPROC)getProc("glBufferSubData");
    g_funcs.BufferStorage = (PFNGLBUFFERSTORAGEPROC)getProc("glBufferStorage");
    g_funcs.MapBufferRange = (PFNGLMAPBUFFERRANGEPROC)getProc("glMapBufferRange");
    g_funcs.UnmapBuffer = (PFNGLUNMAPBUFFERPROC)getProc("glUnmapBuffer");
    g_funcs.FlushMappedBufferRange = (PFNGLFLUSHMAPPEDBUFFERRANGEPROC)getProc("glFlushMappedBufferRange");
    g_funcs.InvalidateBufferData = (PFNGLINVALIDATEBUFFERDATAPROC)getProc("glInvalidateBufferData");
    g_funcs.CopyBufferSubData = (PFNGLCOPYBUFFERSUBDATAPROC)getProc("glCopyBufferSubData");

    g_funcs.GenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)getProc("glGenVertexArrays");
    g_funcs.DeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)getProc("glDeleteVertexArrays");
    g_funcs.BindVertexArray = (PFNGLBINDVERTEXARRAYPROC)getProc("glBindVertexArray");
    g_funcs.EnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)getProc("glEnableVertexAttribArray");
    g_funcs.VertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)getProc("glVertexAttribPointer");
    g_funcs.VertexAttribBinding = (PFNGLVERTEXATTRIBBINDINGPROC)getProc("glVertexAttribBinding");
    g_funcs.VertexAttribFormat = (PFNGLVERTEXATTRIBFORMATPROC)getProc("glVertexAttribFormat");
    g_funcs.BindVertexBuffer = (PFNGLBINDVERTEXBUFFERPROC)getProc("glBindVertexBuffer");
    g_funcs.VertexBindingDivisor = (PFNGLVERTEXBINDINGDIVISORPROC)getProc("glVertexBindingDivisor");

    g_funcs.DrawArrays = (PFNGLDRAWARRAYSPROC)getProc("glDrawArrays");
    g_funcs.DrawElements = (PFNGLDRAWELEMENTSPROC)getProc("glDrawElements");
    g_funcs.DrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)getProc("glDrawArraysInstanced");
    g_funcs.DrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)getProc("glDrawElementsInstanced");
    g_funcs.DrawArraysIndirect = (PFNGLDRAWARRAYSINDIRECTPROC)getProc("glDrawArraysIndirect");
    g_funcs.DrawElementsIndirect = (PFNGLDRAWELEMENTSINDIRECTPROC)getProc("glDrawElementsIndirect");
    g_funcs.MultiDrawArraysIndirect = (PFNGLMULTIDRAWARRAYSINDIRECTPROC)getProc("glMultiDrawArraysIndirect");
    g_funcs.MultiDrawElementsIndirect = (PFNGLMULTIDRAWELEMENTSINDIRECTPROC)getProc("glMultiDrawElementsIndirect");

    g_funcs.DispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)getProc("glDispatchCompute");
    g_funcs.MemoryBarrier = (PFNGLMEMORYBARRIERPROC)getProc("glMemoryBarrier");
    g_funcs.ShaderBinary = (PFNGLSHADERBINARYPROC)getProc("glShaderBinary");
    g_funcs.SpecializeShader = (PFNGLSPECIALIZESHADERPROC)getProc("glSpecializeShader");
    g_funcs.NamedBufferStorage = (PFNGLNAMEDBUFFERSTORAGEPROC)getProc("glNamedBufferStorage");

    g_funcs.CreateShader = (PFNGLCREATESHADERPROC)getProc("glCreateShader");
    g_funcs.DeleteShader = (PFNGLDELETESHADERPROC)getProc("glDeleteShader");
    g_funcs.ShaderSource = (PFNGLSHADERSOURCEPROC)getProc("glShaderSource");
    g_funcs.CompileShader = (PFNGLCOMPILESHADERPROC)getProc("glCompileShader");
    g_funcs.CreateProgram = (PFNGLCREATEPROGRAMPROC)getProc("glCreateProgram");
    g_funcs.DeleteProgram = (PFNGLDELETEPROGRAMPROC)getProc("glDeleteProgram");
    g_funcs.AttachShader = (PFNGLATTACHSHADERPROC)getProc("glAttachShader");
    g_funcs.LinkProgram = (PFNGLLINKPROGRAMPROC)getProc("glLinkProgram");
    g_funcs.UseProgram = (PFNGLUSEPROGRAMPROC)getProc("glUseProgram");
    g_funcs.GetShaderiv = (PFNGLGETSHADERIVPROC)getProc("glGetShaderiv");
    g_funcs.GetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)getProc("glGetShaderInfoLog");
    g_funcs.GetProgramiv = (PFNGLGETPROGRAMIVPROC)getProc("glGetProgramiv");
    g_funcs.GetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)getProc("glGetProgramInfoLog");
    g_funcs.GetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)getProc("glGetUniformLocation");
    g_funcs.Uniform1f = (PFNGLUNIFORM1FPROC)getProc("glUniform1f");
    g_funcs.Uniform2f = (PFNGLUNIFORM2FPROC)getProc("glUniform2f");
    g_funcs.Uniform3f = (PFNGLUNIFORM3FPROC)getProc("glUniform3f");
    g_funcs.Uniform4f = (PFNGLUNIFORM4FPROC)getProc("glUniform4f");
    g_funcs.Uniform1i = (PFNGLUNIFORM1IPROC)getProc("glUniform1i");
    g_funcs.UniformMatrix3fv = (PFNGLUNIFORMMATRIX3FVPROC)getProc("glUniformMatrix3fv");
    g_funcs.UniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)getProc("glUniformMatrix4fv");
    g_funcs.Uniform1ui = (PFNGLUNIFORM1UIPROC)getProc("glUniform1ui");
    g_funcs.Uniform2ui = (PFNGLUNIFORM2UIPROC)getProc("glUniform2ui");
    g_funcs.Uniform1fv = (PFNGLUNIFORM1FVPROC)getProc("glUniform1fv");
    g_funcs.Uniform2fv = (PFNGLUNIFORM2FVPROC)getProc("glUniform2fv");
    g_funcs.Uniform3fv = (PFNGLUNIFORM3FVPROC)getProc("glUniform3fv");
    g_funcs.Uniform4fv = (PFNGLUNIFORM4FVPROC)getProc("glUniform4fv");

    g_funcs.GenTextures = (PFNGLGENTEXTURESPROC)getProc("glGenTextures");
    g_funcs.DeleteTextures = (PFNGLDELETETEXTURESPROC)getProc("glDeleteTextures");
    g_funcs.BindTexture = (PFNGLBINDTEXTUREPROC)getProc("glBindTexture");
    g_funcs.TexImage2D = (PFNGLTEXIMAGE2DPROC)getProc("glTexImage2D");
    g_funcs.TexSubImage2D = (PFNGLTEXSUBIMAGE2DPROC)getProc("glTexSubImage2D");
    g_funcs.TexParameteri = (PFNGLTEXPARAMETERIPROC)getProc("glTexParameteri");
    g_funcs.ActiveTexture = (PFNGLACTIVETEXTUREPROC)getProc("glActiveTexture");

    g_funcs.Enable = (PFNGLENABLEPROC)getProc("glEnable");
    g_funcs.Disable = (PFNGLDISABLEPROC)getProc("glDisable");
    g_funcs.BlendFunc = (PFNGLBLENDFUNCPROC)getProc("glBlendFunc");
    g_funcs.LineWidth = (PFNGLLINEWIDTHPROC)getProc("glLineWidth");
    g_funcs.PointSize = (PFNGLPOINTSIZEPROC)getProc("glPointSize");
    g_funcs.Viewport = (PFNGLVIEWPORTPROC)getProc("glViewport");
    g_funcs.Scissor = (PFNGLSCISSORPROC)getProc("glScissor");
    g_funcs.ClearColor = (PFNGLCLEARCOLORPROC)getProc("glClearColor");
    g_funcs.Clear = (PFNGLCLEARPROC)getProc("glClear");
    g_funcs.ReadPixels = (PFNGLREADPIXELSPROC)getProc("glReadPixels");
    g_funcs.ColorMask = (PFNGLCOLORMASKPROC)getProc("glColorMask");
    g_funcs.DepthMask = (PFNGLDEPTHMASKPROC)getProc("glDepthMask");
    g_funcs.DepthFunc = (PFNGLDEPTHFUNCPROC)getProc("glDepthFunc");
    g_funcs.PolygonMode = (PFNGLPOLYGONMODEPROC)getProc("glPolygonMode");
    g_funcs.GetIntegerv = (PFNGLGETINTEGERVPROC)getProc("glGetIntegerv");
    g_funcs.GetFloatv = (PFNGLGETFLOATVPROC)getProc("glGetFloatv");
    g_funcs.Hint = (PFNGLHINTPROC)getProc("glHint");

    g_funcs.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)getProc("glGenFramebuffers");
    g_funcs.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)getProc("glDeleteFramebuffers");
    g_funcs.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)getProc("glBindFramebuffer");
    g_funcs.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)getProc("glFramebufferTexture2D");

    g_funcs.CreateBuffers = (PFNGLCREATEBUFFERSPROC)getProc("glCreateBuffers");
    g_funcs.NamedBufferData = (PFNGLNAMEDBUFFERDATAPROC)getProc("glNamedBufferData");
    g_funcs.NamedBufferSubData = (PFNGLNAMEDBUFFERSUBDATAPROC)getProc("glNamedBufferSubData");
    g_funcs.MapNamedBufferRange = (PFNGLMAPNAMEDBUFFERRANGEPROC)getProc("glMapNamedBufferRange");
    g_funcs.UnmapNamedBuffer = (PFNGLUNMAPNAMEDBUFFERPROC)getProc("glUnmapNamedBuffer");
    g_funcs.FlushMappedNamedBufferRange =
        (PFNGLFLUSHMAPPEDNAMEDBUFFERRANGEPROC)getProc("glFlushMappedNamedBufferRange");
    g_funcs.CopyNamedBufferSubData = (PFNGLCOPYNAMEDBUFFERSUBDATAPROC)getProc("glCopyNamedBufferSubData");

    g_funcs.Flush = (PFNGLFLUSHPROC)getProc("glFlush");
    g_funcs.BindBufferRange = (PFNGLBINDBUFFERRANGEPROC)getProc("glBindBufferRange");

    g_funcs.CreateTextures = (PFNGLCREATETEXTURESPROC)getProc("glCreateTextures");
    g_funcs.TextureParameteri = (PFNGLTEXTUREPARAMETERIPROC)getProc("glTextureParameteri");
    g_funcs.TextureStorage2D = (PFNGLTEXTURESTORAGE2DPROC)getProc("glTextureStorage2D");
    g_funcs.TextureSubImage2D = (PFNGLTEXTURESUBIMAGE2DPROC)getProc("glTextureSubImage2D");
    g_funcs.BindTextureUnit = (PFNGLBINDTEXTUREUNITPROC)getProc("glBindTextureUnit");
    g_funcs.CreateVertexArrays = (PFNGLCREATEVERTEXARRAYSPROC)getProc("glCreateVertexArrays");
    g_funcs.EnableVertexArrayAttrib = (PFNGLENABLEVERTEXARRAYATTRIBPROC)getProc("glEnableVertexArrayAttrib");
    g_funcs.VertexArrayAttribBinding = (PFNGLVERTEXARRAYATTRIBBINDINGPROC)getProc("glVertexArrayAttribBinding");
    g_funcs.VertexArrayAttribFormat = (PFNGLVERTEXARRAYATTRIBFORMATPROC)getProc("glVertexArrayAttribFormat");
    g_funcs.VertexArrayVertexBuffer = (PFNGLVERTEXARRAYVERTEXBUFFERPROC)getProc("glVertexArrayVertexBuffer");
    g_funcs.VertexArrayElementBuffer = (PFNGLVERTEXARRAYELEMENTBUFFERPROC)getProc("glVertexArrayElementBuffer");

    g_funcs.FenceSync = (PFNGLFENCESYNCPROC)getProc("glFenceSync");
    g_funcs.ClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)getProc("glClientWaitSync");
    g_funcs.DeleteSync = (PFNGLDELETESYNCPROC)getProc("glDeleteSync");

    g_funcs.GetError = (PFNGLGETERRORPROC)getProc("glGetError");
    g_funcs.GetString = (PFNGLGETSTRINGPROC)getProc("glGetString");
    g_funcs.GetStringi = (PFNGLGETSTRINGIPROC)getProc("glGetStringi");
    g_funcs.GetBooleanv = (PFNGLGETBOOLEANVPROC)getProc("glGetBooleanv");

    if (!g_funcs.GenBuffers || !g_funcs.BindVertexArray || !g_funcs.UseProgram)
    {
        return false;
    }

    return true;
}