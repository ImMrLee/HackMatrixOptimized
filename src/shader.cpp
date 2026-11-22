#include "shader.h"
#include <glad/glad.h> // include glad to get the required OpenGL headers
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <optional>
#include <filesystem>

namespace fs = std::filesystem;

#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

std::string
retrieveShaderCode(std::string path)
{
  std::ifstream shaderFile;
  // ensure ifstream objects can throw exceptions:
  shaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
  try {
    shaderFile.open(path.c_str());
    std::stringstream shaderStream;
    shaderStream << shaderFile.rdbuf();
    std::string rv = shaderStream.str();
    shaderFile.close();
    return rv;
  } catch (const std::ifstream::failure& e) {
    spdlog::error("SHADER: file not successfully read: {}", path);
    return "";
  }
}

bool
hasCompileErrors(unsigned int shaderId, std::string shaderName)
{
  int success;
  char infoLog[512];
  glGetShaderiv(shaderId, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(shaderId, 512, NULL, infoLog);
    spdlog::error("SHADER {}: compilation failed ({}): {}", shaderName, shaderId, infoLog);
    return true;
  };
  return false;
}

bool
Shader::createAndCompileShader(GLenum shaderType, const std::string sourceCode)
{
  unsigned int shaderId = glCreateShader(shaderType);
  const std::string::size_type size = sourceCode.size();
  char* src = new char[size + 1]; // we need extra char for NUL
  memcpy(src, sourceCode.c_str(), size + 1);
  glShaderSource(shaderId, 1, &src, NULL);
  glCompileShader(shaderId);
  delete[] src;

  std::string shaderName = "UNKNOWN";
  if (shaderType == GL_FRAGMENT_SHADER) {
    fragment = shaderId;
    shaderName = "FRAGMENT";
  }
  if (shaderType == GL_VERTEX_SHADER) {
    vertex = shaderId;
    shaderName = "VERTEX";
  }
  if (shaderType == GL_GEOMETRY_SHADER) {
    geometry = shaderId;
    shaderName = "GEOMETRY";
  }
  return !hasCompileErrors(shaderId, shaderName);
}

bool
hasLinkingErrors(unsigned int shaderId)
{
  int success;
  char infoLog[2048];
  glGetProgramiv(shaderId, GL_LINK_STATUS, &success);
  if (!success) {
    glGetProgramInfoLog(shaderId, 2048, NULL, infoLog);
    spdlog::error("SHADER program: linking failed ({}): {}", shaderId, infoLog);
    return true;
  }
  return false;
}

void
Shader::linkShaderProgram()
{
  auto newID = glCreateProgram();
  glAttachShader(newID, vertex);
  glAttachShader(newID, fragment);
  if (geometryCode.has_value()) {
    glAttachShader(newID, geometry);
  }
  glLinkProgram(newID);
  if(hasLinkingErrors(newID)) {
    glDeleteProgram(newID);
  } else {
    if(ID != 0) {
      // delete the previous loaded program
      glDeleteProgram(ID);
    }
    ID = newID;
    uniformLocations.clear();
  }
}

static bool
shouldUseGLES()
{
  static std::optional<bool> cached;
  if (!cached.has_value()) {
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* glslVersion =
      reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    bool isES = (glVersion && strstr(glVersion, "OpenGL ES")) ||
                (glslVersion && strstr(glslVersion, "ES"));
    cached = isES;
  }
  return cached.value();
}

static std::string
rewriteToGLES(const std::string& source, bool want320)
{
  std::string out = source;
  auto pos = out.find("#version");
  auto newVersion = want320 ? std::string("#version 320 es")
                            : std::string("#version 300 es");
  if (pos != std::string::npos) {
    auto lineEnd = out.find('\n', pos);
    if (lineEnd == std::string::npos) {
      lineEnd = out.size();
    }
    out.replace(pos, lineEnd - pos, newVersion);
  } else {
    out = newVersion + "\n" + out;
  }

  auto nl = out.find('\n');
  if (nl != std::string::npos) {
    out.insert(nl + 1,
               "precision highp float;\n"
               "precision highp int;\n"
               "precision highp sampler2DArray;\n"
               "precision highp samplerCube;\n");
  }
  return out;
}

bool
Shader::createShaders()
{
  if (shouldUseGLES()) {
    const bool needs320 = geometryCode.has_value();
    vertexCode = rewriteToGLES(vertexCode, needs320);
    fragmentCode = rewriteToGLES(fragmentCode, needs320);
    if (geometryCode.has_value()) {
      geometryCode = rewriteToGLES(geometryCode.value(), true);
    }
  }

  auto vertexSuccess = createAndCompileShader(GL_VERTEX_SHADER, vertexCode);
  auto fragmentSuccess = createAndCompileShader(GL_FRAGMENT_SHADER, fragmentCode);
  std::optional<bool> geometrySuccess = std::nullopt;
  if (geometryCode.has_value()) {
    spdlog::debug("creating geometry shader");
    geometrySuccess = createAndCompileShader(GL_GEOMETRY_SHADER, geometryCode.value());
  }
  if(vertexSuccess && fragmentSuccess && (!geometrySuccess.has_value() || geometrySuccess)) {
    return true;
  } else {
    return false;
  }
}

void
Shader::deleteShaders()
{
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  if (geometryCode.has_value()) {
    glDeleteShader(geometry);
  }
}

void
Shader::loadCode(std::string vertexPath,
                 std::string fragmentPath,
                 std::optional<std::string> geometryPath)
{
  vertexCode = retrieveShaderCode(vertexPath);
  fragmentCode = retrieveShaderCode(fragmentPath);
  if (geometryPath.has_value()) {
    geometryCode = retrieveShaderCode(geometryPath.value());
  }
  if (vertexCode == "" || fragmentCode == "" ||
      (geometryPath.has_value() && geometryCode.value() == "")) {
    spdlog::error("SHADER: failed to initialize");
    return;
  }
}

Shader::Shader(std::string vertexPath, std::string fragmentPath):
vertexPath(vertexPath), fragmentPath(fragmentPath), geometryPath(std::nullopt)
{
  cacheLastTimeSourceCodeChangedOnDisk();
  loadCode(vertexPath, fragmentPath);
  createShaderProgram();
}

Shader::Shader(std::string vertexPath,
               std::string geometryPath,
               std::string fragmentPath):
               vertexPath(vertexPath), 
               fragmentPath(fragmentPath),
               geometryPath(geometryPath)
{
  cacheLastTimeSourceCodeChangedOnDisk();
  loadCode(vertexPath, fragmentPath, geometryPath);
  createShaderProgram();
}

Shader::~Shader() {
  if (ID != 0) {
    glDeleteProgram(ID);
  }
}

void
Shader::createShaderProgram()
{
  auto createSuccess = createShaders();
  if(createSuccess) {
    linkShaderProgram();
  }
  deleteShaders();
}

void Shader::cacheLastTimeSourceCodeChangedOnDisk() {
  std::error_code ec;
  lastVertexChange = fs::last_write_time(vertexPath, ec);
  lastFragmentChange = fs::last_write_time(fragmentPath,ec);
  if(geometryPath) {
    lastGeometryChange = fs::last_write_time(geometryPath.value(), ec);
  }
}

bool Shader::sourceCodeChanged() {
  bool changed = false;
  std::error_code ec;
  changed = changed || lastVertexChange != fs::last_write_time(vertexPath, ec);
  changed = changed || lastFragmentChange != fs::last_write_time(fragmentPath, ec);
  if(geometryPath) {
    changed = changed || lastGeometryChange != fs::last_write_time(geometryPath.value(), ec);
  }
  return changed && !ec;
}

void
Shader::reloadIfChanged()
{
  // At most a few checks per second instead of one per frame.
  const auto now = std::chrono::steady_clock::now();
  if (lastHotReloadCheck.time_since_epoch().count() != 0 &&
      now - lastHotReloadCheck < std::chrono::milliseconds(500)) {
    return;
  }
  lastHotReloadCheck = now;

  if(sourceCodeChanged()) {
    // source code changed, so lets cashe the time
    cacheLastTimeSourceCodeChangedOnDisk();
    loadCode(vertexPath, fragmentPath, geometryPath);
    createShaderProgram();
    restoreUniforms();
  }
}

void
Shader::restoreUniforms() const
{
  if (ID == 0) {
    return;
  }

  GLint previousProgram = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
  glUseProgram(ID);

  for (const auto& [name, value] : boolUniforms) {
    const_cast<Shader*>(this)->setBool(name, value);
  }
  for (const auto& [name, value] : intUniforms) {
    const_cast<Shader*>(this)->setInt(name, value);
  }
  for (const auto& [name, value] : floatUniforms) {
    const_cast<Shader*>(this)->setFloat(name, value);
  }
  for (const auto& [name, value] : vec3Uniforms) {
    const_cast<Shader*>(this)->setVec3(name, value);
  }
  for (const auto& [name, value] : mat4Uniforms) {
    const_cast<Shader*>(this)->setMatrix4(name, value);
  }
  for (const auto& [name, value] : mat3Uniforms) {
    const_cast<Shader*>(this)->setMatrix3(name, value);
  }

  glUseProgram(previousProgram);
}

GLint
Shader::uniformLocation(const std::string& name) const
{
  auto it = uniformLocations.find(name);
  if (it != uniformLocations.end()) {
    return it->second;
  }
  const GLint location = glGetUniformLocation(ID, name.c_str());
  uniformLocations.emplace(name, location);
  return location;
}

void
Shader::use()
{
  glUseProgram(ID);
}

void
Shader::setBool(const std::string& name, bool value)
{
  boolUniforms[name] = value;
  glUniform1i(uniformLocation(name), (int)value);
}
void
Shader::setInt(const std::string& name, int value)
{
  intUniforms[name] = value;
  glUniform1i(uniformLocation(name), value);
}
void
Shader::setFloat(const std::string& name, float value)
{
  floatUniforms[name] = value;
  glUniform1f(uniformLocation(name), value);
}

void
Shader::setMatrix4(const std::string& name, const glm::mat4& value)
{
  mat4Uniforms[name] = value;
  glUniformMatrix4fv(
    uniformLocation(name), 1, GL_FALSE, &value[0][0]);
}

void
Shader::setMatrix3(const std::string& name, const glm::mat3& value)
{
  mat3Uniforms[name] = value;
  glUniformMatrix3fv(
    uniformLocation(name), 1, GL_FALSE, &value[0][0]);
}

void
Shader::setVec3(const std::string& name, const glm::vec3& value)
{
  vec3Uniforms[name] = value;
  glUniform3fv(uniformLocation(name), 1, &value[0]);
}
