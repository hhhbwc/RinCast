#pragma once
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
inline void glUseProgram(GLuint) {}
inline GLint glGetUniformLocation(GLuint, const char*) { return -1; }
inline void glDeleteTextures(GLsizei, const GLuint*) {}
