// Example API-driven voxel app (F13): an animated voxel wave.
//
// Connects to the HackMatrix ZMQ API and replaces the API voxel set every frame
// with a moving surface. This is the minimal form of the "audio visualizer /
// map solver" idea from distro_ideas: an external program driving voxels without
// creating a Wayland window.
//
// Build: cmake -DMATRIX_BUILD_EXAMPLES=ON ... && ./build/voxel_demo
// Run:   ./voxel_demo [endpoint] [frames]
//        default endpoint tcp://127.0.0.1:3345, 60 frames

#include "protos/api.pb.h"
#include <zmq/zmq.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int
main(int argc, char** argv)
{
  const std::string endpoint = argc > 1 ? argv[1] : "tcp://127.0.0.1:3345";
  const int frames = argc > 2 ? std::atoi(argv[2]) : 60;

  zmq::context_t context(1);
  zmq::socket_t socket(context, zmq::socket_type::req);
  socket.set(zmq::sockopt::rcvtimeo, 3000);
  socket.set(zmq::sockopt::linger, 0);
  socket.connect(endpoint);

  int okFrames = 0;
  for (int frame = 0; frame < frames; ++frame) {
    ApiRequest request;
    request.set_type(MessageType::ADD_VOXELS);
    auto* add = request.mutable_addvoxels();
    add->set_replace(true); // animate: swap out the previous frame
    add->set_size(0.5f);

    const float t = static_cast<float>(frame) * 0.15f;
    for (int x = -6; x <= 6; ++x) {
      for (int y = -3; y <= 3; ++y) {
        const float wave = std::sin(x * 0.45f + t) + std::cos(y * 0.45f + t);
        auto* voxel = add->add_voxels();
        // Undulating sheet a few units in front of the default camera
        // (position ~(0, 1.5, 1.8), looking down -Z). The wave displaces Y so
        // it is visible head-on.
        voxel->set_x(x * 0.6f);
        voxel->set_y(y * 0.5f + 0.5f + wave * 0.5f);
        voxel->set_z(-8.0f);

        auto* color = add->add_colors();
        color->set_x(0.5f + 0.5f * std::sin(t + x * 0.2f));
        color->set_y(0.5f + 0.5f * std::cos(t + y * 0.2f));
        color->set_z(0.9f);
      }
    }

    std::string bytes;
    request.SerializeToString(&bytes);
    socket.send(zmq::buffer(bytes), zmq::send_flags::none);

    zmq::message_t reply;
    auto received = socket.recv(reply, zmq::recv_flags::none);
    ApiRequestResponse response;
    if (received &&
        response.ParseFromArray(reply.data(), static_cast<int>(reply.size())) &&
        response.success()) {
      ++okFrames;
    }
    if (frame % 10 == 0) {
      std::printf("frame %d: %d voxels, success=%d\n",
                  frame,
                  add->voxels_size(),
                  (received && response.success()) ? 1 : 0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::printf("done: %d/%d frames accepted\n", okFrames, frames);
  return okFrames == frames ? 0 : 1;
}
