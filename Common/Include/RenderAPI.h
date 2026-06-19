#pragma once

// Windows
#if defined(_WIN32) || defined(_WIN64)
#ifdef RENDER_EXPORTS
#define RENDER_API __declspec(dllexport)
#else
#define RENDER_API __declspec(dllimport)
#endif
// Linux/Unix
#elif defined(__GNUC__) || defined(__clang__)
#ifdef RENDER_EXPORTS
#define RENDER_API __attribute__((visibility("default")))
#else
#define RENDER_API
#endif
// MacOS
#elif defined(__APPLE__)
#ifdef RENDER_EXPORTS
#define RENDER_API __attribute__((visibility("default")))
#else
#define RENDER_API
#endif
#else
#define RENDER_API
#endif
