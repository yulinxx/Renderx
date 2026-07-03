#pragma once

// 根据平台选择 OpenGL 版本
#if defined(__APPLE__)
#define USE_OPENGL_4_1 1
#elif defined(_WIN32) || defined(__linux__)
#define USE_OPENGL_4_5 1
#else
    // 其他平台默认使用 OpenGL 3.3
#define USE_OPENGL_3_3 1
#endif

#if defined(USE_OPENGL_4_6)

#include <QOpenGLFunctions_4_5_Core>
using XGLFunctions = QOpenGLFunctions_4_5_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 6;
#define GLSL_VERSION_STR "#version 460 core"

#elif defined(USE_OPENGL_4_5)

#include <QOpenGLFunctions_4_5_Core>
using XGLFunctions = QOpenGLFunctions_4_5_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 5;
#define GLSL_VERSION_STR "#version 450 core"

#elif defined(USE_OPENGL_4_4)

#include <QOpenGLFunctions_4_4_Core>
using XGLFunctions = QOpenGLFunctions_4_4_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 4;
#define GLSL_VERSION_STR "#version 440 core"

#elif defined(USE_OPENGL_4_3)

#include <QOpenGLFunctions_4_3_Core>
using XGLFunctions = QOpenGLFunctions_4_3_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 3;
#define GLSL_VERSION_STR "#version 430 core"

#elif defined(USE_OPENGL_4_2)

#include <QOpenGLFunctions_4_2_Core>
using XGLFunctions = QOpenGLFunctions_4_2_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 2;
#define GLSL_VERSION_STR "#version 420 core"

#elif defined(USE_OPENGL_4_1)

#include <QOpenGLFunctions_4_1_Core>
using XGLFunctions = QOpenGLFunctions_4_1_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 1;
#define GLSL_VERSION_STR "#version 410 core"

#elif defined(USE_OPENGL_4_0)

#include <QOpenGLFunctions_4_0_Core>
using XGLFunctions = QOpenGLFunctions_4_0_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 4;
constexpr int TARGET_GL_VERSION_MINOR = 0;
#define GLSL_VERSION_STR "#version 400"

#else

#include <QOpenGLFunctions_3_3_Core>
using XGLFunctions = QOpenGLFunctions_3_3_Core;
constexpr int TARGET_GL_VERSION_MAJOR = 3;
constexpr int TARGET_GL_VERSION_MINOR = 3;
#define GLSL_VERSION_STR "#version 330 core"

#endif
