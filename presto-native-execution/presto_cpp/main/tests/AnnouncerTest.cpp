/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "presto_cpp/main/Announcer.h"
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include "presto_cpp/main/tests/HttpServerWrapper.h"

namespace fs = boost::filesystem;
using namespace facebook::presto;

namespace {

std::string getCertsPath(const std::string& fileName) {
  std::string currentPath = fs::current_path().c_str();
  if (boost::algorithm::ends_with(currentPath, "fbcode")) {
    return currentPath +
        "/github/presto-trunk/presto-native-execution/presto_cpp/main/tests/certs/" +
        fileName;
  }

  // CLion runs the tests from cmake-build-release/ or cmake-build-debug/
  // directory. Hard-coded json files are not copied there and test fails with
  // file not found. Fixing the path so that we can trigger these tests from
  // CLion.
  boost::algorithm::replace_all(currentPath, "cmake-build-release/", "");
  boost::algorithm::replace_all(currentPath, "cmake-build-debug/", "");

  return currentPath + "/certs/" + fileName;
}

template <typename T>
struct PromiseHolder {
  explicit PromiseHolder(folly::Promise<T> promise)
      : promise_(std::move(promise)) {}

  folly::Promise<T>& get() {
    return promise_;
  }

 private:
  folly::Promise<T> promise_;
};

static std::unique_ptr<http::HttpServer> createHttpServer(bool useHttps) {
  if (useHttps) {
    std::string certPath = getCertsPath("test_cert1.pem");
    std::string keyPath = getCertsPath("test_key1.pem");
    std::string ciphers = "AES128-SHA,AES128-SHA256,AES256-GCM-SHA384";
    auto httpsConfig = std::make_unique<http::HttpsConfig>(
        folly::SocketAddress("127.0.0.1", 0), certPath, keyPath, ciphers);
    return std::make_unique<http::HttpServer>(nullptr, std::move(httpsConfig));
  } else {
    return std::make_unique<http::HttpServer>(
        std::make_unique<http::HttpConfig>(
            folly::SocketAddress("127.0.0.1", 0)));
  }
}

std::unique_ptr<facebook::presto::test::HttpServerWrapper> makeDiscoveryServer(
    std::function<void()> onAnnouncement,
    bool useHttps) {
  auto httpServer = createHttpServer(useHttps);

  httpServer->registerPut(
      R"(/v1/announcement/(.+))",
      [onAnnouncement](
          proxygen::HTTPMessage* /*message*/,
          const std::vector<std::unique_ptr<folly::IOBuf>>& /*body*/,
          proxygen::ResponseHandler* downstream) mutable {
        onAnnouncement();
        proxygen::ResponseBuilder(downstream)
            .status(http::kHttpAccepted, "Accepted")
            .sendWithEOM();
      });
  return std::make_unique<facebook::presto::test::HttpServerWrapper>(
      std::move(httpServer));
}

} // namespace

class AnnouncerTestSuite : public ::testing::TestWithParam<bool> {};

TEST_P(AnnouncerTestSuite, basic) {
  const bool useHttps = GetParam();
  auto [promise, future] = folly::makePromiseContract<bool>();

  std::atomic_int announcementCnt(0);
  auto onAnnouncement = [&announcementCnt,
                         promiseHolder = std::make_shared<PromiseHolder<bool>>(
                             std::move(promise))]() {
    if (++announcementCnt == 5) {
      promiseHolder->get().setValue(true);
    }
  };

  auto discoveryServer = makeDiscoveryServer(onAnnouncement, useHttps);
  auto serverAddress = discoveryServer->start().get();

  std::atomic_int addressLookupCnt(0);
  auto addressLookup = [&]() mutable {
    auto prevCnt = addressLookupCnt++;
    if (prevCnt < 3) {
      return serverAddress;
    }
    if (prevCnt == 3) {
      // Simulate failure to reach discovery server
      discoveryServer->stop();
      return serverAddress;
    }
    if (prevCnt < 6) {
      // Simulate failure to get the discovery server address
      throw std::runtime_error("Server is down");
    }
    if (prevCnt == 6) {
      discoveryServer = makeDiscoveryServer(onAnnouncement, useHttps);
      serverAddress = discoveryServer->start().get();
    }
    return serverAddress;
  };

  std::string keyPath = useHttps ? getCertsPath("client_ca.pem") : "";
  std::string ciphers =
      useHttps ? "ECDHE-ECDSA-AES256-GCM-SHA384,AES256-GCM-SHA384" : "";

  Announcer announcer(
      "127.0.0.1",
      useHttps,
      1234,
      addressLookup,
      "testversion",
      "testing",
      "test-node",
      "test-node-location",
      {"hive", "tpch"},
      100 /*milliseconds*/,
      keyPath,
      ciphers);

  announcer.start();
  ASSERT_TRUE(std::move(future).getTry().hasValue());
  ASSERT_GE(addressLookupCnt, 8);
  announcer.stop();
}

INSTANTIATE_TEST_CASE_P(
    AnnouncerTest,
    AnnouncerTestSuite,
    ::testing::Values(true, false));
