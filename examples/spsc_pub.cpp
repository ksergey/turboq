#include <iostream>
#include <string>

#include <fmt/format.h>

#include <turboq/BoundedSPSCRawQueue.h>
#include <turboq/utils.h>

int main(int argc, char* argv[]) {
  try {
    char const* queueName = "turboq.spsc";
    if (argc > 1) {
      queueName = argv[1];
    }

    auto producer = turboq::BoundedSPSCRawQueue(queueName, {5 * 1024 * 1024}).createProducer();

    for (std::string line; std::getline(std::cin, line);) {
      if (line.empty()) {
        continue;
      }

      auto result = producer.prepare(line.size());
      if (result.empty()) {
        throw std::runtime_error("failed to prepare buffer to send");
      }
      std::memcpy(result.data(), line.data(), line.size());
      producer.commit();
    }

  } catch (std::exception const& e) {
    fmt::print(stderr, "ERROR: {}\n", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
