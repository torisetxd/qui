#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define QUI_GLAPI __stdcall
#else
#define QUI_GLAPI
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLenum GL_FALSE = 0;
inline constexpr GLenum GL_TRUE = 1;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_ACTIVE_UNIFORMS = 0x8B86;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;

using PFNGLACTIVETEXTUREPROC = void(QUI_GLAPI *)(GLenum);
using PFNGLATTACHSHADERPROC = void(QUI_GLAPI *)(GLuint, GLuint);
using PFNGLBINDBUFFERPROC = void(QUI_GLAPI *)(GLenum, GLuint);
using PFNGLBINDFRAMEBUFFERPROC = void(QUI_GLAPI *)(GLenum, GLuint);
using PFNGLBINDRENDERBUFFERPROC = void(QUI_GLAPI *)(GLenum, GLuint);
using PFNGLBINDTEXTUREPROC = void(QUI_GLAPI *)(GLenum, GLuint);
using PFNGLBINDVERTEXARRAYPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLBLENDFUNCPROC = void(QUI_GLAPI *)(GLenum, GLenum);
using PFNGLBUFFERDATAPROC = void(QUI_GLAPI *)(GLenum, GLsizeiptr, const void *, GLenum);
using PFNGLBUFFERSUBDATAPROC = void(QUI_GLAPI *)(GLenum, GLsizeiptr, GLsizeiptr, const void *);
using PFNGLCLEARPROC = void(QUI_GLAPI *)(GLbitfield);
using PFNGLCLEARCOLORPROC = void(QUI_GLAPI *)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFNGLCOMPILESHADERPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLCREATEPROGRAMPROC = GLuint(QUI_GLAPI *)();
using PFNGLCREATESHADERPROC = GLuint(QUI_GLAPI *)(GLenum);
using PFNGLDELETEPROGRAMPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLDELETESHADERPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLDISABLEPROC = void(QUI_GLAPI *)(GLenum);
using PFNGLDRAWARRAYSPROC = void(QUI_GLAPI *)(GLenum, GLint, GLsizei);
using PFNGLDRAWARRAYSINSTANCEDPROC = void(QUI_GLAPI *)(GLenum, GLint, GLsizei, GLsizei);
using PFNGLENABLEPROC = void(QUI_GLAPI *)(GLenum);
using PFNGLENABLEVERTEXATTRIBARRAYPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLFRAMEBUFFERRENDERBUFFERPROC = void(QUI_GLAPI *)(GLenum, GLenum, GLenum, GLuint);
using PFNGLGENBUFFERSPROC = void(QUI_GLAPI *)(GLsizei, GLuint *);
using PFNGLGENFRAMEBUFFERSPROC = void(QUI_GLAPI *)(GLsizei, GLuint *);
using PFNGLGENRENDERBUFFERSPROC = void(QUI_GLAPI *)(GLsizei, GLuint *);
using PFNGLGENTEXTURESPROC = void(QUI_GLAPI *)(GLsizei, GLuint *);
using PFNGLGENVERTEXARRAYSPROC = void(QUI_GLAPI *)(GLsizei, GLuint *);
using PFNGLGETACTIVEUNIFORMPROC = void(QUI_GLAPI *)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
using PFNGLGETPROGRAMINFOLOGPROC = void(QUI_GLAPI *)(GLuint, GLsizei, GLsizei *, GLchar *);
using PFNGLGETPROGRAMIVPROC = void(QUI_GLAPI *)(GLuint, GLenum, GLint *);
using PFNGLGETSHADERINFOLOGPROC = void(QUI_GLAPI *)(GLuint, GLsizei, GLsizei *, GLchar *);
using PFNGLGETSHADERIVPROC = void(QUI_GLAPI *)(GLuint, GLenum, GLint *);
using PFNGLGETUNIFORMLOCATIONPROC = GLint(QUI_GLAPI *)(GLuint, const GLchar *);
using PFNGLLINKPROGRAMPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLPIXELSTOREIPROC = void(QUI_GLAPI *)(GLenum, GLint);
using PFNGLREADPIXELSPROC = void(QUI_GLAPI *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
using PFNGLRENDERBUFFERSTORAGEPROC = void(QUI_GLAPI *)(GLenum, GLenum, GLsizei, GLsizei);
using PFNGLSCISSORPROC = void(QUI_GLAPI *)(GLint, GLint, GLsizei, GLsizei);
using PFNGLSHADERSOURCEPROC = void(QUI_GLAPI *)(GLuint, GLsizei, const GLchar *const *, const GLint *);
using PFNGLTEXIMAGE2DPROC = void(QUI_GLAPI *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using PFNGLTEXPARAMETERIPROC = void(QUI_GLAPI *)(GLenum, GLenum, GLint);
using PFNGLTEXSUBIMAGE2DPROC = void(QUI_GLAPI *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
using PFNGLUNIFORM1FPROC = void(QUI_GLAPI *)(GLint, GLfloat);
using PFNGLUNIFORM1IPROC = void(QUI_GLAPI *)(GLint, GLint);
using PFNGLUNIFORM2FPROC = void(QUI_GLAPI *)(GLint, GLfloat, GLfloat);
using PFNGLUNIFORM3FPROC = void(QUI_GLAPI *)(GLint, GLfloat, GLfloat, GLfloat);
using PFNGLUNIFORM4FPROC = void(QUI_GLAPI *)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFNGLUSEPROGRAMPROC = void(QUI_GLAPI *)(GLuint);
using PFNGLVERTEXATTRIBDIVISORPROC = void(QUI_GLAPI *)(GLuint, GLuint);
using PFNGLVERTEXATTRIBIPOINTERPROC = void(QUI_GLAPI *)(GLuint, GLint, GLenum, GLsizei, const void *);
using PFNGLVERTEXATTRIBPOINTERPROC = void(QUI_GLAPI *)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
using PFNGLVIEWPORTPROC = void(QUI_GLAPI *)(GLint, GLint, GLsizei, GLsizei);
