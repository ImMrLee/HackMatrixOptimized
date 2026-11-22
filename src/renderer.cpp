#include "IndexPool.h"
#include "glm/ext/matrix_transform.hpp"
#include "model.h"
#include "components/Light.h"
#include "systems/Intersections.h"
#include "texture.h"
#include "renderer.h"
#include "renderer_internal.h"
#include "shader.h"
#include "AppSurface.h"
#include "TypedKeyOverlay.h"
#include "wayland_app.h"
#include "camera.h"
#include "screen.h"
#include "components/Bootable.h"
#include "time_utils.h"
#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/round.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "tracy/TracyOpenGL.hpp"
#include "tracy/Tracy.hpp"

#ifdef WLROOTS_DEBUG_LOGS
constexpr bool kWlrootsDebugLogs = true;
#else
constexpr bool kWlrootsDebugLogs = false;
#endif

static FILE*
wlroots_renderer_log()
{
  if constexpr (kWlrootsDebugLogs) {
    static FILE* f = std::fopen("/tmp/matrix-wlroots-renderer.log", "a");
    return f ? f : stderr;
  }
  return nullptr;
}

#define WL_RENDERER_LOG(...)                                                      \
  do {                                                                            \
    if (kWlrootsDebugLogs) {                                                      \
      FILE* f = wlroots_renderer_log();                                           \
      if (f) {                                                                    \
        std::fprintf(f, __VA_ARGS__);                                             \
        std::fflush(f);                                                           \
      }                                                                           \
    }                                                                             \
  } while (0)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#include <ctime>
#include <cmath>
#include <iomanip>
#include <cstring>
#include <cstdio>

static bool
gl_version_at_least(int wantMajor, int wantMinor)
{
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (!version) {
    return false;
  }
  // Handles strings like "OpenGL ES 3.2 ...", "OpenGL ES 3.0", "3.3.0"
  int major = 0;
  int minor = 0;
  if (strstr(version, "OpenGL ES")) {
    std::sscanf(version, "%*s %*s %d.%d", &major, &minor);
  } else {
    std::sscanf(version, "%d.%d", &major, &minor);
  }
  return (major > wantMajor) || (major == wantMajor && minor >= wantMinor);
}

static bool
is_gles()
{
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  return version && std::strstr(version, "OpenGL ES");
}
#define DISABLE_CULLING true

constexpr int MAX_LIGHTS = 5;

namespace {
template<size_t N>
std::array<std::string, N>
makeIndexedUniformNames(const char* prefix)
{
  std::array<std::string, N> names;
  for (size_t i = 0; i < N; ++i) {
    names[i] = std::string(prefix) + "[" + std::to_string(i) + "]";
  }
  return names;
}

// Pre-built uniform names: lightUniforms() runs every frame, so building these
// with std::to_string + concatenation per call was hot-path churn.
const auto kLightPositionNames = makeIndexedUniformNames<MAX_LIGHTS>("lightPos");
const auto kLightColorNames = makeIndexedUniformNames<MAX_LIGHTS>("lightColor");
const auto kFarPlaneNames = makeIndexedUniformNames<MAX_LIGHTS>("far_plane");
const auto kShadowMatrixNames = makeIndexedUniformNames<6>("shadowMatrices");
const auto kDepthCubeMapNames = makeIndexedUniformNames<MAX_LIGHTS>("depthCubeMap");
} // namespace

void
Renderer::toggleWireframe()
{
  if (isWireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); //  normal
    isWireframe = false;
  } else {
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); // wireframe
    isWireframe = true;
  }
}

Renderer::Renderer(shared_ptr<EntityRegistry> registry,
                   Camera* camera,
                   World* world,
                   std::shared_ptr<TypedKeyOverlay> typedKeyOverlay,
                   shared_ptr<blocks::TexturePack> texturePack,
                   bool invertY)
  : texturePack(texturePack)
  , registry(registry)
  , typedKeyOverlay(std::move(typedKeyOverlay))
  , invertY(invertY)
{
  this->camera = camera;
  this->world = world;

  logger = make_shared<spdlog::logger>("Renderer", fileSink);
  logger->set_level(spdlog::level::info);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  // glEnable(GL_MULTISAMPLE);
  genGlResources();
  fillBuffers();
  // Align global screen dimensions to the actual GL viewport (Wayland context)
  // so default app sizing (Bootable defaults) matches the compositor output,
  // then rebuild the app quad with the correct aspect.
  auto refreshScreenFromViewport = [&]() {
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0) {
      SCREEN_WIDTH = static_cast<float>(vp[2]);
      SCREEN_HEIGHT = static_cast<float>(vp[3]);
      int defaultW =
        std::max(1, static_cast<int>(std::lround(SCREEN_WIDTH * 0.85f)));
      int defaultH =
        std::max(1, static_cast<int>(std::lround(SCREEN_HEIGHT * 0.85f)));
      Bootable::DEFAULT_WIDTH = defaultW;
      Bootable::DEFAULT_HEIGHT = defaultH;
      float newHeight = SCREEN_HEIGHT / SCREEN_WIDTH / 2.0f;
      float verts[] = {
        -0.5f, -newHeight, 0, 0.0f, 0.0f, 0.5f,  -newHeight, 0, 1.0f, 0.0f,
        0.5f,  newHeight,  0, 1.0f, 1.0f, 0.5f,  newHeight,  0, 1.0f, 1.0f,
        -0.5f, newHeight,  0, 0.0f, 1.0f, -0.5f, -newHeight, 0, 0.0f, 0.0f,
      };
      std::memcpy(appVertices, verts, sizeof(appVertices));
      glBindBuffer(GL_ARRAY_BUFFER, APP_VBO);
      glBufferData(
        GL_ARRAY_BUFFER, sizeof(appVertices), appVertices, GL_STATIC_DRAW);
      FILE* f = std::fopen("/tmp/matrix-debug.log", "a");
      if (f) {
        std::fprintf(f,
                     "Renderer: viewport=%d x %d -> SCREEN=%d x %d DEFAULT=%d x "
                     "%d newHeight=%.6f\n",
                     vp[2],
                     vp[3],
                     (int)SCREEN_WIDTH,
                     (int)SCREEN_HEIGHT,
                     defaultW,
                     defaultH,
                     newHeight);
        std::fflush(f);
        std::fclose(f);
      }
    }
  };
  refreshScreenFromViewport();
  setupVertexAttributePointers();

  std::vector<std::string> images = texturePack->imageNames();
  textures.emplace("allBlocks", std::make_unique<Texture>(images, GL_TEXTURE0));
  cameraShader = std::make_unique<Shader>("shaders/vertex.glsl", "shaders/fragment.glsl");
  shadowsEnabled = gl_version_at_least(3, 2);
  if (!shadowsEnabled) {
    logger->warn("Disabling shadows: GL version too low for geometry shader");
  } else {
    depthShader = std::make_unique<Shader>("shaders/depthVertex.glsl",
                                            "shaders/depthGeometry.glsl",
                                            "shaders/depthFragment.glsl");
  }

  shader = cameraShader.get();

  shader->use(); // may need to move into loop to use changing uniforms

  shader->setInt("allBlocks", 0);
  // apps are on texture unit 0
  shader->setInt("appTex", 0);
  shader->setInt("totalBlockTypes", images.size());
  shader->setBool("SHADOWS_ENABLED", shadowsEnabled);

  cursorShader = std::make_unique<Shader>("shaders/cursor.vert", "shaders/cursor.frag");

  shader->setBool("lookedAtValid", false);
  shader->setBool("isLookedAt", false);
  shader->setBool("isMesh", false);
  shader->setBool("isModel", false);
  shader->setBool("directRender", false);
  shader->setBool("isVoxel", false);
  shader->setBool("voxelsEnabled", voxelsEnabled);
  shader->setFloat("uAmbientStrength", 0.001);

  voxelSpace.add(glm::vec3(0, 4, 4), voxelSize);
  voxelSpace.add(glm::vec3(voxelSize, 4, 4), voxelSize);
  voxelSpace.add(glm::vec3(-voxelSize, 4, 4), voxelSize);
  voxelSpace.add(glm::vec3(0, 4 + voxelSize, 4), voxelSize);
  voxelSpace.add(glm::vec3(0, 4 - voxelSize, 4), voxelSize);
  voxelMesh = voxelSpace.render();

  if (!is_gles()) {
    if (isWireframe) {
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); // wireframe
    } else {
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); //  normal
    }
  }
  glClearColor(163.0 / 255.0, 163.0 / 255.0, 167.0 / 255.0, 1.0f);
  if (!is_gles()) {
    glLineWidth(10.0);
  } else {
    glLineWidth(1.0f);
  }
}

void
Renderer::initCursorResources()
{
  if (cursorInitialized) {
    return;
  }
  // Generate a simple fractal tree into a small RGBA texture.
  constexpr int kCursorSize = 64;
  std::vector<uint32_t> pixels(kCursorSize * kCursorSize, 0x00000000);
  // Dark burgundy with full opacity for legibility on light backgrounds.
  constexpr uint32_t kCursorColor = 0x5a1020ff; // RGBA

  auto set_px = [&](int x, int y, uint32_t color) {
    if (x >= 0 && x < kCursorSize && y >= 0 && y < kCursorSize) {
      pixels[y * kCursorSize + x] = color;
    }
  };
  auto draw_line = [&](int x0, int y0, int x1, int y1, uint32_t color) {
    // Thicken strokes by writing a small square around each point.
    auto stamp = [&](int px, int py) {
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          set_px(px + dx, py + dy, color);
        }
      }
    };
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
      stamp(x0, y0);
      if (x0 == x1 && y0 == y1) {
        break;
      }
      int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
    }
  };

  std::function<void(int, int, float, float, int)> draw_branch =
    [&](int x, int y, float angle, float length, int depth) {
      if (depth <= 0 || length < 2.0f) {
        return;
      }
      int x2 = x + static_cast<int>(std::cos(angle) * length);
      int y2 = y + static_cast<int>(std::sin(angle) * length);
      draw_line(x, y, x2, y2, kCursorColor);
      float nextLen = length * 0.65f;
      float spread = 0.6f;
      draw_branch(x2, y2, angle - spread, nextLen, depth - 1);
      draw_branch(x2, y2, angle + spread, nextLen, depth - 1);
    };

  int baseX = kCursorSize / 2;
  int baseY = kCursorSize - 2;
  draw_branch(baseX, baseY, -glm::half_pi<float>(), static_cast<float>(kCursorSize) * 0.3f, 7);

  // Use the top of the drawn tree as the hotspot so the cursor is aimed with
  // its tip/top rather than its base. Prefer the first painted pixel in the
  // centre column; fall back to the topmost painted row for other art.
  int topRow = kCursorSize;
  for (int y = 0; y < kCursorSize; ++y) {
    if (pixels[y * kCursorSize + kCursorSize / 2] != 0) {
      topRow = y;
      break;
    }
  }
  if (topRow == kCursorSize) {
    for (int y = 0; y < kCursorSize && topRow == kCursorSize; ++y) {
      for (int x = 0; x < kCursorSize; ++x) {
        if (pixels[y * kCursorSize + x] != 0) {
          topRow = y;
          break;
        }
      }
    }
  }
  if (topRow < kCursorSize) {
    cursorHotspotYRatio = static_cast<float>(topRow) / kCursorSize;
  }

  glGenTextures(1, &cursorTexture);
  glBindTexture(GL_TEXTURE_2D, cursorTexture);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               kCursorSize,
               kCursorSize,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               pixels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  cursorShader->use();
  cursorShader->setInt("uTexture", 0);
  cursorInitialized = true;
}
void
Renderer::updateTransformMatrices()
{
  // Route camera transforms through Shader so hot reload/reset restores them.
  shader->setMatrix4("view", camera->getViewMatrix());
  shader->setMatrix4("projection", camera->getProjectionMatrix(true));
}

void
Renderer::updateChunkMeshBuffers(vector<shared_ptr<ChunkMesh>>& meshes)
{
  verticesInMesh = 0;
  for (auto mesh : meshes) {
    // if(mesh.updated) {
    glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_POSITIONS);
    glBufferSubData(GL_ARRAY_BUFFER,
                    sizeof(glm::vec3) * verticesInMesh,
                    sizeof(glm::vec3) * mesh->positions.size(),
                    mesh->positions.data());

    glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_TEX_COORDS);
    glBufferSubData(GL_ARRAY_BUFFER,
                    sizeof(glm::vec2) * verticesInMesh,
                    sizeof(glm::vec2) * mesh->texCoords.size(),
                    mesh->texCoords.data());

    glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_BLOCK_TYPES);
    glBufferSubData(GL_ARRAY_BUFFER,
                    sizeof(int) * verticesInMesh,
                    sizeof(int) * mesh->blockTypes.size(),
                    mesh->blockTypes.data());

    glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_SELECTS);
    glBufferSubData(GL_ARRAY_BUFFER,
                    sizeof(int) * verticesInMesh,
                    sizeof(int) * mesh->selects.size(),
                    mesh->selects.data());
    //}
    verticesInMesh += mesh->positions.size();
  }
}

void
Renderer::updateDynamicObjects(shared_ptr<DynamicObject> obj)
{
  auto objectSpace = dynamic_pointer_cast<DynamicObjectSpace>(obj);
  if (objectSpace == NULL) {
    auto renderable = obj->makeRenderable();
    glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_POSITIONS);
    glBufferSubData(GL_ARRAY_BUFFER,
                    0,
                    sizeof(glm::vec3) * renderable.vertices.size(),
                    renderable.vertices.data());

    glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_COLORS);
    glBufferSubData(GL_ARRAY_BUFFER,
                    0,
                    sizeof(glm::vec3) * renderable.colors.size(),
                    renderable.colors.data());
    verticesInDynamicObjects = renderable.vertices.size();
    return;
  }

  const auto& renderable = objectSpace->renderableCache();
  const auto upload = objectSpace->pendingUploadSpan();
  if (upload.vertexCount == 0 && !upload.fullRefresh) {
    return;
  }

  const size_t start = upload.fullRefresh ? 0 : upload.startVertex;
  const size_t count =
    upload.fullRefresh ? renderable.vertices.size() : upload.vertexCount;

  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_POSITIONS);
  glBufferSubData(GL_ARRAY_BUFFER,
                  sizeof(glm::vec3) * start,
                  sizeof(glm::vec3) * count,
                  renderable.vertices.data() + start);

  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_COLORS);
  glBufferSubData(GL_ARRAY_BUFFER,
                  sizeof(glm::vec3) * start,
                  sizeof(glm::vec3) * count,
                  renderable.colors.data() + start);

  verticesInDynamicObjects = renderable.vertices.size();
  objectSpace->clearPendingUpload();
}

void
Renderer::addLine(int index, Line line)
{
  glBindBuffer(GL_ARRAY_BUFFER, LINE_VBO);
  glBufferSubData(GL_ARRAY_BUFFER,
                  (sizeof(glm::vec3) * 2) * index,
                  sizeof(glm::vec3),
                  &line.points[0]);
  glBufferSubData(GL_ARRAY_BUFFER,
                  (sizeof(glm::vec3) * 2) * index + (sizeof(glm::vec3)),
                  sizeof(glm::vec3),
                  &line.points[1]);

  glBindBuffer(GL_ARRAY_BUFFER, LINE_INSTANCE);
  glBufferSubData(GL_ARRAY_BUFFER,
                  (sizeof(glm::vec3) * 2 * index),
                  (sizeof(glm::vec3)),
                  &line.color);
  glBufferSubData(GL_ARRAY_BUFFER,
                  (sizeof(glm::vec3) * 2 * index + sizeof(glm::vec3)),
                  (sizeof(glm::vec3)),
                  &line.color);
}

void
Renderer::setLines(const std::vector<Line>& lines)
{
  for (size_t i = 0; i < lines.size(); ++i) {
    addLine(static_cast<int>(i), lines[i]);
  }
}

void
Renderer::renderDynamicObjects()
{
  shader->setBool("isApp", false);
  shader->setBool("isModel", false);
  shader->setBool("isVoxel", false);
  shader->setBool("isDynamicObject", true);
  shader->setBool("directRender", false);
  shader->setBool("isLine", false);
  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_POSITIONS);
  glBindVertexArray(DYNAMIC_OBJECT_VERTEX);
  glDrawArrays(GL_TRIANGLES, 0, verticesInDynamicObjects);
  shader->setBool("isDynamicObject", false);
}

static void
readFramebufferToRgba(int width, int height, std::vector<unsigned char>& out)
{
  out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  GLint prevPack = 0;
  glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  GLenum readFmt =
#ifdef GL_BGRA
    GL_BGRA
#elif defined(GL_BGRA_EXT)
    GL_BGRA_EXT
#else
    GL_RGBA
#endif
    ;
  bool swapRB = (readFmt != GL_RGBA);

  glReadPixels(0, 0, width, height, readFmt, GL_UNSIGNED_BYTE, out.data());

  if (swapRB) {
    size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < total; ++i) {
      auto* px = out.data() + i * 4;
      std::swap(px[0], px[2]);
    }
  }
  glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
}

void
Renderer::screenshotFromCurrentFramebuffer(int width, int height, unsigned int fbo)
{
  GLint prevReadFbo = 0;
  GLint prevDrawFbo = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
  if (fbo == 0 && currentFbo != 0) {
    fbo = currentFbo;
  }
  if (fbo != 0) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLint>(fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLint>(fbo));
#ifdef GL_COLOR_ATTACHMENT0
    glReadBuffer(GL_COLOR_ATTACHMENT0);
#endif
  }

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  stringstream filenameSS;
  filenameSS << "screenshots/" << std::put_time(&tm, "%d-%m-%Y %H-%M-%S.png");

  string filename = filenameSS.str();

  std::vector<unsigned char> data;
  readFramebufferToRgba(width, height, data);
  std::thread saver([filename, width, height, data = std::move(data)]() mutable {
    int channels = 4; // 4 for RGBA
    // Do not flip here; wlr path already reads from the onscreen-oriented FBO.
    stbi_flip_vertically_on_write(false);
    stbi_write_png(
      filename.c_str(), width, height, channels, data.data(), width * channels);
  });
  // Restore previous framebuffer bindings.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
  saver.detach();
}

void
Renderer::renderSoftwareCursor(float xPixels, float yPixels, float sizePixels)
{
  initCursorResources();
  cursorShader->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, cursorTexture);

  // Center the compositor-owned cursor quad on the pointer hotspot so the
  // drawn cursor matches hit-testing coordinates.
  glm::vec2 mapped = mapCursorToScreen(xPixels, yPixels);
  // Aim with the top of the generated tree (the click point should be the
  // visible top edge, not the trunk at the bottom).
  float hotspotX = sizePixels * 0.5f;
  float hotspotY = sizePixels * cursorHotspotYRatio;
  float drawX = mapped.x - hotspotX;
  float drawY = mapped.y - hotspotY;
  float yFlipped = SCREEN_HEIGHT - drawY - sizePixels;
  float left = static_cast<float>(drawX) / SCREEN_WIDTH * 2.0f - 1.0f;
  float right =
    static_cast<float>(drawX + sizePixels) / SCREEN_WIDTH * 2.0f - 1.0f;
  float top = 1.0f - static_cast<float>(yFlipped) / SCREEN_HEIGHT * 2.0f;
  float bottom =
    1.0f - static_cast<float>(yFlipped + sizePixels) / SCREEN_HEIGHT * 2.0f;

  float verts[] = {
    left,  bottom, 0.0f, 0.0f, 1.0f, right, bottom, 0.0f, 1.0f, 1.0f,
    right, top,    0.0f, 1.0f, 0.0f, left,  top,    0.0f, 0.0f, 0.0f,
    left,  bottom, 0.0f, 0.0f, 1.0f, right, top,    0.0f, 1.0f, 0.0f
  };

  glBindVertexArray(CURSOR_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, CURSOR_VBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

  GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
  glDisable(GL_DEPTH_TEST);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  if (depthEnabled) {
    glEnable(GL_DEPTH_TEST);
  }
}

glm::vec2
Renderer::mapCursorToScreen(float xPixels, float yPixels) const
{
  // Use raw output pixel coordinates so drawn cursor matches input hit-testing.
  return glm::vec2(xPixels, yPixels);
}
void
Renderer::updateShaderUniforms()
{
  shader->setFloat("time", nowSeconds());
  shader->setBool("isApp", false);
  shader->setBool("isLine", false);
  shader->setBool("isVoxel", false);
  shader->setBool("voxelsEnabled", voxelsEnabled);
}

void
Renderer::renderChunkMesh()
{
  shader->setBool("isMesh", true);
  glBindVertexArray(MESH_VERTEX);
  // TODO: fix this
  // glEnable(GL_CULL_FACE);
  glDisable(GL_CULL_FACE);
  glDrawArrays(GL_TRIANGLES, 0, verticesInMesh);
  shader->setBool("isMesh", false);
}

void
Renderer::renderLines()
{
  shader->setBool("isApp", false);
  shader->setBool("isModel", false);
  shader->setBool("isVoxel", false);
  shader->setBool("isDynamicObject", false);
  shader->setBool("isLine", true);
  // Draw lines on top; depth test off for clear visibility of selection boxes.
  GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
  if (depthEnabled) {
    glDisable(GL_DEPTH_TEST);
  }
  glLineWidth(2.5f);
  glBindVertexArray(LINE_VAO);
  glDrawArrays(GL_LINES, 0, world->getLines().size() * 2);
  if (depthEnabled) {
    glEnable(GL_DEPTH_TEST);
  }
  glLineWidth(1.0f);
  shader->setBool("isLine", false);
}

void
Renderer::renderVoxels()
{
  if (!voxelsEnabled) {
    return;
  }
  shader->setBool("isVoxel", true);
  shader->setMatrix4("model", glm::mat4(1.0f));
  glDisable(GL_CULL_FACE);
  voxelMesh.draw();
  shader->setBool("isVoxel", false);
}

void
Renderer::addVoxels(const std::vector<glm::vec3>& positions,
                    bool replace,
                    float size,
                    glm::vec3 color)
{
  if (size > 0.0f) {
    voxelSize = size;
  }
  if (replace) {
    voxelSpace.clear();
  }
  if (positions.empty() && replace) {
    voxelMesh = RenderedVoxelSpace();
    voxelsEnabled = false;
    return;
  }
  for (const auto& pos : positions) {
    voxelSpace.add(pos, size, color);
  }
  voxelMesh = voxelSpace.render();
  voxelsEnabled = true;
}

void
Renderer::clearVoxelsInBox(const glm::vec3& minCorner,
                           const glm::vec3& maxCorner)
{
  auto removed = voxelSpace.remove(minCorner, maxCorner);
  if (removed == 0) {
    return;
  }
  voxelMesh = voxelSpace.render();
  voxelsEnabled = !voxelSpace.empty();
}

bool
Renderer::voxelExistsAt(const glm::vec3& worldPosition, float size) const
{
  float s = size > 0.0f ? size : voxelSize;
  glm::vec3 pos = worldPosition;
  // Basic epsilon to tolerate float drift when comparing grid-aligned voxels.
  const float EPS = 0.0001f;
  glm::vec3 expected = glm::round(pos / s) * s;
  if (glm::length(pos - expected) > EPS) {
    pos = expected;
  }

  // VoxelSpace currently only exposes add/render; mirror the simple storage
  // here by iterating the backing container.
  return voxelSpace.has(pos, s);
}

void
Renderer::lightUniforms(RenderPerspective perspective,
                        std::optional<entt::entity> fromLight)
{
  auto lightView = registry->view<Light, Positionable>();
  int lightIndex = 0;
  for (auto [entity, light, positionable] : lightView.each()) {
    if (lightIndex < MAX_LIGHTS) {
      shader->setVec3(kLightPositionNames[lightIndex], positionable.pos);
      shader->setVec3(kLightColorNames[lightIndex], light.color);
      shader->setFloat(kFarPlaneNames[lightIndex], light.farPlane);
    }
    if (perspective == LIGHT && fromLight == entity) {
      shader->setInt("fromLightIndex", lightIndex);
      for (unsigned int i = 0; i < 6; ++i) {
        shader->setMatrix4(kShadowMatrixNames[i], light.shadowTransforms[i]);
      }
    }
    if (perspective == CAMERA && lightIndex < MAX_LIGHTS) {
      shader->setInt(kDepthCubeMapNames[lightIndex], light.textureUnit);
    }

    // THIS IS A HACK. If I exceed 5 lights, use texture array (proper)
    for (int i = lightIndex; i < MAX_LIGHTS; ++i) {
      shader->setInt(kDepthCubeMapNames[i], light.textureUnit);
    }
    lightIndex++;
  }
  shader->setInt("numLights", lightIndex);
}

void
Renderer::renderModels(RenderPerspective perspective)
{
  TracyGpuZone("render models");
  if(perspective != LIGHT) {
    glEnable(GL_CULL_FACE);
  }
  auto frustum = camera->createFrustum();
  shader->setBool("isModel", true);
  shader->setVec3("viewPos", camera->position);
  shader->setBool("isLight", false);

  auto modelView = registry->view<Positionable, Model>();

  bool hasLight = false;

  set<entt::entity> lightEntities;
  auto lightView = registry->view<Light, Positionable>();
  for (auto [entity, light, positionable] : lightView.each()) {
    lightEntities.insert(entity);
  }

  static int lastCount = 0;
  int count = 0;
  for (auto [entity, p, m] : modelView.each()) {
    bool shouldDraw = systems::isOnFrustum(registry, entity, frustum);
    if(DISABLE_CULLING) {
      shouldDraw = true;
    }
    if (!shouldDraw) {
      continue;
    }
    if (!lightEntities.empty() && lightEntities.contains(entity)) {
      shader->setBool("isLight", true);
      if (perspective == LIGHT) {
        shouldDraw = false;
      }
    }
    auto normalMatrix = p.normalMatrix;
    auto modelMatrix = p.modelMatrix;
    shader->setMatrix3("normalMatrix", normalMatrix);
    shader->setMatrix4("model", modelMatrix);

    if (shouldDraw) {
      count++;
      m.Draw(*shader);
    }
    shader->setBool("isLight", false);
  }
  if (count != lastCount) {
    stringstream countSS;
    countSS << "model render count: " << count;
    logger->debug(countSS.str());
    lastCount = count;
  }
  shader->setBool("isModel", false);
}

void
Renderer::render(RenderPerspective perspective,
                 std::optional<entt::entity> fromLight)
{
  ZoneScoped;
  TracyGpuZone("render");
  auto allWl = registry->view<WaylandApp::Component, Positionable>();
  size_t wlCount = allWl.size_hint();
  WL_RENDERER_LOG("Renderer: frame WaylandApp count=%zu\n", wlCount);
  allWl.each([&](auto ent, WaylandApp::Component& comp, Positionable& pos) {
    WL_RENDERER_LOG("Renderer: Wayland app ent=%d pos=(%.2f,%.2f,%.2f) texId=%d surface=%p\n",
                    (int)entt::to_integral(ent),
                    pos.pos.x,
                    pos.pos.y,
                    pos.pos.z,
                    comp.app ? comp.app->getTextureId() : -1,
                    comp.app ? (void*)comp.app->getSurface() : nullptr);
  });
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  GLint boundFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
  currentFbo = static_cast<unsigned int>(boundFbo);
  glFrontFace(invertY ? GL_CW : GL_CCW);
  if (perspective == CAMERA) {
    shader = cameraShader.get();
    shader->reloadIfChanged();
    shader->use();
    // must use prior to updating uniforms

    camera->tick();
    updateTransformMatrices();
  } else {
    if (!shadowsEnabled || depthShader == nullptr) {
      return;
    }
    shader = depthShader.get();
    shader->reloadIfChanged();
    shader->use();
  }
  updateShaderUniforms();
  lightUniforms(perspective, fromLight);
  renderDynamicObjects();
  renderModels(perspective);
  if (perspective == CAMERA) {
    renderVoxels();
    renderApps();
    if (typedKeyOverlay) {
      typedKeyOverlay->render(cursorShader.get(),
                              SCREEN_WIDTH,
                              SCREEN_HEIGHT,
                              wm && wm->hasCurrentOrPendingFocus());
    }
  }
  //renderChunkMesh();
}

Camera*
Renderer::getCamera()
{
  return camera;
}

void
Renderer::toggleMeshing()
{
  logger->debug("toggleMeshing");
  logger->flush();
}

void
Renderer::wireWindowManager(WindowManager::WindowManagerPtr wm,
                            shared_ptr<WindowManager::Space> windowManagerSpace)
{
  this->wm = wm;
  this->windowManagerSpace = windowManagerSpace;
}

Renderer::~Renderer() = default;
