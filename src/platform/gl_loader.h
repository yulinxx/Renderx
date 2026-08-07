#pragma once

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <GL/gl.h>
#elif defined(__linux__)
  #include <GL/gl.h>
#elif defined(__APPLE__)
  #include <OpenGL/gl3.h>
#endif

#ifdef _WIN32
  typedef ptrdiff_t GLintptr;
  typedef ptrdiff_t GLsizeiptr;
  typedef char GLchar;
  struct __GLsync;
  typedef struct __GLsync* GLsync;
  typedef unsigned long long GLuint64;
  typedef long long GLint64;
#endif

#ifndef GL_POINTS
#define GL_POINTS 0x0000
#endif
#ifndef GL_LINES
#define GL_LINES 0x0001
#endif
#ifndef GL_LINE_LOOP
#define GL_LINE_LOOP 0x0002
#endif
#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP 0x0003
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x0004
#endif
#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP 0x0005
#endif
#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN 0x0006
#endif

#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif
#ifndef GL_INT
#define GL_INT 0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif

#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x0B71
#endif
#ifndef GL_BLEND
#define GL_BLEND 0x0BE2
#endif
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH 0x0B20
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif
#ifndef GL_LESS
#define GL_LESS 0x0201
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL 0x0203
#endif

#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x0302
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif

#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif

#ifndef GL_FRONT_AND_BACK
#define GL_FRONT_AND_BACK 0x0408
#endif
#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif

#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif

#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_DYNAMIC_STORAGE_BIT
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif

#ifndef GL_BUFFER_IMMUTABLE_STORAGE
#define GL_BUFFER_IMMUTABLE_STORAGE 0x821F
#endif

#ifndef GL_COMMAND_BARRIER_BIT
#define GL_COMMAND_BARRIER_BIT 0x00000040
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_BUFFER_UPDATE_BARRIER_BIT
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#endif
#ifndef GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT
#define GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT 0x00004000
#endif

#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif

#ifdef _WIN32
  #define RENDER_GLAPI __stdcall
#else
  #define RENDER_GLAPI
#endif

typedef void   (RENDER_GLAPI *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void   (RENDER_GLAPI *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint* buffers);
typedef void   (RENDER_GLAPI *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void   (RENDER_GLAPI *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void   (RENDER_GLAPI *PFNGLBUFFERSUBDATAPROC)(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
typedef void   (RENDER_GLAPI *PFNGLBUFFERSTORAGEPROC)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
typedef void*  (RENDER_GLAPI *PFNGLMAPBUFFERRANGEPROC)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
typedef GLboolean (RENDER_GLAPI *PFNGLUNMAPBUFFERPROC)(GLenum target);
typedef void   (RENDER_GLAPI *PFNGLFLUSHMAPPEDBUFFERRANGEPROC)(GLenum target, GLintptr offset, GLsizeiptr length);
typedef void   (RENDER_GLAPI *PFNGLINVALIDATEBUFFERDATAPROC)(GLuint buffer);
typedef void   (RENDER_GLAPI *PFNGLCOPYBUFFERSUBDATAPROC)(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);

typedef void   (RENDER_GLAPI *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void   (RENDER_GLAPI *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void   (RENDER_GLAPI *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void   (RENDER_GLAPI *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void   (RENDER_GLAPI *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void   (RENDER_GLAPI *PFNGLVERTEXATTRIBBINDINGPROC)(GLuint attribindex, GLuint bindingindex);
typedef void   (RENDER_GLAPI *PFNGLVERTEXATTRIBFORMATPROC)(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
typedef void   (RENDER_GLAPI *PFNGLBINDVERTEXBUFFERPROC)(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
typedef void   (RENDER_GLAPI *PFNGLVERTEXBINDINGDIVISORPROC)(GLuint bindingindex, GLuint divisor);

typedef void   (RENDER_GLAPI *PFNGLDRAWARRAYSPROC)(GLenum mode, GLint first, GLsizei count);
typedef void   (RENDER_GLAPI *PFNGLDRAWELEMENTSPROC)(GLenum mode, GLsizei count, GLenum type, const void* indices);
typedef void   (RENDER_GLAPI *PFNGLDRAWARRAYSINSTANCEDPROC)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
typedef void   (RENDER_GLAPI *PFNGLDRAWELEMENTSINSTANCEDPROC)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
typedef void   (RENDER_GLAPI *PFNGLDRAWARRAYSINDIRECTPROC)(GLenum mode, const void* indirect);
typedef void   (RENDER_GLAPI *PFNGLDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void* indirect);
typedef void   (RENDER_GLAPI *PFNGLMULTIDRAWARRAYSINDIRECTPROC)(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
typedef void   (RENDER_GLAPI *PFNGLMULTIDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);

typedef void   (RENDER_GLAPI *PFNGLDISPATCHCOMPUTEPROC)(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
typedef void   (RENDER_GLAPI *PFNGLMEMORYBARRIERPROC)(GLbitfield barriers);
typedef void   (RENDER_GLAPI *PFNGLSHADERBINARYPROC)(GLsizei count, const GLuint* shaders, GLenum binaryformat, const void* binary, GLsizei length);
typedef void   (RENDER_GLAPI *PFNGLSPECIALIZESHADERPROC)(GLuint shader, const GLchar* pEntryPoint, GLuint numSpecializationConstants, const GLuint* pConstantIndex, const GLuint* pConstantValue);
typedef void   (RENDER_GLAPI *PFNGLNAMEDBUFFERSTORAGEPROC)(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags);

typedef GLuint (RENDER_GLAPI *PFNGLCREATESHADERPROC)(GLenum type);
typedef void   (RENDER_GLAPI *PFNGLDELETESHADERPROC)(GLuint shader);
typedef void   (RENDER_GLAPI *PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void   (RENDER_GLAPI *PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef GLuint (RENDER_GLAPI *PFNGLCREATEPROGRAMPROC)(void);
typedef void   (RENDER_GLAPI *PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef void   (RENDER_GLAPI *PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void   (RENDER_GLAPI *PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void   (RENDER_GLAPI *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void   (RENDER_GLAPI *PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint* params);
typedef void   (RENDER_GLAPI *PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void   (RENDER_GLAPI *PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint* params);
typedef void   (RENDER_GLAPI *PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef GLint  (RENDER_GLAPI *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar* name);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM1FPROC)(GLint location, GLfloat v0);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM2FPROC)(GLint location, GLfloat v0, GLfloat v1);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM3FPROC)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM4FPROC)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void   (RENDER_GLAPI *PFNGLUNIFORMMATRIX3FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void   (RENDER_GLAPI *PFNGLUNIFORMMATRIX4FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM1UIPROC)(GLint location, GLuint v0);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM2UIPROC)(GLint location, GLuint v0, GLuint v1);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM1FVPROC)(GLint location, GLsizei count, const GLfloat* value);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM2FVPROC)(GLint location, GLsizei count, const GLfloat* value);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM3FVPROC)(GLint location, GLsizei count, const GLfloat* value);
typedef void   (RENDER_GLAPI *PFNGLUNIFORM4FVPROC)(GLint location, GLsizei count, const GLfloat* value);

typedef void   (RENDER_GLAPI *PFNGLGENTEXTURESPROC)(GLsizei n, GLuint* textures);
typedef void   (RENDER_GLAPI *PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void   (RENDER_GLAPI *PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void   (RENDER_GLAPI *PFNGLTEXIMAGE2DPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
typedef void   (RENDER_GLAPI *PFNGLTEXSUBIMAGE2DPROC)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
typedef void   (RENDER_GLAPI *PFNGLTEXPARAMETERIPROC)(GLenum target, GLenum pname, GLint param);
typedef void   (RENDER_GLAPI *PFNGLACTIVETEXTUREPROC)(GLenum texture);

typedef void   (RENDER_GLAPI *PFNGLENABLEPROC)(GLenum cap);
typedef void   (RENDER_GLAPI *PFNGLDISABLEPROC)(GLenum cap);
typedef void   (RENDER_GLAPI *PFNGLBLENDFUNCPROC)(GLenum sfactor, GLenum dfactor);
typedef void   (RENDER_GLAPI *PFNGLLINEWIDTHPROC)(GLfloat width);
typedef void   (RENDER_GLAPI *PFNGLPOINTSIZEPROC)(GLfloat size);
typedef void   (RENDER_GLAPI *PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void   (RENDER_GLAPI *PFNGLSCISSORPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void   (RENDER_GLAPI *PFNGLCLEARCOLORPROC)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
typedef void   (RENDER_GLAPI *PFNGLCLEARPROC)(GLbitfield mask);
typedef void   (RENDER_GLAPI *PFNGLCOLORMASKPROC)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
typedef void   (RENDER_GLAPI *PFNGLDEPTHMASKPROC)(GLboolean flag);
typedef void   (RENDER_GLAPI *PFNGLDEPTHFUNCPROC)(GLenum func);
typedef void   (RENDER_GLAPI *PFNGLPOLYGONMODEPROC)(GLenum face, GLenum mode);
typedef void   (RENDER_GLAPI *PFNGLGETINTEGERVPROC)(GLenum pname, GLint* data);

typedef void   (RENDER_GLAPI *PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void   (RENDER_GLAPI *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void   (RENDER_GLAPI *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void   (RENDER_GLAPI *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);

typedef void   (RENDER_GLAPI *PFNGLCREATEBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void   (RENDER_GLAPI *PFNGLNAMEDBUFFERDATAPROC)(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage);
typedef void   (RENDER_GLAPI *PFNGLNAMEDBUFFERSUBDATAPROC)(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
typedef void*  (RENDER_GLAPI *PFNGLMAPNAMEDBUFFERRANGEPROC)(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
typedef GLboolean (RENDER_GLAPI *PFNGLUNMAPNAMEDBUFFERPROC)(GLuint buffer);
typedef void   (RENDER_GLAPI *PFNGLFLUSHMAPPEDNAMEDBUFFERRANGEPROC)(GLuint buffer, GLintptr offset, GLsizeiptr length);
typedef void   (RENDER_GLAPI *PFNGLCOPYNAMEDBUFFERSUBDATAPROC)(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
typedef void   (RENDER_GLAPI *PFNGLCREATETEXTURESPROC)(GLenum target, GLsizei n, GLuint* textures);
typedef void   (RENDER_GLAPI *PFNGLTEXTUREPARAMETERIPROC)(GLuint texture, GLenum pname, GLint param);
typedef void   (RENDER_GLAPI *PFNGLTEXTURESTORAGE2DPROC)(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
typedef void   (RENDER_GLAPI *PFNGLTEXTURESUBIMAGE2DPROC)(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
typedef void   (RENDER_GLAPI *PFNGLBINDTEXTUREUNITPROC)(GLuint unit, GLuint texture);
typedef void   (RENDER_GLAPI *PFNGLCREATEVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void   (RENDER_GLAPI *PFNGLENABLEVERTEXARRAYATTRIBPROC)(GLuint vaobj, GLuint index);
typedef void   (RENDER_GLAPI *PFNGLVERTEXARRAYATTRIBBINDINGPROC)(GLuint vaobj, GLuint attribindex, GLuint bindingindex);
typedef void   (RENDER_GLAPI *PFNGLVERTEXARRAYATTRIBFORMATPROC)(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
typedef void   (RENDER_GLAPI *PFNGLVERTEXARRAYVERTEXBUFFERPROC)(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
typedef void   (RENDER_GLAPI *PFNGLVERTEXARRAYELEMENTBUFFERPROC)(GLuint vaobj, GLuint buffer);

typedef GLsync (RENDER_GLAPI *PFNGLFENCESYNCPROC)(GLenum condition, GLbitfield flags);
typedef GLenum (RENDER_GLAPI *PFNGLCLIENTWAITSYNCPROC)(GLsync sync, GLbitfield flags, GLuint64 timeout);
typedef void   (RENDER_GLAPI *PFNGLDELETESYNCPROC)(GLsync sync);

typedef void   (RENDER_GLAPI *PFNGLFLUSHPROC)(void);
typedef void   (RENDER_GLAPI *PFNGLBINDBUFFERRANGEPROC)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);

typedef GLenum         (RENDER_GLAPI *PFNGLGETERRORPROC)(void);
typedef const GLubyte* (RENDER_GLAPI *PFNGLGETSTRINGPROC)(GLenum name);
typedef const GLubyte* (RENDER_GLAPI *PFNGLGETSTRINGIPROC)(GLenum name, GLuint index);
typedef void           (RENDER_GLAPI *PFNGLGETBOOLEANVPROC)(GLenum pname, GLboolean* data);

#ifndef RENDER_API
  #ifdef _WIN32
    #ifdef RENDER_EXPORTS
      #define RENDER_API __declspec(dllexport)
    #else
      #define RENDER_API __declspec(dllimport)
    #endif
  #else
    #define RENDER_API __attribute__((visibility("default")))
  #endif
#endif

struct GLFuncs {
    PFNGLGENBUFFERSPROC              GenBuffers;
    PFNGLDELETEBUFFERSPROC           DeleteBuffers;
    PFNGLBINDBUFFERPROC              BindBuffer;
    PFNGLBUFFERDATAPROC              BufferData;
    PFNGLBUFFERSUBDATAPROC           BufferSubData;
    PFNGLBUFFERSTORAGEPROC           BufferStorage;
    PFNGLMAPBUFFERRANGEPROC          MapBufferRange;
    PFNGLUNMAPBUFFERPROC             UnmapBuffer;
    PFNGLFLUSHMAPPEDBUFFERRANGEPROC  FlushMappedBufferRange;
    PFNGLINVALIDATEBUFFERDATAPROC    InvalidateBufferData;
    PFNGLCOPYBUFFERSUBDATAPROC       CopyBufferSubData;

    PFNGLGENVERTEXARRAYSPROC         GenVertexArrays;
    PFNGLDELETEVERTEXARRAYSPROC      DeleteVertexArrays;
    PFNGLBINDVERTEXARRAYPROC         BindVertexArray;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC     VertexAttribPointer;
    PFNGLVERTEXATTRIBBINDINGPROC     VertexAttribBinding;
    PFNGLVERTEXATTRIBFORMATPROC      VertexAttribFormat;
    PFNGLBINDVERTEXBUFFERPROC        BindVertexBuffer;
    PFNGLVERTEXBINDINGDIVISORPROC    VertexBindingDivisor;

    PFNGLDRAWARRAYSPROC              DrawArrays;
    PFNGLDRAWELEMENTSPROC            DrawElements;
    PFNGLDRAWARRAYSINSTANCEDPROC     DrawArraysInstanced;
    PFNGLDRAWELEMENTSINSTANCEDPROC   DrawElementsInstanced;
    PFNGLDRAWARRAYSINDIRECTPROC      DrawArraysIndirect;
    PFNGLDRAWELEMENTSINDIRECTPROC    DrawElementsIndirect;
    PFNGLMULTIDRAWARRAYSINDIRECTPROC  MultiDrawArraysIndirect;
    PFNGLMULTIDRAWELEMENTSINDIRECTPROC MultiDrawElementsIndirect;

    PFNGLDISPATCHCOMPUTEPROC         DispatchCompute;
    PFNGLMEMORYBARRIERPROC           MemoryBarrier;
    PFNGLSHADERBINARYPROC            ShaderBinary;
    PFNGLSPECIALIZESHADERPROC        SpecializeShader;
    PFNGLNAMEDBUFFERSTORAGEPROC      NamedBufferStorage;

    PFNGLCREATESHADERPROC            CreateShader;
    PFNGLDELETESHADERPROC            DeleteShader;
    PFNGLSHADERSOURCEPROC            ShaderSource;
    PFNGLCOMPILESHADERPROC           CompileShader;
    PFNGLCREATEPROGRAMPROC           CreateProgram;
    PFNGLDELETEPROGRAMPROC           DeleteProgram;
    PFNGLATTACHSHADERPROC            AttachShader;
    PFNGLLINKPROGRAMPROC             LinkProgram;
    PFNGLUSEPROGRAMPROC              UseProgram;
    PFNGLGETSHADERIVPROC             GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC        GetShaderInfoLog;
    PFNGLGETPROGRAMIVPROC            GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC       GetProgramInfoLog;
    PFNGLGETUNIFORMLOCATIONPROC      GetUniformLocation;
    PFNGLUNIFORM1FPROC               Uniform1f;
    PFNGLUNIFORM2FPROC               Uniform2f;
    PFNGLUNIFORM3FPROC               Uniform3f;
    PFNGLUNIFORM4FPROC               Uniform4f;
    PFNGLUNIFORM1IPROC               Uniform1i;
    PFNGLUNIFORMMATRIX3FVPROC        UniformMatrix3fv;
    PFNGLUNIFORMMATRIX4FVPROC        UniformMatrix4fv;
    PFNGLUNIFORM1UIPROC              Uniform1ui;
    PFNGLUNIFORM2UIPROC              Uniform2ui;
    PFNGLUNIFORM1FVPROC              Uniform1fv;
    PFNGLUNIFORM2FVPROC              Uniform2fv;
    PFNGLUNIFORM3FVPROC              Uniform3fv;
    PFNGLUNIFORM4FVPROC              Uniform4fv;

    PFNGLGENTEXTURESPROC             GenTextures;
    PFNGLDELETETEXTURESPROC          DeleteTextures;
    PFNGLBINDTEXTUREPROC             BindTexture;
    PFNGLTEXIMAGE2DPROC              TexImage2D;
    PFNGLTEXSUBIMAGE2DPROC           TexSubImage2D;
    PFNGLTEXPARAMETERIPROC           TexParameteri;
    PFNGLACTIVETEXTUREPROC           ActiveTexture;

    PFNGLENABLEPROC                  Enable;
    PFNGLDISABLEPROC                 Disable;
    PFNGLBLENDFUNCPROC               BlendFunc;
    PFNGLLINEWIDTHPROC               LineWidth;
    PFNGLPOINTSIZEPROC               PointSize;
    PFNGLVIEWPORTPROC                Viewport;
    PFNGLSCISSORPROC                 Scissor;
    PFNGLCLEARCOLORPROC              ClearColor;
    PFNGLCLEARPROC                   Clear;
    PFNGLCOLORMASKPROC               ColorMask;
    PFNGLDEPTHMASKPROC               DepthMask;
    PFNGLDEPTHFUNCPROC               DepthFunc;
    PFNGLPOLYGONMODEPROC             PolygonMode;
    PFNGLGETINTEGERVPROC             GetIntegerv;

    PFNGLGENFRAMEBUFFERSPROC         GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC      DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC         BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC    FramebufferTexture2D;

    PFNGLCREATEBUFFERSPROC                CreateBuffers;
    PFNGLNAMEDBUFFERDATAPROC              NamedBufferData;
    PFNGLNAMEDBUFFERSUBDATAPROC           NamedBufferSubData;
    PFNGLMAPNAMEDBUFFERRANGEPROC          MapNamedBufferRange;
    PFNGLUNMAPNAMEDBUFFERPROC             UnmapNamedBuffer;
    PFNGLFLUSHMAPPEDNAMEDBUFFERRANGEPROC  FlushMappedNamedBufferRange;
    PFNGLCOPYNAMEDBUFFERSUBDATAPROC       CopyNamedBufferSubData;

    typedef void   (RENDER_GLAPI *PFNGLFLUSHPROC)(void);
    PFNGLFLUSHPROC Flush;

    typedef void   (RENDER_GLAPI *PFNGLBINDBUFFERRANGEPROC)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    PFNGLBINDBUFFERRANGEPROC BindBufferRange;

    PFNGLCREATETEXTURESPROC               CreateTextures;
    PFNGLTEXTUREPARAMETERIPROC            TextureParameteri;
    PFNGLTEXTURESTORAGE2DPROC             TextureStorage2D;
    PFNGLTEXTURESUBIMAGE2DPROC            TextureSubImage2D;
    PFNGLBINDTEXTUREUNITPROC              BindTextureUnit;
    PFNGLCREATEVERTEXARRAYSPROC           CreateVertexArrays;
    PFNGLENABLEVERTEXARRAYATTRIBPROC      EnableVertexArrayAttrib;
    PFNGLVERTEXARRAYATTRIBBINDINGPROC     VertexArrayAttribBinding;
    PFNGLVERTEXARRAYATTRIBFORMATPROC      VertexArrayAttribFormat;
    PFNGLVERTEXARRAYVERTEXBUFFERPROC      VertexArrayVertexBuffer;
    PFNGLVERTEXARRAYELEMENTBUFFERPROC     VertexArrayElementBuffer;

    PFNGLFENCESYNCPROC               FenceSync;
    PFNGLCLIENTWAITSYNCPROC          ClientWaitSync;
    PFNGLDELETESYNCPROC              DeleteSync;

    PFNGLGETERRORPROC                GetError;
    PFNGLGETSTRINGPROC               GetString;
    PFNGLGETSTRINGIPROC              GetStringi;
    PFNGLGETBOOLEANVPROC             GetBooleanv;
};

extern "C" RENDER_API GLFuncs* gl();
extern "C" RENDER_API bool gl_loader_init(void* getProcAddress);
