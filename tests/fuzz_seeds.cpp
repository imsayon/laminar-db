#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "laminar/coding.h"
#include "storage/format.h"
#include "storage_test_support.h"
#include "v1_fixtures.h"

int main(int argc, char** argv) try {
  using namespace laminar;
  using namespace laminar::storage;
  using namespace laminar::test;
  LAMINAR_CHECK_EQ(argc, 2);
  const std::filesystem::path directory = argv[1];
  std::filesystem::create_directories(directory);
  std::vector<std::string> seeds{std::string(kFresh.substr(0, kFileHeaderBytes)),
                                 std::string(kFresh.substr(kFileHeaderBytes)),
                                 std::string(kMixed.substr(kFresh.size())),
                                 std::string(kCheckpoint.substr(kFileHeaderBytes, 45))};
  const Operation empty{Tag::Put, {}, {}};
  seeds.push_back(EncodeFrame(Kind::Mutation, 1, std::span(&empty, 1)).value());
  const std::vector<Operation> many(kMaxOperations, empty);
  seeds.push_back(EncodeFrame(Kind::Mutation, 1, many).value());
  seeds.push_back(std::string(kMixed.substr(kFresh.size(), kFrameHeaderBytes)));
  for (const size_t cut : {size_t{1}, size_t{31}, size_t{32}, size_t{53}}) {
    seeds.emplace_back(kMixed.substr(kFresh.size(), cut));
  }
  auto invalid = seeds[2];
  std::string length;
  PutFixed32(&length, static_cast<uint32_t>(kMaxPayload));
  invalid.replace(4, 4, length);
  seeds.push_back(invalid);
  invalid[32] = 3;
  seeds.push_back(invalid);
  for (size_t i = 0; i < seeds.size(); ++i) {
    const auto path = directory / std::to_string(i);
    Write(path, seeds[i]);
    LAMINAR_CHECK_EQ(Read(path), seeds[i]);
  }
  std::cout << seeds.size() << " deterministic decoder seeds\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
