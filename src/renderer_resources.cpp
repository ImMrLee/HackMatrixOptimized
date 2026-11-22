#include "renderer.h"
#include "renderer_internal.h"

#include <glad/glad.h>
#include <glm/glm.hpp>

void
Renderer::genMeshResources()
{
  MESH_VERTEX.create();
  MESH_VERTEX_POSITIONS.create(GL_ARRAY_BUFFER);
  MESH_VERTEX_TEX_COORDS.create(GL_ARRAY_BUFFER);
  MESH_VERTEX_BLOCK_TYPES.create(GL_ARRAY_BUFFER);
  MESH_VERTEX_SELECTS.create(GL_ARRAY_BUFFER);
}

void
Renderer::genDynamicObjectResources()
{
  DYNAMIC_OBJECT_VERTEX.create();
  DYNAMIC_OBJECT_POSITIONS.create(GL_ARRAY_BUFFER);
  DYNAMIC_OBJECT_COLORS.create(GL_ARRAY_BUFFER);
};

void
Renderer::genGlResources()
{
  APP_VAO.create();
  APP_VBO.create(GL_ARRAY_BUFFER);

  DIRECT_RENDER_VAO.create();
  DIRECT_RENDER_VBO.create(GL_ARRAY_BUFFER);

  CURSOR_VAO.create();
  CURSOR_VBO.create(GL_ARRAY_BUFFER);

  LINE_VAO.create();
  LINE_VBO.create(GL_ARRAY_BUFFER);
  LINE_INSTANCE.create(GL_ARRAY_BUFFER);

  genMeshResources();
  genDynamicObjectResources();
}

void
Renderer::setupMeshVertexAttributePoiners()
{
  glBindVertexArray(MESH_VERTEX);
  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_POSITIONS);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_TEX_COORDS);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_SELECTS);
  glVertexAttribIPointer(2, 1, GL_INT, sizeof(int), (void*)0);
  glEnableVertexAttribArray(2);

  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_BLOCK_TYPES);
  glVertexAttribIPointer(3, 1, GL_INT, sizeof(int), (void*)0);
  glEnableVertexAttribArray(3);
}

void
Renderer::setupDynamicObjectVertexAttributePointers()
{
  glBindVertexArray(DYNAMIC_OBJECT_VERTEX);
  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_POSITIONS);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_COLORS);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(1);
}

void
Renderer::setupVertexAttributePointers()
{
  setupMeshVertexAttributePoiners();
  setupDynamicObjectVertexAttributePointers();

  glBindVertexArray(APP_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, APP_VBO);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  // texture coord attribute
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // direct render
  glBindVertexArray(DIRECT_RENDER_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, DIRECT_RENDER_VBO);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  // texture coord attribute
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // line
  glBindVertexArray(LINE_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, LINE_VBO);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, LINE_INSTANCE);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(1);

  // software cursor quad (screen-space)
  glBindVertexArray(CURSOR_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, CURSOR_VBO);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
}

int MAX_CUBES = 1000000;

void
Renderer::fillDynamicObjectBuffers()
{
  int CUBES = 1000000;
  int VERTS = CUBES * 36;
  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_POSITIONS);
  glBufferData(
    GL_ARRAY_BUFFER, (sizeof(glm::vec3) * VERTS), (void*)0, GL_DYNAMIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, DYNAMIC_OBJECT_COLORS);
  glBufferData(
               GL_ARRAY_BUFFER, (sizeof(glm::vec3) * VERTS), (void*)0, GL_DYNAMIC_DRAW);
}

void
Renderer::fillBuffers()
{
  glBindBuffer(GL_ARRAY_BUFFER, APP_VBO);
  glBufferData(
    GL_ARRAY_BUFFER, sizeof(appVertices), appVertices, GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, DIRECT_RENDER_VBO);
  glBufferData(GL_ARRAY_BUFFER,
               sizeof(directRenderQuad),
               directRenderQuad,
               GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, LINE_VBO);
  glBufferData(
    GL_ARRAY_BUFFER, (sizeof(glm::vec3) * 200000), (void*)0, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, LINE_INSTANCE);
  glBufferData(
    GL_ARRAY_BUFFER, (sizeof(glm::vec3) * 200000), (void*)0, GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_POSITIONS);
  glBufferData(GL_ARRAY_BUFFER,
               (sizeof(glm::vec3) * 36 * MAX_CUBES),
               (void*)0,
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_TEX_COORDS);
  glBufferData(GL_ARRAY_BUFFER,
               (sizeof(glm::vec2) * 36 * MAX_CUBES),
               (void*)0,
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_BLOCK_TYPES);
  glBufferData(
    GL_ARRAY_BUFFER, (sizeof(int) * 36 * MAX_CUBES), (void*)0, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, MESH_VERTEX_SELECTS);
  glBufferData(
    GL_ARRAY_BUFFER, (sizeof(int) * 36 * MAX_CUBES), (void*)0, GL_DYNAMIC_DRAW);

  fillDynamicObjectBuffers();
}
